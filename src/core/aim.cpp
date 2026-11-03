#include "core/aim.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/anchors.h"
#include "core/api.h"
#include "core/body.h"
#include "core/gestures.h"
#include "core/gun_melee.h"
#include "core/offhand.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/hands.h"
#include "core/d3d_hooks.h"
#include "core/dual.h"
#include "core/hooks.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/pose.h"
#include "core/reload.h"
#include "core/xinput.h"

namespace rdrvr::aim {
namespace {
std::atomic<uint64_t> g_player_shot_n{0};  // the player's shots (player_shots)

std::atomic<bool> g_barrel{true};  // [Hands] BarrelAim
// (2026-11-02, GOAL-offhand-changes C12b; round 67 check 5, the user: dead at the catch; the bisection, r97, cycle J5: the
// stranger dead in 2 of 3 throws with the lasso's ray from the hand, in none from the game's own origin with the mod's
// direction, nor with the game's own ray) the lasso's reticle ray keeps the game's origin, the mod's direction from it (1);
// "aim lassoray hand|game|off" (the session, a test's): 2 from the hand (as before), 0 the game's own ray
// (round 69, the user: they still die, and it is very hard to lasso anyone unless right in front of them: the game's origin
// neither stopped the kill nor aims as well: the hand's again, 2)
std::atomic<int> g_lasso_ray{2};
std::atomic<bool> g_fire{true};    // [Hands] FireInFirstPerson
std::atomic<bool> g_reticle{false};      // [Hands] Reticle (off by default)
std::atomic<float> g_reticle_deg{1.2f};  // [Hands] ReticleSize (degrees across)
std::atomic<bool> g_reticle_dot{false};    // [Hands] ReticleStyle=dot: a dot only
std::atomic<bool> g_ret_smooth{true};   // [Hands] ReticleSmooth (run 10 item 8)
std::atomic<uint64_t> g_reticle_shown{0}, g_reticle_offline{0};
std::atomic<bool> g_assist{false}; // [Hands] AimAssist (soft lock and the reticle magnet in first person)
std::atomic<bool> g_tracer{true};  // [Hands] TracerFromMuzzle
std::atomic<bool> g_perfect{true};   // [Aim] PerfectAccuracy (on by default since run 8)
std::atomic<bool> g_pattern{true};   // [Aim] ShotgunPattern
std::atomic<bool> g_spawn_hooked{false};
// hk_spawn: the muzzle-blocked flip (DoProbeCheck: W +0x9b1 = 1, +0x9b2 = 0) at the player's shots, whatever
// PerfectAccuracy is (its undo, (a) in hk_spawn, runs only with it on)
std::atomic<uint64_t> g_flips_seen{0}, g_flips_left{0};
std::atomic<uint64_t> g_npc_spawns{0}, g_npc_bloomed{0};  // others' shots (left to the game) and those with the game's bloom drawn
std::atomic<float> g_npc_sigma{0.0f};                      // the last such shot's bloom size
std::atomic<double> g_npc_log_ms{-1.0e9};                  // others' shots in the log: at most every 30 s
std::atomic<uint64_t> g_npc_logged{0};                     // the count at the last such line
void log_npc_shots(const char* when) {
    const uint64_t n = g_npc_spawns.load(std::memory_order_relaxed), m = g_npc_bloomed.load(std::memory_order_relaxed);
    g_npc_logged.store(n, std::memory_order_relaxed);
    log::info("[aim] others' shots%s: %llu, with the game's spread %llu (the last %.3f); perfect accuracy %d (the player's shots only)", when,
              static_cast<unsigned long long>(n), static_cast<unsigned long long>(m), static_cast<double>(g_npc_sigma.load()),
              g_perfect.load() ? 1 : 0);
}
std::atomic<uint64_t> g_spawns{0}, g_player_spawns{0}, g_bloom_zeroed{0}, g_block_fixes{0}, g_aligned{0}, g_speed_drops{0},
    g_straightened{0};
// one shot's pellets as hk_launch sees them inside the spawn (the game thread only): the first's numbers, the widest
thread_local bool t_in_spawn = false;
thread_local int t_pellets = 0;
thread_local double t_first_err = -1.0, t_max_err = -1.0;
thread_local float t_first_start[3] = {}, t_first_dir[3] = {}, t_first_speed = 0.0f;
thread_local bool t_first_fresh = false;
std::atomic<uint64_t> g_assist_off{0}, g_shots{0}, g_tracers{0};
std::atomic<double> g_last_override_ms{0.0};
// the player's last shot as launched: start, velocity direction, and the angle to the barrel ray then (degrees)
float g_shot_start[3] = {}, g_shot_dir[3] = {}, g_shot_err = -1;
std::atomic<uint64_t> g_rays{0}, g_overrides{0}, g_no_gun{0}, g_no_hand{0}, g_fires{0}, g_swaps{0}, g_hand_rays{0};
// item 1b: the aim ray's and barrel_ray's barrels from the drawn build's matrix (body::item_gun) or the live W +0x80
std::atomic<uint64_t> g_ray_built{0}, g_ray_live{0}, g_br_built{0}, g_br_live{0};
// the last override, for the status line (game thread writes, the test channel reads: torn reads are harmless)
float g_last_muzzle[3] = {}, g_last_dir[3] = {}, g_last_offset[3] = {};
// The reticle's own probe (2026-10-11, the user: "crosshair needs to be visible not just when aiming. Also needs to be
// visible in dead eye. Also it sometimes gets stuck red even when not aimed at an enemy"). Before, the reticle came
// only from the game's aim ray (hk_aim_ray, called while the game aims) and its target point and handle (stale outside
// it). Now each script tick the drawn barrel's ray is probed with CAMERA_PROBE 0x720F2CA7 (handler 0x140355c20: args
// [Result*][Sx Sy][Sz][Tx Ty][Tz][Owner][Flag], a by-value Vector3 packed in two slots; 1 on a hit and its point,
// tests/sim/r13_probe.py), its result read on the next tick. Red only while the game aims and its own target point
// agrees with the probe's hit.
constexpr float kProbeRange = 300.0f;
std::mutex g_rp_mutex;
struct ReticleProbe {
    float muzzle[3] = {}, dir[3] = {};
    float hand[3] = {};  // run 10 item 8: the drawn gun hand when the probe was asked (the muzzle moves with it since)
    bool hand_ok = false;
    float dist = 0.0f;
    bool hit = false;
    double ms = 0.0;
    int32_t tgt = 0;           // the game's target handle (targeting +0x3900) while it aims, and its GET_POSITION
    float tgt_pos[3] = {};
    double tgt_ms = 0.0;
    // the second gun's probe (g_rp2): the muzzle offset in the drawn gun's axes (the copy's shift in), the gun, its hand
    float mo[3] = {};
    uintptr_t W = 0;
    int john = -1;
    bool copy = false;
} g_rp;  // under g_rp_mutex
std::atomic<uint64_t> g_rp_probes{0}, g_rp_hits{0}, g_rp_lost{0}, g_rp_nogun{0}, g_rp_nobarrel{0}, g_rp_red{0}, g_rp_unred{0};
// the last barrel in the IK target's axes and the drawn wrist's, and the game's gun in its animated wrist's ("aim frames");
// the first also for the frame end (barrel_in_target: the loading point, two-handed aim)
float g_dir_target[3] = {}, g_dir_wrist[3] = {}, g_gun_anim[3] = {};
std::atomic<float> g_bt[3];
std::atomic<double> g_bt_ms{0.0};

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

uintptr_t call_get(uintptr_t fn, uintptr_t obj) {
    __try {
        return reinterpret_cast<uintptr_t (*)(uintptr_t)>(fn)(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// a script handle's actor (the object's +0xb0 handle in the actor pool, both generations checked); 0 none
uintptr_t actor_of_handle(uint32_t h) {
    uintptr_t pool = 0, obj = 0, apool = 0, actor = 0;
    uint16_t gen = 0, idx = 0, agen = 0, sgen = 0;
    if (!rd(anchors::addr(anchors::Id::ObjectsPool), &pool) || !pool) return 0;
    uintptr_t slot = pool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16) || !rd(slot, &obj) || !obj) return 0;
    if (!rd(obj + 0xb0, &idx) || !rd(obj + 0xb2, &agen) || !rd(anchors::addr(anchors::Id::ActorPool), &apool) || !apool) return 0;
    if (!rd(apool + idx * 0x10u + 8, &sgen) || sgen != agen || !rd(apool + idx * 0x10u, &actor)) return 0;
    return actor;
}
// The player's actor, as body.cpp finds it.
uintptr_t player_actor() {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.object) return 0;
    return actor_of_handle(static_cast<uint32_t>(st.object));
}
// (2026-11-01, GOAL-offhand-changes C11b; round 65 check 14, the user: most of the time the lasso does not aim at the NPC, or
// stops before hitting anything) LassoThrow=lock, LT held and the game holding a lock (T +0x3914, the player's): that actor's
// chest (its root, [[actor +0xb0] +0x18] +0x30 as gun_melee's, 1.1 m up), world; false: none
std::atomic<uint64_t> g_lasso_at_lock{0};
std::atomic<float> g_lock_chest[3] = {};  // (the status: the last chest aimed at)
bool locked_chest(float out[3]) {
    const uintptr_t actor = player_actor();
    uintptr_t T = 0, phys = 0, pm = 0;
    uint32_t lk = 0;
    if (!actor || !rd(actor + 0xb8, &T) || !T || !rd(T + 0x3914, &lk) || !lk || lk == 0xffffffffu) return false;
    const uintptr_t a = actor_of_handle(lk);
    if (!a || a == actor || !rd(a + 0xb0, &phys) || !phys || !rd(phys + 0x18, &pm) || !pm || !raw(pm + 0x30, out, 12)) return false;
    if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || !std::isfinite(out[2])) return false;
    out[1] += 1.1f;
    for (int k = 0; k < 3; ++k) g_lock_chest[k].store(out[k], std::memory_order_relaxed);
    return true;
}

// The player's firing object W: actor +0x70 (weapon manager) -> +0x80 (the item in hand) -> vtable +0xd8 -> +0xa0.
uintptr_t player_gun(uintptr_t actor) {
    uintptr_t wmgr = 0, item = 0, vt = 0, fn = 0, W = 0;
    const uintptr_t base = anchors::base();
    if (!rd(actor + 0x70, &wmgr) || !rd(wmgr + 0x80, &item) || !item || !rd(item, &vt) || !rd(vt + 0xd8, &fn)) return 0;
    if (fn < base || fn >= base + 0x4000000) return 0;
    const uintptr_t obj = call_get(fn, item);
    if (!obj || !rd(obj + 0xa0, &W)) return 0;
    return W;
}

// FUN_140396b50(targeting, out, yaw/pitch offsets) -> out: the reticle ray as a matrix, rows a (right), b (up), c
// (back: the ray looks along -c), d (origin), each 4 floats.
using AimRay_t = float* (*)(uintptr_t self, float* out, const float* yp);
AimRay_t o_aim_ray = nullptr;
std::atomic<uint64_t> g_de_skips{0};

// Dead Eye (research\gc-weapons.md 1.3): R = player info +0x410; R +0x1c the mode (1 slow motion, 2 marks), +0x20
// the phase (2 painting, 3 firing the marks). While the marks are fired the game turns its own camera onto each mark
// and shoots along it, so the barrel ray stands aside then.
bool dead_eye_firing_marks() {
    uintptr_t info = 0, R = 0;
    int32_t mode = 0, phase = 0;
    return rd(anchors::addr(anchors::Id::PlayerInfo), &info) && info && rd(info + 0x410, &R) && R && rd(R + 0x1c, &mode) &&
           rd(R + 0x20, &phase) && mode == 2 && phase == 3;
}

float* hk_aim_ray(uintptr_t self, float* out, const float* yp) {
    float* r = o_aim_ray(self, out, yp);
    g_rays.fetch_add(1, std::memory_order_relaxed);
    if (!g_barrel.load(std::memory_order_relaxed) || !out || !pose::anchor_active()) return r;
    if (dead_eye_firing_marks()) {
        g_de_skips.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    float A[9], a[3];
    bool ik = false;
    if (!body::item_correction(1, A, a, &ik) || !ik) {  // what the game's right hand holds, as drawn
        g_no_hand.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    // the lasso and thrown weapons have no barrel (the lasso's item matrix points along its coil): the ray goes along
    // the gun hand's pointing (the reticle target, the lasso's lock and the game's throw arc follow it)
    {
        RdrvrActorState st{};
        body::BodyPoints bp;
        const int lray = g_lasso_ray.load(std::memory_order_relaxed);
        if (api::actor_state(&st) && st.weapon == 21 && lray == 0) return r;  // (C12b's test: the game's own ray)
        if (api::actor_state(&st) && (st.weapon == 21 || gestures::is_thrown(st.weapon))) {
            if (!body::body_points(&bp) || !bp.hand_ok[bp.gun]) return r;
            const int gj = bp.gun;
            float bt[3] = {0.0f, 0.174f, -0.985f}, d[3], up[3] = {0, 1, 0}, rt[3], u2[3];
            barrel_in_target(bt);
            for (int k = 0; k < 3; ++k) d[k] = bp.target_rot[gj][k * 3] * bt[0] + bp.target_rot[gj][k * 3 + 1] * bt[1] + bp.target_rot[gj][k * 3 + 2] * bt[2];
            // 2026-10-12 ([Gestures] LassoAim): the lasso's ray along the head, the other hand, or the hand's frozen aim
            if (st.weapon == 21) {
                float la[3];
                if (gestures::lasso_aim_dir(d, la)) std::memcpy(d, la, sizeof(la));
                // (C11b) the lock held: the ray from the hand to the locked one's chest (within 40 m), so the throw goes at it
                float tc[3];
                if (gestures::lasso_lock_held() && locked_chest(tc)) {
                    // (C12b) from where the ray starts: the game's origin (out's fourth row), else the hand
                    const float* o0 = lray == 1 && out ? out + 12 : bp.hand[gj];
                    float v[3] = {tc[0] - o0[0], tc[1] - o0[1], tc[2] - o0[2]};
                    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (l > 0.5f && l < 40.0f) {
                        for (int k = 0; k < 3; ++k) d[k] = v[k] / l;
                        g_lasso_at_lock.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            rt[0] = d[1] * up[2] - d[2] * up[1];
            rt[1] = d[2] * up[0] - d[0] * up[2];
            rt[2] = d[0] * up[1] - d[1] * up[0];
            const float rl = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
            if (rl < 1e-3f) return r;  // pointing straight up or down
            for (int k = 0; k < 3; ++k) rt[k] /= rl;
            u2[0] = rt[1] * d[2] - rt[2] * d[1];
            u2[1] = rt[2] * d[0] - rt[0] * d[2];
            u2[2] = rt[0] * d[1] - rt[1] * d[0];
            for (int k = 0; k < 3; ++k) {
                out[0 * 4 + k] = rt[k];
                out[1 * 4 + k] = u2[k];
                out[2 * 4 + k] = -d[k];
                if (!(st.weapon == 21 && lray == 1)) out[12 + k] = bp.hand[gj][k] + d[k] * 0.10f;  // (lray 1: the game's origin kept)
                g_last_muzzle[k] = out[12 + k];
                g_last_dir[k] = d[k];
            }
            g_hand_rays.fetch_add(1, std::memory_order_relaxed);
            g_last_override_ms.store(log::now_ms(), std::memory_order_relaxed);
            return r;
        }
    }
    const uintptr_t actor = player_actor(), W = actor ? player_gun(actor) : 0;
    float gm[16];
    {  // (item 1b, after optimumbox's fork, v0.8.11.10, MIT) the correction and the game's matrix of the build it was drawn
       // with, when that is this gun's (barrel_ray's); else, as before, the live W +0x80 (a build newer in a clip)
        float A2[9], a2[3];
        bool ik2 = false;
        uintptr_t Wd = 0;
        if (W && body::item_gun(1, A2, a2, &ik2, gm, &Wd) && Wd == W && ik2) {
            std::memcpy(A, A2, sizeof(A));
            std::memcpy(a, a2, sizeof(a));
            g_ray_built.fetch_add(1, std::memory_order_relaxed);
        } else if (!W || !raw(W + 0x80, gm, sizeof(gm))) {
            g_no_gun.fetch_add(1, std::memory_order_relaxed);
            return r;
        } else {
            g_ray_live.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // the gun as drawn: each axis row turned by A, the origin through T(x) = A x + a
    float rows[4][3];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) rows[i][k] = A[k * 3] * gm[i * 4] + A[k * 3 + 1] * gm[i * 4 + 1] + A[k * 3 + 2] * gm[i * 4 + 2];
    for (int k = 0; k < 3; ++k) rows[3][k] = A[k * 3] * gm[12] + A[k * 3 + 1] * gm[13] + A[k * 3 + 2] * gm[14] + a[k];
    // the muzzle: the tune's MuzzleOffset (weapon info +0x310) in the gun's axes, when it is a sane offset
    uintptr_t info = 0;
    float mo[3] = {0, 0, 0};
    if (rd(W + 0x28, &info) && info && raw(info + 0x310, mo, sizeof(mo))) {
        const float len2 = mo[0] * mo[0] + mo[1] * mo[1] + mo[2] * mo[2];
        if (!(len2 < 1.0f)) mo[0] = mo[1] = mo[2] = 0;  // also catches NaN
    }
    float muzzle[3];
    for (int k = 0; k < 3; ++k) muzzle[k] = rows[3][k] + rows[0][k] * mo[0] + rows[1][k] * mo[1] + rows[2][k] * mo[2];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) out[i * 4 + k] = rows[i][k];
    for (int k = 0; k < 3; ++k) out[12 + k] = muzzle[k];
    for (int k = 0; k < 3; ++k) {
        g_last_muzzle[k] = muzzle[k];
        g_last_dir[k] = -rows[2][k];
        g_last_offset[k] = mo[k];
    }
    body::BodyPoints bp;
    if (body::body_points(&bp)) {
        float gd[3] = {-gm[8], -gm[9], -gm[10]};
        const float gl = std::sqrt(gd[0] * gd[0] + gd[1] * gd[1] + gd[2] * gd[2]);
        for (int k = 0; k < 3 && gl > 1e-6f; ++k) gd[k] /= gl;
        for (int k = 0; k < 3; ++k) {  // column k of each frame against the direction
            g_dir_target[k] = 0;
            g_dir_wrist[k] = 0;
            g_gun_anim[k] = 0;
            for (int i = 0; i < 3; ++i) {
                g_dir_target[k] += bp.target_rot[bp.gun][i * 3 + k] * -rows[2][i];
                g_dir_wrist[k] += bp.wrist_drawn[bp.gun][i * 3 + k] * -rows[2][i];
                g_gun_anim[k] += bp.wrist_anim[1][i * 3 + k] * gd[i];
            }
        }
        // (2026-11-02, GOAL-offhand-changes C15g; round 69, the user: the Blunderbuss's interaction points and loading ring move
        // about after a shot) through a steadied gun's fire clip the barrel kept as before it (the clip swings the game's gun:
        // the rings made along it followed the swing)
        // (the review: kept only for the gun and the hand it was learned with)
        static int bt_w = -1, bt_j = -1;
        RdrvrActorState bst{};
        const int w_now = api::actor_state(&bst) ? bst.weapon : -1;
        if (!body::steadied_fire_clip() || w_now != bt_w || bp.gun != bt_j) {
            for (int k = 0; k < 3; ++k) g_bt[k].store(g_dir_target[k], std::memory_order_relaxed);
            bt_w = w_now, bt_j = bp.gun;
        }
        g_bt_ms.store(log::now_ms(), std::memory_order_release);
    }
    g_overrides.fetch_add(1, std::memory_order_relaxed);
    g_last_override_ms.store(log::now_ms(), std::memory_order_relaxed);
    return r;
}

// The gun W's barrel through the correction T(x) = A x + a on its game matrix gm: the muzzle (the tune's MuzzleOffset in
// the gun's axes) and the direction; false on a bad one.
void muzzle_offset(uintptr_t W, float mo[3]) {  // the tune's MuzzleOffset (weapon info +0x310) in the gun's axes, when sane
    uintptr_t info = 0;
    mo[0] = mo[1] = mo[2] = 0.0f;
    if (rd(W + 0x28, &info) && info && raw(info + 0x310, mo, 3 * sizeof(float))) {
        const float len2 = mo[0] * mo[0] + mo[1] * mo[1] + mo[2] * mo[2];
        if (!(len2 < 1.0f)) mo[0] = mo[1] = mo[2] = 0;  // also catches NaN
    }
}
bool barrel_from(uintptr_t W, const float A[9], const float a[3], const float gm[16], float muzzle[3], float dir[3]) {
    float rows[4][3];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) rows[i][k] = A[k * 3] * gm[i * 4] + A[k * 3 + 1] * gm[i * 4 + 1] + A[k * 3 + 2] * gm[i * 4 + 2];
    for (int k = 0; k < 3; ++k) rows[3][k] = A[k * 3] * gm[12] + A[k * 3 + 1] * gm[13] + A[k * 3 + 2] * gm[14] + a[k];
    float mo[3];
    muzzle_offset(W, mo);
    float dl = 0.0f;
    for (int k = 0; k < 3; ++k) {
        muzzle[k] = rows[3][k] + rows[0][k] * mo[0] + rows[1][k] * mo[1] + rows[2][k] * mo[2];
        dir[k] = -rows[2][k];
        dl += dir[k] * dir[k];
    }
    if (!(dl > 0.98f && dl < 1.02f)) return false;  // also catches NaN
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(muzzle[k])) return false;
    return true;
}
// The drawn gun's barrel now (as hk_aim_ray places the shot): the muzzle and the direction. The game thread.
// (2026-10-28, GOAL-weapons-handling item 1b, after optimumbox's fork, v0.8.11.10, MIT) The drawn build's correction and
// the game's matrix of that same build (body::item_gun, when its W is the gun in hand: *built), else the old pairing of
// the correction with a live read of W +0x80, which the game moves between builds in an animation's clip (the fork: the
// Schofield's barrel turned up to 19 degrees for a frame, the hand still). live: the old pairing only (barrel_step_tick's
// control). Neither sees GunKick (drawn only: neither the correction nor the game's matrix carries it).
bool barrel_ray_path(bool live, float muzzle[3], float dir[3], bool* built, uint64_t* frame = nullptr) {
    float A[9], a[3], gm[16];
    bool ik = false;
    uintptr_t Wd = 0;
    if (built) *built = false;
    const uintptr_t actor = player_actor(), W = actor ? player_gun(actor) : 0;
    if (!W) return false;
    if (!live && body::item_gun(1, A, a, &ik, gm, &Wd, frame) && Wd == W) {
        if (!ik) return false;
        if (built) *built = true;
    } else if (!body::item_correction(1, A, a, &ik) || !ik || !raw(W + 0x80, gm, sizeof(gm))) {
        return false;
    }
    return barrel_from(W, A, a, gm, muzzle, dir);
}
bool barrel_ray(float muzzle[3], float dir[3]) {
    bool built = false;
    const bool ok = barrel_ray_path(false, muzzle, dir, &built);
    if (ok) (built ? g_br_built : g_br_live).fetch_add(1, std::memory_order_relaxed);
    return ok;
}
// item 1b's measure ("aim barrelstep [on|off|reset]", the session; reads only): each script tick with a gun in hand, the
// barrel both ways, the drawn build's (barrel_ray's) and the old pairing (the live W +0x80), each one's largest turn
// from the tick before, the two's largest difference at one tick, and the gun hand's IK target's largest turn (the
// controller: is the hand still?). Starts over on another gun, or when either way fails for a tick.
// (The review: that measures barrel_ray against itself, so a steady but wrong way would pass.) Also each tick against the
// gun as DRAWN (body::drawn_gun, the same body frame as the correction the new way took; else skipped): both ways'
// direction against the drawn root's barrel (-z) and their muzzles against the drawn root's (the same MuzzleOffset),
// for the drawn root after the hand's correction ([0], before the steadying) and as seen ([1], after it); and how many
// of those draws matched an older build's sample, and the held draws' newest/older matches since the reset.
std::atomic<bool> g_bs_on{false};
std::mutex g_bs_mutex;
struct BarrelSteps {
    uint64_t ticks = 0, built = 0, steps = 0, new_over = 0, old_over = 0;  // (over: a step over 2 degrees)
    double max_new = 0.0, max_old = 0.0, max_apart = 0.0, max_hand = 0.0;
    float d_new[3] = {}, d_old[3] = {}, R[9] = {};
    bool have = false;
    uintptr_t W = 0;
    uint64_t drawn = 0, drawn_skip = 0, drawn_older = 0;  // ticks compared with the drawn gun; skipped (another frame, none); older
    double dw_deg[2][2] = {}, dw_mm[2][2] = {};          // the largest: [0 the new way, 1 the old][the drawn root 0 corrected, 1 seen]
    uint64_t newest0 = 0, older0 = 0;                     // the held draws' matches at the reset
} g_bs;  // under g_bs_mutex
double step_deg(const float* u, const float* v) {  // atan2(|u x v|, u.v) in double: acos in float resolves only ~0.02 deg
    const double cx = static_cast<double>(u[1]) * v[2] - static_cast<double>(u[2]) * v[1];
    const double cy = static_cast<double>(u[2]) * v[0] - static_cast<double>(u[0]) * v[2];
    const double cz = static_cast<double>(u[0]) * v[1] - static_cast<double>(u[1]) * v[0];
    const double d = static_cast<double>(u[0]) * v[0] + static_cast<double>(u[1]) * v[1] + static_cast<double>(u[2]) * v[2];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), d) * 57.29577951308232;
}
void barrel_step_tick() {
    float m1[3], d1[3], m0[3], d0[3];
    bool built = false;
    uint64_t cframe = 0;
    const bool ok1 = barrel_ray_path(false, m1, d1, &built, &cframe), ok0 = barrel_ray_path(true, m0, d0, nullptr);
    body::BodyPoints bp;
    const bool hok = body::body_points(&bp) && bp.gun >= 0 && bp.gun < 2 && bp.hand_ok[bp.gun];
    const uintptr_t actor = player_actor(), W = actor ? player_gun(actor) : 0;
    float DR[2][9], Do[2][3], mo[3] = {0, 0, 0};
    uint64_t dframe = 0;
    bool dolder = false;
    const bool dok = body::drawn_gun(DR, Do, &dframe, &dolder, nullptr) && built && dframe == cframe;
    if (dok && W) muzzle_offset(W, mo);
    std::lock_guard lock(g_bs_mutex);
    BarrelSteps& s = g_bs;
    if (!ok1 || !ok0 || !hok || !W || W != s.W) {
        s.have = false;
        s.W = W;
        return;
    }
    ++s.ticks;
    if (built) ++s.built;
    if (!dok) {
        ++s.drawn_skip;
    } else {
        ++s.drawn;
        if (dolder) ++s.drawn_older;
        for (int st = 0; st < 2; ++st) {
            const float* R = DR[st];
            const float bz[3] = {-R[2], -R[5], -R[8]};  // the drawn root's -z (axis 2's components)
            float mz[3];
            for (int k = 0; k < 3; ++k) mz[k] = Do[st][k] + R[k * 3] * mo[0] + R[k * 3 + 1] * mo[1] + R[k * 3 + 2] * mo[2];
            for (int way = 0; way < 2; ++way) {
                const float* dd = way ? d0 : d1;
                const float* mm = way ? m0 : m1;
                const double e = std::sqrt(static_cast<double>(mm[0] - mz[0]) * (mm[0] - mz[0]) + static_cast<double>(mm[1] - mz[1]) * (mm[1] - mz[1]) +
                                           static_cast<double>(mm[2] - mz[2]) * (mm[2] - mz[2]));
                s.dw_deg[way][st] = std::fmax(s.dw_deg[way][st], step_deg(dd, bz));
                s.dw_mm[way][st] = std::fmax(s.dw_mm[way][st], e * 1000.0);
            }
        }
    }
    s.max_apart = std::fmax(s.max_apart, step_deg(d1, d0));
    const float* R = bp.target_rot[bp.gun];
    if (s.have) {
        ++s.steps;
        const double sn = step_deg(d1, s.d_new), so = step_deg(d0, s.d_old);
        s.max_new = std::fmax(s.max_new, sn);
        s.max_old = std::fmax(s.max_old, so);
        if (sn > 2.0) ++s.new_over;
        if (so > 2.0) ++s.old_over;
        double tr = 0.0;  // the turn between the two targets: acos((tr(R0^T R1) - 1) / 2)
        for (int i = 0; i < 9; ++i) tr += static_cast<double>(R[i]) * s.R[i];
        s.max_hand = std::fmax(s.max_hand, std::acos(std::clamp((tr - 1.0) * 0.5, -1.0, 1.0)) * 57.29577951308232);
    }
    std::memcpy(s.d_new, d1, sizeof(d1));
    std::memcpy(s.d_old, d0, sizeof(d0));
    std::memcpy(s.R, R, sizeof(s.R));
    s.have = true;
}
// The reticle's distance held steady (2026-10-28, GOAL-weapons-handling item 1b, after optimumbox's fork, v0.8.11.10,
// MIT; the fork's user: the dot "bounces"): for an aim held almost still the barrel's probe flips between two distances
// (the fork: 65, 149, 69, 156 m on consecutive probes: a thin thing, branches, in and out of the ray). A distance far off
// the held one (over 30 %, and over 1 m) replaces it only once three probes in a row agree on it (about 0.1 s at a probe
// each tick or two); one near it is taken at once (and tracked). Ours also starts over after a gap of 250 ms in the
// probes (the reticle off, no gun, a stall): the first probe then is taken at once, not held against an old aim's.
// The game thread (reticle_tick); its trace ("aim reticle dist") under g_rp_mutex.
// (2026-10-28, round 38 check 2) the second gun's probe has its own (g_dh2, its trace "aim reticle dist2")
struct DistHold {
    bool ok = false;
    float d = 0.0f, cd = 0.0f;  // the held distance, the candidate
    bool hit = false, chit = false;
    int n = 0;                  // the candidate's probes in a row
    double ms = 0.0;
} g_dh, g_dh2;
std::atomic<uint64_t> g_dist_far{0}, g_dist_changes{0};  // probes far off the held one; changes of it
std::atomic<uint64_t> g_dist2_far{0}, g_dist2_changes{0};
constexpr int kDistTraceN = 128;
struct DistTrace {
    double ms;
    float raw, held;
} g_dist_trace[kDistTraceN], g_dist2_trace[kDistTraceN];  // the last probes (under g_rp_mutex)
uint64_t g_dist_trace_n = 0, g_dist2_trace_n = 0;
bool dist_close(float x, float y) { return std::fabs(x - y) <= std::fmax(1.0f, 0.3f * std::fmin(x, y)); }
float dist_hold(DistHold& h, std::atomic<uint64_t>& far_n, std::atomic<uint64_t>& changes, float dist, bool hit, double now, bool* held_hit) {
    if (!h.ok || now - h.ms > 250.0 || dist_close(dist, h.d)) {
        h.d = dist;
        h.hit = hit;
        h.n = 0;
    } else {
        far_n.fetch_add(1, std::memory_order_relaxed);
        h.n = h.n > 0 && dist_close(dist, h.cd) ? h.n + 1 : 1;
        h.cd = dist;
        h.chit = hit;
        if (h.n >= 3) {
            changes.fetch_add(1, std::memory_order_relaxed);
            h.d = h.cd;
            h.hit = h.chit;
            h.n = 0;
        }
    }
    h.ok = true;
    h.ms = now;
    *held_hit = h.hit;
    return h.d;
}
uint64_t f32bits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}
uint64_t pack2(float x, float y) {
    float xy[2] = {x, y};
    uint64_t v = 0;
    std::memcpy(&v, xy, sizeof(xy));
    return v;
}
// (2026-10-28, round 38 check 2, the user: "works for the main gun, second guns reticle still all over the place") The
// second reticle's own probe. Before, the second reticle took the main barrel's held distance (its own hit never
// probed), and its line was the second gun's W +0xa0 read live from the XR thread (the game's holster matrix when that
// read came before this update's placement, or when the placement refused) or, for the copy, the gun hand's line moved
// by the copy's T of the game's newest update (driven by the gun hand at the XR rate, snapped at the game's). Now each
// script tick, while dual wield is on and not carried, a probe as the main's along the second gun (or the copy) as
// drawn (body::second_drawn: the published draw, no game memory read across threads), its muzzle the tune's
// MuzzleOffset on that gun (the copy: the two models' muzzles' shift too), its distance held as the main's (g_dh2).
// "aim reticle drawn2 off" (the session): no second probe, the second reticle on the old line (the tests' control; and
// "scene render" with it and without, the probe's cost: one more CAMERA_PROBE a tick, its own run timed by api).
std::atomic<bool> g_ret2_drawn_on{true};
std::atomic<uint64_t> g_rp2_probes{0}, g_rp2_hits{0}, g_rp2_lost{0}, g_rp2_nodrawn{0}, g_rp2_otherW{0};
ReticleProbe g_rp2;  // under g_rp_mutex
std::atomic<bool> g_shot2_drawn{true};  // "aim shot2 drawn on|off" (the session): the second gun's and the copy's shots on the drawn map
std::atomic<uint64_t> g_shot2_mapped{0}, g_shot2_unmapped{0};
void second_probe_tick(double now) {
    static uint64_t pending = 0;
    static double pending_ms = 0.0;
    static ReticleProbe req;
    if (pending) {
        RdrvrNativeResult r{};
        if (api::wait_native(pending, &r, 0)) {
            pending = 0;
            const bool hit = (r.value & 0xff) != 0;
            float dd = 0.0f;
            for (int k = 0; k < 3; ++k) dd += (r.vec[k] - req.muzzle[k]) * (r.vec[k] - req.muzzle[k]);
            float dist = hit && std::isfinite(dd) ? std::sqrt(dd) : kProbeRange;
            if (dist > kProbeRange) dist = kProbeRange;
            bool held_hit = hit;
            std::lock_guard lock(g_rp_mutex);
            const float held = dist_hold(g_dh2, g_dist2_far, g_dist2_changes, dist, hit, now, &held_hit);
            g_rp2 = req;
            g_rp2.dist = held;
            g_rp2.hit = held_hit;
            g_rp2.ms = now;
            g_dist2_trace[g_dist2_trace_n++ % kDistTraceN] = {now, dist, held};
            if (hit) g_rp2_hits.fetch_add(1, std::memory_order_relaxed);
        } else if (now - pending_ms > 500.0) {  // the plugin never ran it (a stall): given up
            api::cancel_op(pending);
            pending = 0;
            g_rp2_lost.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (pending || !g_reticle.load(std::memory_order_relaxed) || !g_barrel.load(std::memory_order_relaxed) || !g_ret2_drawn_on.load(std::memory_order_relaxed))
        return;
    const dual::State ds = dual::state();
    if (!ds.on || ds.carry || !ds.ready) return;
    RdrvrActorState st{};
    // (a gun in hand, or [Hands] DualWieldBesideTorch's torch: dual::second_beside)
    if (!api::actor_state(&st) || !st.valid || !dual::second_beside(st.weapon)) return;
    body::SecondDrawn sd;
    if (!body::second_drawn(&sd) || sd.copy != ds.copy) {
        g_rp2_nodrawn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // (this thread's own view of the gun: the draw published must be of it)
    const uintptr_t W = ds.copy ? dual::copy_W() : dual::secondary_W(nullptr);
    if (!W || W != sd.W) {
        g_rp2_otherW.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float mo[3];
    muzzle_offset(W, mo);
    for (int k = 0; k < 3; ++k) mo[k] += sd.shift[k];
    float m[3], d[3], dl = 0.0f;
    for (int k = 0; k < 3; ++k) {  // the rows: the axes, then the position
        m[k] = sd.m[12 + k] + sd.m[k] * mo[0] + sd.m[4 + k] * mo[1] + sd.m[8 + k] * mo[2];
        d[k] = -sd.m[8 + k];
        dl += d[k] * d[k];
    }
    if (!(dl > 0.98f && dl < 1.02f) || !std::isfinite(m[0] + m[1] + m[2])) {  // also NaN
        g_rp2_nodrawn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float t[3] = {m[0] + d[0] * kProbeRange, m[1] + d[1] * kProbeRange, m[2] + d[2] * kProbeRange};
    const uint64_t args[7] = {0, pack2(m[0], m[1]), f32bits(m[2]), pack2(t[0], t[1]), f32bits(t[2]),
                              static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), 0};
    pending = api::queue_native(0x720F2CA7u, args, 7, 1);  // CAMERA_PROBE(Result*, Source, Target, Owner, 0), as the main's
    api::time_native(pending, 1);
    pending_ms = now;
    req = ReticleProbe{};
    std::memcpy(req.muzzle, m, sizeof(m));
    std::memcpy(req.dir, d, sizeof(d));
    std::memcpy(req.hand, sd.t, sizeof(req.hand));  // its hand's target in the drawn frame: the muzzle moves with it since
    req.hand_ok = true;
    std::memcpy(req.mo, mo, sizeof(mo));
    req.W = W;
    req.john = sd.john;
    req.copy = sd.copy;
    g_rp2_probes.fetch_add(1, std::memory_order_relaxed);
}
// Each script tick (api's tick hook): the last probe's result taken, a new one queued along the barrel now.
void reticle_tick() {
    static uint64_t pending = 0;
    static double pending_ms = 0.0;
    static float req_m[3], req_d[3], req_h[3];
    static bool req_h_ok = false;
    const double now = log::now_ms();
    if (g_bs_on.load(std::memory_order_relaxed)) barrel_step_tick();  // item 1b's measure (the session)
    second_probe_tick(now);  // round 38 check 2: the second gun's own
    if (pending) {
        RdrvrNativeResult r{};
        if (api::wait_native(pending, &r, 0)) {
            pending = 0;
            const bool hit = (r.value & 0xff) != 0;
            float dd = 0.0f;
            for (int k = 0; k < 3; ++k) dd += (r.vec[k] - req_m[k]) * (r.vec[k] - req_m[k]);
            float dist = hit && std::isfinite(dd) ? std::sqrt(dd) : kProbeRange;
            if (dist > kProbeRange) dist = kProbeRange;
            bool held_hit = hit;
            const float held = dist_hold(g_dh, g_dist_far, g_dist_changes, dist, hit, now, &held_hit);  // item 1b: the distance held steady
            std::lock_guard lock(g_rp_mutex);
            std::memcpy(g_rp.muzzle, req_m, sizeof(req_m));
            std::memcpy(g_rp.dir, req_d, sizeof(req_d));
            std::memcpy(g_rp.hand, req_h, sizeof(req_h));
            g_rp.hand_ok = req_h_ok;
            g_rp.dist = held;
            g_rp.hit = held_hit;
            g_rp.ms = now;
            g_dist_trace[g_dist_trace_n++ % kDistTraceN] = {now, dist, held};
            if (hit) g_rp_hits.fetch_add(1, std::memory_order_relaxed);  // (the probes' own hits)
        } else if (now - pending_ms > 500.0) {  // the plugin never ran it (a stall): given up
            api::cancel_op(pending);
            pending = 0;
            g_rp_lost.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (pending || !g_reticle.load(std::memory_order_relaxed) || !g_barrel.load(std::memory_order_relaxed)) return;
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.valid || !reload::is_gun(st.weapon)) {
        g_rp_nogun.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float m[3], d[3];
    if (!barrel_ray(m, d)) {
        g_rp_nobarrel.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float t[3] = {m[0] + d[0] * kProbeRange, m[1] + d[1] * kProbeRange, m[2] + d[2] * kProbeRange};
    const uint64_t args[7] = {0, pack2(m[0], m[1]), f32bits(m[2]), pack2(t[0], t[1]), f32bits(t[2]),
                              static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), 0};
    pending = api::queue_native(0x720F2CA7u, args, 7, 1);  // CAMERA_PROBE(Result*, Source, Target, Owner, 0)
    api::time_native(pending, 0);  // (its run timed: the second probe's cost against it, "aim reticle dist2")
    pending_ms = now;
    std::memcpy(req_m, m, sizeof(m));
    std::memcpy(req_d, d, sizeof(d));
    {
        body::BodyPoints bp;
        req_h_ok = body::body_points(&bp) && bp.hand_ok[bp.gun];
        if (req_h_ok) std::memcpy(req_h, bp.hand[bp.gun], sizeof(req_h));
    }
    g_rp_probes.fetch_add(1, std::memory_order_relaxed);
    // while the game aims: where its target actor is (the review of 0.8.3: its target point follows the barrel's ray,
    // so only the actor's own position tells a stale handle from one under the reticle)
    static uint64_t tpend = 0;
    static int32_t treq = 0;
    if (tpend) {
        RdrvrNativeResult r{};
        if (api::wait_native(tpend, &r, 0)) {
            tpend = 0;
            std::lock_guard lock(g_rp_mutex);
            g_rp.tgt = treq;
            std::memcpy(g_rp.tgt_pos, r.vec, sizeof(g_rp.tgt_pos));
            g_rp.tgt_ms = now;
        } else if (api::cancel_op(tpend)) {
            tpend = 0;
        }
    }
    if (!tpend && now - g_last_override_ms.load(std::memory_order_relaxed) < 150.0) {
        const uintptr_t actor = player_actor();
        uintptr_t T = 0;
        int32_t h = 0;
        if (actor && rd(actor + 0xb8, &T) && T && rd(T + 0x3900, &h) && h != 0 && h != -1) {
            const uint64_t pa[2] = {static_cast<uint64_t>(static_cast<uint32_t>(h)), 0};
            tpend = api::queue_native(0x99BD9D6Fu, pa, 2, 2);  // GET_POSITION(actor, Vector3*)
            treq = h;
        }
    }
}

// FUN_1403237a0: the projectile launch, the one way player gun projectiles start (research\gc-aim.md 8-9):
// (projectile, matrix rows a b c d, weapon type, velocity, owner handle, launch position (the tracer's start), flags,
// ...). For the player's shots: logged against the barrel ray, and the tracer started at the drawn muzzle.
using Launch_t = void (*)(uintptr_t p, const float* mtx, int wtype, const float* vel, uint32_t owner, const float* launch, uint32_t flags,
                          char c8, float f9, const float* voff);
Launch_t o_launch = nullptr;
std::atomic<bool> g_drop_lit{true};  // [Gestures] DropLitSlow (step 5, answer 5)
std::atomic<uint64_t> g_lit_drops{0};
std::atomic<double> g_grip_letgo_ms{0.0};  // (the review) the held throw's grip let go (not its timeout): the lit drop's window

std::atomic<double> g_throw_start_ms{0};  // the player's last throw start (QuickThrow), for the launch's log
// [Hands] DualWield: the spawn's W (hk_spawn sets it around the game's call), and the second gun's pellets as seen
thread_local uintptr_t t_spawn_W = 0;
thread_local int t_sec_pellets = 0;
thread_local int t_copy_pellets = 0;  // [Hands] DualWieldCopy: the copy's shot's pellets (one log line)
thread_local double t_sec_first = -1.0, t_sec_max = -1.0;
thread_local float t_sec_start[3] = {}, t_sec_dir[3] = {}, t_sec_speed = 0.0f;
thread_local double t_sec_placed = -1.0;  // (round 38 check 2) the game's own first pellet against its placed barrel (-W +0xa0)
thread_local bool t_sec_mapped = false;   // on the drawn map
// row-vector affine matrices (the game's, 4x4): the inverse (R^-1 and -t R^-1) and a b
bool rows_inverse(const float* m, float* out) {
    const float a = m[0], b_ = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b_ * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-8f)) return false;
    const float k = 1.0f / det;
    const float r[9] = {(e * i - f * h) * k, (c * h - b_ * i) * k, (b_ * f - c * e) * k, (f * g - d * i) * k, (a * i - c * g) * k,
                        (c * d - a * f) * k, (d * h - e * g) * k, (b_ * g - a * h) * k, (a * e - b_ * d) * k};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) out[row * 4 + col] = r[row * 3 + col];
        out[row * 4 + 3] = 0.0f;
    }
    for (int col = 0; col < 3; ++col) out[12 + col] = -(m[12] * r[col] + m[13] * r[3 + col] + m[14] * r[6 + col]);
    out[15] = 1.0f;
    return true;
}
void rows_mul(const float* a, const float* b, float* out) {  // out = a b (affine: the fourth columns taken as 0 0 0 1)
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 3; ++col)
            out[row * 4 + col] = a[row * 4] * b[col] + a[row * 4 + 1] * b[4 + col] + a[row * 4 + 2] * b[8 + col] + (row == 3 ? b[12 + col] : 0.0f);
        out[row * 4 + 3] = row == 3 ? 1.0f : 0.0f;
    }
}
std::atomic<uint64_t> g_sec_shots{0};
std::atomic<float> g_sec_err{-1.0f};
// (2026-10-13) a mounted gun's last round (mounted_shot)
std::mutex g_mounted_mutex;
float g_mounted_dir[3] = {0.0f, 0.0f, 0.0f};  // its last round's direction (world), under g_mounted_mutex
double g_mounted_dir_ms = -1e12;
// (2026-11-01, GOAL-offhand-changes C13; round 64, the user: every off-hand throwable bursts at the hand or drops) a thrown
// projectile's flight, measured: the launch's projectile (FUN_1403237a0's first argument: its matrix +0x20, its place
// +0x50, its velocity +0x1f0, the launch's 7th argument <<3 at +0x284, its 8th bit 0x20 of +0x2f8, its type +0x298) read
// at 100, 300, 600 and 1000 ms (throw_hold_tick, the game thread), its distance from the launch point logged once. The
// pool's memory stays mapped (raw reads; a projectile done and reused reads as another's place: the line says so by its
// jump). The off hand's launches ("ours") and the game's own throws
struct ThrowTrack {
    uintptr_t p = 0;
    float org[3] = {};
    double ms = 0.0;
    int wtype = -1, step = 0;
    bool ours = false;
    float d[4] = {-1, -1, -1, -1}, dy[4] = {}, sp[4] = {};
};
ThrowTrack g_track;  // (the game thread's)
std::atomic<double> g_game_throw_launch_ms{-1e12};  // (C1) the player's own thrown item last launched by the game's throw
char g_track_last[320] = "";
std::mutex g_track_mx;  // g_track, g_track_last
void track_throw(uintptr_t p, const float* org, int wtype, bool ours) {
    std::lock_guard lock(g_track_mx);
    g_track = ThrowTrack{};
    g_track.p = p;
    std::memcpy(g_track.org, org, sizeof(g_track.org));
    g_track.ms = log::now_ms();
    g_track.wtype = wtype;
    g_track.ours = ours;
}
void track_tick() {
    std::lock_guard lock(g_track_mx);
    ThrowTrack& t = g_track;
    if (!t.p || t.step >= 4) return;
    static const double kAt[4] = {100.0, 300.0, 600.0, 1000.0};
    if (log::now_ms() - t.ms < kAt[t.step]) return;
    alignas(16) float pos[4] = {}, v[4] = {};
    if (raw(t.p + 0x50, pos, sizeof(pos)) && raw(t.p + 0x1f0, v, sizeof(v))) {
        const float dx = pos[0] - t.org[0], dy = pos[1] - t.org[1], dz = pos[2] - t.org[2];
        t.d[t.step] = std::sqrt(dx * dx + dy * dy + dz * dz);
        t.dy[t.step] = dy;
        t.sp[t.step] = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }
    if (++t.step < 4) return;
    uint32_t f284 = 0, ty = 0;
    uint8_t f2f8 = 0;
    raw(t.p + 0x284, &f284, sizeof(f284));
    raw(t.p + 0x2f8, &f2f8, sizeof(f2f8));
    raw(t.p + 0x298, &ty, sizeof(ty));
    std::snprintf(g_track_last, sizeof(g_track_last),
                  "weapon %d (%s): from the launch point at 0.1/0.3/0.6/1.0 s %.2f/%.2f/%.2f/%.2f m (up %.2f/%.2f/%.2f/%.2f), speed %.1f/%.1f/%.1f/%.1f m/s | "
                  "+0x284 %u +0x2f8 0x%02x type %u",
                  t.wtype, t.ours ? "the off hand's launch" : "the game's throw", t.d[0], t.d[1], t.d[2], t.d[3], t.dy[0], t.dy[1], t.dy[2], t.dy[3], t.sp[0],
                  t.sp[1], t.sp[2], t.sp[3], f284, f2f8, ty);
    log::info("[aim] the throw's flight: %s", g_track_last);
    t.p = 0;
}

void hk_launch(uintptr_t p, const float* mtx, int wtype, const float* vel, uint32_t owner, const float* launch, uint32_t flags, char c8,
               float f9, const float* voff) {
    {  // [Hands] OffHandItems: the off hand's throw (FUN_1403ba970 called by offhand.cpp on this thread, now): the hand's
       // velocity and place for the game's 650 m/s from the hand (its matrix's place and the launch point moved there)
        alignas(16) float ov[4] = {0, 0, 0, 0}, oo[3] = {0, 0, 0};
        int owt = -1;
        bool othrown = false;
        if (vel && offhand::launch_override(ov, oo, &owt, &othrown)) {
            alignas(16) float v0[4] = {0, 0, 0, 0}, m[16], at[4] = {oo[0], oo[1], oo[2], 0.0f};
            raw(reinterpret_cast<uintptr_t>(vel), v0, sizeof(v0));
            ov[3] = v0[3];
            const bool mok = mtx && raw(reinterpret_cast<uintptr_t>(mtx), m, sizeof(m));
            if (mok)
                for (int k = 0; k < 3; ++k) m[12 + k] = oo[k];
            if (launch) raw(reinterpret_cast<uintptr_t>(launch) + 12, &at[3], sizeof(float));
            o_launch(p, mok ? m : mtx, owt >= 0 ? owt : wtype, ov, owner, launch ? at : launch, othrown ? 1u : flags, othrown ? 1 : c8, f9,
                     voff);  // (F4's test)
            track_throw(p, oo, owt >= 0 ? owt : wtype, true);
            return;
        }
    }
    uintptr_t info = 0;
    uint32_t me = 0;
    if (!vel || !rd(anchors::addr(anchors::Id::PlayerInfo), &info) || !info || !rd(info + 0x5ec, &me) || owner != me) {
        o_launch(p, mtx, wtype, vel, owner, launch, flags, c8, f9, voff);
        return;
    }
    float v[4] = {0, 0, 0, 0}, d[3] = {0, 0, 0};
    raw(reinterpret_cast<uintptr_t>(vel), v, sizeof(v));
    if (gestures::is_thrown(wtype)) {
        // a thrown weapon leaves the hand with its velocity ([Gestures] Throw): the projectile's matrix moved to the hand
        alignas(16) float nv[4] = {0, 0, 0, v[3]}, org[3], m[16];
        if (mtx && gestures::throw_launch(nv, org) && raw(reinterpret_cast<uintptr_t>(mtx), m, sizeof(m))) {
            for (int k = 0; k < 3; ++k) m[12 + k] = org[k];
            alignas(16) float at[4] = {org[0], org[1], org[2], 0.0f};
            if (launch) raw(reinterpret_cast<uintptr_t>(launch) + 12, &at[3], sizeof(float));
            const double ts = g_throw_start_ms.load(std::memory_order_relaxed);
            log::info("[aim] thrown weapon %d: from the hand (%.2f %.2f %.2f) at (%.2f %.2f %.2f) m/s (the game's %.1f m/s), %.0f ms after the throw's start",
                      wtype, org[0], org[1], org[2], nv[0], nv[1], nv[2], std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]),
                      ts > 0 ? log::now_ms() - ts : -1.0);
            o_launch(p, m, wtype, nv, owner, launch ? at : launch, flags, c8, f9, voff);
            track_throw(p, org, wtype, false);
            g_game_throw_launch_ms.store(log::now_ms(), std::memory_order_relaxed);
            return;
        }
        // (2026-10-31, the off-hand build step 5; the user's answer 5: lit dynamite let go slowly, "Drop it") the dynamite or
        // the fire bottle lit by the trigger (ThrowByGrip) let go with the hand all but still: dropped from the hand, its fuse
        // or flame running, not thrown at the game's own speed (r78 P6: 80 m/s from a still let-go)
        // (the review: only a held throw the grip let go within 500 ms; never the 15 s timeout's, nor a quick throw's)
        const double lg = g_grip_letgo_ms.exchange(0.0, std::memory_order_relaxed);
        if ((wtype == 23 || wtype == 24) && grip_throw() && g_drop_lit.load(std::memory_order_relaxed) && mtx && lg > 0.0 &&
            log::now_ms() - lg < 500.0) {
            float hv[3], ho[3], hs = 0.0f;
            alignas(16) float m[16];
            if (gestures::hand_throw(controls::gun_hand(), 300.0, hv, ho, &hs) && hs < 1.5f && raw(reinterpret_cast<uintptr_t>(mtx), m, sizeof(m))) {
                for (int k = 0; k < 3; ++k) m[12 + k] = ho[k];
                alignas(16) float nv[4] = {0.0f, -0.5f, 0.0f, v[3]}, at[4] = {ho[0], ho[1], ho[2], 0.0f};
                if (launch) raw(reinterpret_cast<uintptr_t>(launch) + 12, &at[3], sizeof(float));
                g_lit_drops.fetch_add(1, std::memory_order_relaxed);
                log::info("[aim] thrown weapon %d: let go slowly (the hand %.2f m/s): dropped from the hand (%.2f %.2f %.2f), lit", wtype, hs, ho[0], ho[1], ho[2]);
                o_launch(p, m, wtype, nv, owner, launch ? at : launch, flags, c8, f9, voff);
                g_game_throw_launch_ms.store(log::now_ms(), std::memory_order_relaxed);
                return;
            }
        }
        log::info("[aim] thrown weapon %d: the game's throw (%.1f m/s)", wtype, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
        o_launch(p, mtx, wtype, vel, owner, launch, flags, c8, f9, voff);
        g_game_throw_launch_ms.store(log::now_ms(), std::memory_order_relaxed);
        if (alignas(16) float gm[16]; mtx && raw(reinterpret_cast<uintptr_t>(mtx), gm, sizeof(gm))) track_throw(p, gm + 12, wtype, false);  // (C13: the control)
        return;
    }
    if (wtype >= 26 && wtype <= 28) {  // (2026-10-13) a mounted gun's round: the game's, its direction kept (mounted_shot)
        const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (vl > 1e-3f) {
            std::lock_guard lock(g_mounted_mutex);
            for (int k = 0; k < 3; ++k) g_mounted_dir[k] = v[k] / vl;
            g_mounted_dir_ms = log::now_ms();
        }
        o_launch(p, mtx, wtype, vel, owner, launch, flags, c8, f9, voff);
        return;
    }
    // (2026-10-28, round 38 check 2) the copy's and the second gun's shots on the drawn map ("aim shot2 off", the session:
    // as before), so the shot leaves the gun the second reticle is drawn on: the copy's by Tw, the gun in hand as the aim
    // takes it onto the copy as drawn (body::second_drawn: the copy's T was the game's newest update's, the hand there),
    // its muzzle shift as T's; the second gun's by X = P^-1 D, P the matrix the shot was made from (W +0x80, read here:
    // this thread's own) when it is this update's placement (body::secondary_placed), onto it as drawn. Either refused
    // (the old way) when the moved shot would start over 1 m from the gun as drawn. A launch point not read: passed on as
    // the game gave it (never zeros)
    // p' = p T on the shot's matrix m (rows: the translation row takes T's), its velocity, its launch point; false: the
    // moved start over 1 m from `drawn_at` (when given)
    auto move_shot = [&](const float* T, const float* m, float* mm, float* nv, float* at, bool* at_ok, const float* drawn_at) {
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                mm[row * 4 + col] = m[row * 4] * T[col] + m[row * 4 + 1] * T[4 + col] + m[row * 4 + 2] * T[8 + col] +
                                    (row == 3 ? T[12 + col] : m[row * 4 + 3] * T[12 + col]);
        for (int col = 0; col < 3; ++col) nv[col] = v[0] * T[col] + v[1] * T[4 + col] + v[2] * T[8 + col];
        *at_ok = launch && raw(reinterpret_cast<uintptr_t>(launch), at, 4 * sizeof(float));
        if (*at_ok) {
            const float l0 = at[0], l1 = at[1], l2 = at[2];
            for (int col = 0; col < 3; ++col) at[col] = l0 * T[col] + l1 * T[4 + col] + l2 * T[8 + col] + T[12 + col];
        }
        if (!drawn_at) return true;
        const float ex = mm[12] - drawn_at[12], ey = mm[13] - drawn_at[13], ez = mm[14] - drawn_at[14];
        return ex * ex + ey * ey + ez * ez < 1.0f;  // (also NaN)
    };
    if (t_in_spawn && dual::copy_armed(t_spawn_W)) {  // [Hands] DualWieldCopy: the copy's shot, moved by T onto the copy
        alignas(16) float T[16], m[16], mm[16], nv[4] = {0, 0, 0, v[3]}, at[4] = {0, 0, 0, 0};
        body::SecondDrawn sd;
        bool drawn_map = false, at_ok = false;
        if (mtx && raw(reinterpret_cast<uintptr_t>(mtx), m, sizeof(m))) {
            if (g_shot2_drawn.load(std::memory_order_relaxed) && body::second_drawn(&sd) && sd.copy && sd.tw_ok && sd.W == t_spawn_W) {
                std::memcpy(T, sd.Tw, sizeof(T));
                for (int k = 0; k < 3; ++k) T[12 + k] += sd.shift[0] * sd.m[k] + sd.shift[1] * sd.m[4 + k] + sd.shift[2] * sd.m[8 + k];  // d D (rows)
                drawn_map = move_shot(T, m, mm, nv, at, &at_ok, sd.m);
            }
            if (drawn_map || (dual::copy_T(T) && move_shot(T, m, mm, nv, at, &at_ok, nullptr))) {
                const float sl = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
                if (t_copy_pellets++ == 0) {
                    (drawn_map ? g_shot2_mapped : g_shot2_unmapped).fetch_add(1, std::memory_order_relaxed);
                    log::info("[wield] the copy's shot: start (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) speed %.0f (moved from (%.2f %.2f %.2f)) | the drawn map %d",
                              mm[12], mm[13], mm[14], sl > 1e-3f ? nv[0] / sl : 0.0f, sl > 1e-3f ? nv[1] / sl : 0.0f, sl > 1e-3f ? nv[2] / sl : 0.0f, sl,
                              m[12], m[13], m[14], drawn_map ? 1 : 0);
                }
                o_launch(p, mm, dual::copy_shot_weapon(wtype), nv, owner, at_ok ? at : launch, flags, c8, f9, voff);
                return;
            }
        }
    }
    if (t_in_spawn && dual::is_secondary_W(t_spawn_W)) {  // the second gun's: along its own barrel (-W +0xa0), its own tracer
        // the drawn map X = P^-1 D (rows): P the shot's own matrix (W +0x80 now), only when it is this update's placement (a
        // refused placement leaves the holster's matrix there, and secondary_placed the last one: the review), D as drawn
        alignas(16) float X[16], P[16], Pw[16], Pi[16], m[16], mm[16], nv[4] = {0, 0, 0, v[3]}, at[4] = {0, 0, 0, 0};
        body::SecondDrawn sd;
        bool drawn_map = false, at_ok = false;
        if (g_shot2_drawn.load(std::memory_order_relaxed) && mtx && body::second_drawn(&sd) && !sd.copy && sd.W == t_spawn_W &&
            body::secondary_placed(t_spawn_W, P) && raw(t_spawn_W + 0x80, Pw, sizeof(Pw)) && raw(reinterpret_cast<uintptr_t>(mtx), m, sizeof(m))) {
            float dmax = 0.0f;  // W +0x80 against the placement: its axes and its place (rows 0-3, xyz)
            for (int row = 0; row < 4; ++row)
                for (int k = 0; k < 3; ++k) dmax = std::fmax(dmax, std::fabs(Pw[row * 4 + k] - P[row * 4 + k]));
            if (dmax < 2e-3f && rows_inverse(Pw, Pi)) {
                rows_mul(Pi, sd.m, X);
                drawn_map = true;
                for (int i = 0; i < 16; ++i) drawn_map = drawn_map && std::isfinite(X[i]);
                // (the shot from the placed gun, the gun drawn near it: X's own translation is a world one, large far from the origin)
                const float sx = m[12] - Pw[12], sy = m[13] - Pw[13], sz = m[14] - Pw[14];
                drawn_map = drawn_map && sx * sx + sy * sy + sz * sz < 1.0f && move_shot(X, m, mm, nv, at, &at_ok, sd.m);
            }
        }
        const float* use_v = drawn_map ? nv : v;
        const float sl = std::sqrt(use_v[0] * use_v[0] + use_v[1] * use_v[1] + use_v[2] * use_v[2]);
        const float vl0 = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float sd_[3] = {0, 0, 0}, gd[3] = {0, 0, 0}, bz[3] = {0, 0, 0}, st[3] = {0, 0, 0};
        if (sl > 1e-3f)
            for (int k = 0; k < 3; ++k) sd_[k] = use_v[k] / sl;
        if (vl0 > 1e-3f)
            for (int k = 0; k < 3; ++k) gd[k] = v[k] / vl0;  // the game's own shot
        if (drawn_map)
            for (int k = 0; k < 3; ++k) st[k] = mm[12 + k];
        else if (mtx)
            raw(reinterpret_cast<uintptr_t>(mtx) + 48, st, sizeof(st));
        auto off_deg = [](const float* s, const float* z) {  // the shot s's angle to the barrel -z
            const double bl = std::sqrt(static_cast<double>(z[0]) * z[0] + static_cast<double>(z[1]) * z[1] + static_cast<double>(z[2]) * z[2]);
            if (!(bl > 1e-6)) return -1.0;
            const double b0 = -z[0] / bl, b1 = -z[1] / bl, b2 = -z[2] / bl;
            const double cx = s[1] * b2 - s[2] * b1, cy = s[2] * b0 - s[0] * b2, cz = s[0] * b1 - s[1] * b0;
            return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), s[0] * b0 + s[1] * b1 + s[2] * b2) * 57.29577951308232;
        };
        // its barrel: the drawn one when the shot is on it, else (as before) -W +0xa0 (read here: the game thread's own);
        // and the game's own shot against that placed barrel (the log's second figure)
        const double game_err = raw(t_spawn_W + 0xa0, bz, sizeof(bz)) ? off_deg(gd, bz) : -1.0;
        const double err = drawn_map ? off_deg(sd_, sd.m + 8) : off_deg(sd_, bz);
        if (t_sec_pellets++ == 0) {
            t_sec_first = err;
            t_sec_placed = game_err;
            t_sec_mapped = drawn_map;
            std::memcpy(t_sec_start, st, sizeof(st));
            std::memcpy(t_sec_dir, sd_, sizeof(sd_));
            t_sec_speed = sl;
        }
        if (err > t_sec_max) t_sec_max = err;
        if (drawn_map) {
            o_launch(p, mm, wtype, nv, owner, at_ok ? at : launch, flags, c8, f9, voff);
            return;
        }
        o_launch(p, mtx, wtype, vel, owner, launch, flags, c8, f9, voff);
        return;
    }
    g_shots.fetch_add(1, std::memory_order_relaxed);
    const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (vl > 1e-3f)
        for (int k = 0; k < 3; ++k) d[k] = v[k] / vl;
    float start[3] = {0, 0, 0};
    if (mtx) raw(reinterpret_cast<uintptr_t>(mtx) + 48, start, sizeof(start));
    const bool fresh = log::now_ms() - g_last_override_ms.load(std::memory_order_relaxed) < 500.0;
    double err = -1;
    if (fresh) {  // atan2(|d x b|, d.b) in double: acos in float resolves only about 0.02 degree
        const double b0 = g_last_dir[0], b1 = g_last_dir[1], b2 = g_last_dir[2];
        const double cx = d[1] * b2 - d[2] * b1, cy = d[2] * b0 - d[0] * b2, cz = d[0] * b1 - d[1] * b0;
        err = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), d[0] * b0 + d[1] * b1 + d[2] * b2) * 57.29577951308232;
    }
    for (int k = 0; k < 3; ++k) {
        g_shot_start[k] = start[k];
        g_shot_dir[k] = d[k];
    }
    g_shot_err = static_cast<float>(err);
    if (t_in_spawn) {  // a pellet of the spawn's shot: summarised in one line after it (hk_spawn)
        if (t_pellets++ == 0) {
            t_first_err = err;
            t_first_fresh = fresh;
            t_first_speed = vl;
            std::memcpy(t_first_start, start, sizeof(start));
            std::memcpy(t_first_dir, d, sizeof(d));
        }
        if (err > t_max_err) t_max_err = err;
    } else {
        log::info("[aim] player shot: start (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) speed %.0f; barrel ray %s (%.2f deg off)", start[0], start[1],
                  start[2], d[0], d[1], d[2], vl, fresh ? "on" : "off", err);
    }
    if (fresh && g_tracer.load(std::memory_order_relaxed) && launch) {
        alignas(16) float at[4] = {g_last_muzzle[0], g_last_muzzle[1], g_last_muzzle[2], 0.0f};
        raw(reinterpret_cast<uintptr_t>(launch) + 12, &at[3], sizeof(float));  // keep its w
        g_tracers.fetch_add(1, std::memory_order_relaxed);
        o_launch(p, mtx, wtype, vel, owner, at, flags, c8, f9, voff);
        return;
    }
    o_launch(p, mtx, wtype, vel, owner, launch, flags, c8, f9, voff);
}

// FUN_140302a50 (research\run3\accuracy.md 3.1, 7): one shot's projectiles. rcx W, rdx the first projectile, r8d the
// count, r9 the pellet matrices (count x 64 bytes, read only); the stack: the speed, an unused dword, a no-bloom byte
// (external fire), a seed, a turret byte. The bloom (W +0x2b4/+0x2b8, drawn once a shot while W +0x9b0 == 0, sized by
// max(W +0x1d0, W +0x1d4)) turns every pellet; the shooter's velocity W +0x110 is added to each.
using Spawn_t = void (*)(uintptr_t W, uintptr_t proj, int32_t count, const float* pellets, float speed, uint32_t u6, uint8_t no_bloom,
                         uint32_t seed, uint8_t turret);
Spawn_t o_spawn = nullptr;

std::atomic<double> g_mounted_shot_ms{-1e12};  // the player's last mounted gun shot (mounted_gun_fired)
void hk_spawn(uintptr_t W, uintptr_t proj, int32_t count, const float* pellets, float speed, uint32_t u6, uint8_t no_bloom, uint32_t seed,
              uint8_t turret) {
    g_spawns.fetch_add(1, std::memory_order_relaxed);
    // the player's own gun shots only: normal fire, the game's own player test (as hk_launch's), not the lasso or a throw
    uintptr_t X = 0, info = 0;
    uint32_t me = 0, owner = 0;
    uint8_t live = 0;
    int16_t wtype = -1;
    if (no_bloom || turret || !rd(anchors::addr(anchors::Id::PlayerInfo), &X) || !X || !rd(X + 0x5e9, &live) || !live || !rd(X + 0x5ec, &me) ||
        !rd(W + 0x20, &owner) || owner != me || !rd(W + 0x28, &info) || !info || !rd(info + 8, &wtype) || wtype == 21 ||
        gestures::is_thrown(wtype)) {
        if (me && owner && owner != me) {  // another's shot, left to the game: its bloom read for the check (run 8 item 0b)
            float b0 = 0.0f, b1 = 0.0f;
            uint8_t drawn = 1;
            g_npc_spawns.fetch_add(1, std::memory_order_relaxed);
            if (rd(W + 0x1d0, &b0) && rd(W + 0x1d4, &b1) && rd(W + 0x9b0, &drawn) && !drawn && (b0 > 0.0f || b1 > 0.0f)) {
                g_npc_bloomed.fetch_add(1, std::memory_order_relaxed);
                g_npc_sigma.store(b0 > b1 ? b0 : b1, std::memory_order_relaxed);
            }
            const double now = log::now_ms();
            if (now - g_npc_log_ms.load(std::memory_order_relaxed) >= 30000.0) {
                g_npc_log_ms.store(now, std::memory_order_relaxed);
                log_npc_shots("");
            }
        }
        o_spawn(W, proj, count, pellets, speed, u6, no_bloom, seed, turret);
        return;
    }
    g_player_spawns.fetch_add(1, std::memory_order_relaxed);
    if (wtype >= 26 && wtype <= 28) {  // (2026-10-13) a mounted gun's shot: the game's own, and the anchor stands down (pose.cpp)
        g_mounted_shot_ms.store(log::now_ms(), std::memory_order_relaxed);
        o_spawn(W, proj, count, pellets, speed, u6, no_bloom, seed, turret);
        return;
    }
    float s0 = 0.0f, s1 = 0.0f;  // the game's bloom state, for the log
    rd(W + 0x1d0, &s0);
    rd(W + 0x1d4, &s1);
    const float sigma = s0 > s1 ? s0 : s1;
    const bool pa = g_perfect.load(std::memory_order_relaxed);
    float vel_saved[3] = {}, dropped = 0.0f, aligned_deg = -1.0f;
    bool vel_zeroed = false, block_fixed = false;
    // [Hands] DualWield: the second gun leaves along its own barrel (dual.cpp latches W +0x9b1); the gun in hand's aim
    // fixes below, (a) and (b), are not for it (a W that was once in hand keeps W +0x9b2, and (b) turned its shot onto
    // the gun in hand's shoot-from row: the simulator, 6.7 degrees off a second sidearm's barrel)
    const bool second = dual::is_secondary_W(W);
    {  // run 7 item 1e: the muzzle-blocked flip at this shot (W +0x9b1 = 1, +0x9b2 = 0), counted whatever PerfectAccuracy is
        uint8_t f1 = 0, f2 = 0;
        if (rd(W + 0x9b1, &f1) && rd(W + 0x9b2, &f2) && f1 && !f2) {
            g_flips_seen.fetch_add(1, std::memory_order_relaxed);
            if (!pa) g_flips_left.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (pa) {
        const bool marks = dead_eye_firing_marks() || second;
        const bool fresh = g_barrel.load(std::memory_order_relaxed) && pose::anchor_active() &&
                           log::now_ms() - g_last_override_ms.load(std::memory_order_relaxed) < 500.0;
        uint8_t b1 = 0, b2 = 0, b3 = 0, b8 = 0, fl = 0;
        rd(W + 0x9b1, &b1);
        rd(W + 0x9b2, &b2);
        rd(W + 0x9b3, &b3);
        rd(W + 0x9b8, &b8);
        rd(info + 0x408, &fl);
        // (a) the muzzle-blocked flip against the animated gun (DoProbeCheck: the animated muzzle in a wall, the VR one not)
        if (!marks && fresh && (fl & 1) && !b3 && !b8 && b1 && wr<uint8_t>(W + 0x9b1, 0) && wr<uint8_t>(W + 0x9b2, 1)) {
            b2 = 1;
            block_fixed = true;
            g_block_fixes.fetch_add(1, std::memory_order_relaxed);
        }
        // (b) the shot along this frame's shoot-from row (the barrel ray in first person); not the bow (its arc)
        float c[3], dcur[3];
        if (!marks && b2 && wtype != 30 && raw(W + 0xf0, c, 12) && raw(W + 0x920, dcur, 12)) {
            const float l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (l > 0.5f && l < 2.0f) {  // also rejects NaN
                const float nd[3] = {-c[0] / l, -c[1] / l, -c[2] / l};
                const float lc = std::sqrt(dcur[0] * dcur[0] + dcur[1] * dcur[1] + dcur[2] * dcur[2]);
                if (lc > 1e-6f) {
                    const float dt = (nd[0] * dcur[0] + nd[1] * dcur[1] + nd[2] * dcur[2]) / lc;
                    aligned_deg = std::acos(dt > 1.0f ? 1.0f : dt < -1.0f ? -1.0f : dt) * 57.29578f;
                }
                for (int k = 0; k < 3; ++k) wr<float>(W + 0x920 + 4 * k, nd[k]);
                g_aligned.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // (c) no bloom: the game's own neutral state for the shot (FUN_1403019b0 resets both after this timeline pass)
        if (wr<uint64_t>(W + 0x2b4, 0) && wr<uint8_t>(W + 0x9b0, 1)) g_bloom_zeroed.fetch_add(1, std::memory_order_relaxed);
        // (d) no inherited shooter velocity for this call (put back after it)
        if (raw(W + 0x110, vel_saved, 12)) {
            dropped = std::sqrt(vel_saved[0] * vel_saved[0] + vel_saved[1] * vel_saved[1] + vel_saved[2] * vel_saved[2]);
            vel_zeroed = wr<uint64_t>(W + 0x110, 0) && wr<uint32_t>(W + 0x118, 0);
            if (vel_zeroed && dropped > 0.01f) g_speed_drops.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // ShotgunPattern off: a straightened copy of the pellet matrices (rows a b c to the identity, row d and w kept)
    alignas(16) float copy[32 * 16];
    const float* use = pellets;
    const bool straight = !g_pattern.load(std::memory_order_relaxed) && count > 1 && count <= 32 && pellets &&
                          raw(reinterpret_cast<uintptr_t>(pellets), copy, static_cast<size_t>(count) * 64);
    if (straight) {
        for (int i = 0; i < count; ++i) {
            float* m = copy + i * 16;
            m[0] = 1, m[1] = 0, m[2] = 0, m[4] = 0, m[5] = 1, m[6] = 0, m[8] = 0, m[9] = 0, m[10] = 1;
        }
        use = copy;
        g_straightened.fetch_add(1, std::memory_order_relaxed);
    }
    t_in_spawn = true;
    t_pellets = 0;
    t_first_err = t_max_err = -1.0;
    t_spawn_W = W;
    t_sec_pellets = 0;
    t_copy_pellets = 0;
    t_sec_first = t_sec_max = -1.0;
    t_sec_placed = -1.0;
    t_sec_mapped = false;
    o_spawn(W, proj, count, use, speed, u6, no_bloom, seed, turret);
    t_in_spawn = false;
    t_spawn_W = 0;
    if (t_sec_pellets > 0) {  // [Hands] DualWield: the second gun's shot, one line
        g_sec_shots.fetch_add(1, std::memory_order_relaxed);
        g_sec_err.store(static_cast<float>(t_sec_max), std::memory_order_relaxed);
        char extra[64] = "";
        if (t_sec_pellets > 1) std::snprintf(extra, sizeof(extra), ", %d pellets, the widest %.2f deg", t_sec_pellets, t_sec_max);
        // (round 38 check 2) "its barrel": the drawn one when the shot is on the drawn map, else the placed one (-W +0xa0)
        log::info("[wield] second gun shot: start (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) speed %.0f; its barrel (%.2f deg off)%s | weapon %d, the "
                  "game's bloom %.3f deg, perfect %d | the drawn map %d, the game's shot %.2f deg off its placed barrel",
                  t_sec_start[0], t_sec_start[1], t_sec_start[2], t_sec_dir[0], t_sec_dir[1], t_sec_dir[2], t_sec_speed, t_sec_first, extra, wtype,
                  sigma * 57.29578f, pa ? 1 : 0, t_sec_mapped ? 1 : 0, t_sec_placed);
        (t_sec_mapped ? g_shot2_mapped : g_shot2_unmapped).fetch_add(1, std::memory_order_relaxed);
    }
    if (vel_zeroed)
        for (int k = 0; k < 3; ++k) wr<float>(W + 0x110 + 4 * k, vel_saved[k]);  // the owner's velocity back
    float off[2] = {};
    raw(W + 0x2b4, off, sizeof(off));  // this shot's bloom offsets (still there; FUN_1403019b0 clears them later)
    // one line per shot ("player shot: ... (X deg off)" as before, the first pellet's), the widest pellet for a shotgun
    if (t_pellets > 0) {
        g_player_shot_n.fetch_add(1, std::memory_order_relaxed);
        char extra[96] = "";
        if (t_pellets > 1) std::snprintf(extra, sizeof(extra), ", %d pellets, the widest %.2f deg", t_pellets, t_max_err);
        log::info("[aim] player shot: start (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) speed %.0f; barrel ray %s (%.2f deg off)%s | weapon %d, the "
                  "game's bloom %.3f deg (offsets %.4f %.4f), perfect %d (muzzle fix %d, aligned %.3f deg, %.2f m/s left out), pattern %d",
                  t_first_start[0], t_first_start[1], t_first_start[2], t_first_dir[0], t_first_dir[1], t_first_dir[2], t_first_speed,
                  t_first_fresh ? "on" : "off", t_first_err, extra, wtype, sigma * 57.29578f, off[0], off[1], pa ? 1 : 0, block_fixed ? 1 : 0,
                  aligned_deg, vel_zeroed ? dropped : 0.0f, straight ? 0 : 1);
    }
}

// FUN_1403480a0(player info, weapon manager): the fire trigger; it fires (and executes Dead Eye) only while the
// camera on channel 0 ([[CamManager +8] +0x18] +0x10) is the gameplay camera ([CamManager +0x28]).
using Fire_t = void (*)(uintptr_t info, uintptr_t wmgr);

}  // namespace

std::atomic<bool> g_fire_swap{false};  // in hk_fire: channel 0 may be on the gameplay camera for its call (not the game's own)
std::atomic<int> g_other_swaps{0};     // another hook's swap for one call (deadeye.cpp's reticle UI update): a count, so
                                       // hk_fire's flag and this one never clear each other

void camera_swap_flag(bool on) { g_other_swaps.fetch_add(on ? 1 : -1, std::memory_order_acq_rel); }

bool gameplay_camera_current() {
    if (g_fire_swap.load(std::memory_order_acquire) || g_other_swaps.load(std::memory_order_acquire) > 0) return false;
    uintptr_t cm = 0, chs = 0, ch = 0, cur = 0, gp = 0;
    return rd(anchors::addr(anchors::Id::CamManager), &cm) && cm && rd(cm + 8, &chs) && chs && rd(chs + 0x18, &ch) && ch &&
           rd(ch + 0x10, &cur) && rd(cm + 0x28, &gp) && gp && cur == gp;
}

namespace {
Fire_t o_fire = nullptr;

void hk_fire(uintptr_t info, uintptr_t wmgr) {
    g_fires.fetch_add(1, std::memory_order_relaxed);
    struct SwapFlag {  // (the plugin's camera job must not read the swap below as the game's own gameplay camera)
        SwapFlag() { g_fire_swap.store(true, std::memory_order_release); }
        ~SwapFlag() { g_fire_swap.store(false, std::memory_order_release); }
    } swap_flag;
    uint8_t reload_before = 0;  // the reload request byte: the empty trigger sets it (reload.cpp holds that back)
    rd(info + 0x248, &reload_before);
    uintptr_t cm = 0, chs = 0, ch = 0, cur = 0, gp = 0;
    if (!g_fire.load(std::memory_order_relaxed) || !pose::anchor_active() || !rd(anchors::addr(anchors::Id::CamManager), &cm) ||
        !rd(cm + 8, &chs) || !rd(chs + 0x18, &ch) || !ch || !rd(ch + 0x10, &cur) || !rd(cm + 0x28, &gp) || !gp || cur == gp ||
        !wr<uintptr_t>(ch + 0x10, gp)) {
        o_fire(info, wmgr);
        reload::after_fire_trigger(info, reload_before);
        return;
    }
    o_fire(info, wmgr);
    wr<uintptr_t>(ch + 0x10, cur);
    g_swaps.fetch_add(1, std::memory_order_relaxed);
    reload::after_fire_trigger(info, reload_before);
}

// FUN_14034ba30(player info, ped): picks the soft-lock tuning each frame and stores it at T +0x57c0 (T = the
// player actor +0xb8); soft lock and the reticle magnet (which moves the target point onto a nearby actor and forces
// the hit) both need it set. Cleared after it in first person unless [Hands] AimAssist.
using SoftLock_t = void (*)(uintptr_t info, uintptr_t ped);
SoftLock_t o_soft_lock = nullptr;

void hk_soft_lock(uintptr_t info, uintptr_t ped) {
    o_soft_lock(info, ped);
    if (g_assist.load(std::memory_order_relaxed) || !pose::anchor_active()) return;
    {
        RdrvrActorState st{};
        // the lasso catches what it locks on (along the hand ray); fists, the knife and the torch lock on to hit
        if (api::actor_state(&st) && (st.weapon == 21 || st.weapon < 0 || st.weapon == 22 || st.weapon == 33)) return;
    }
    uint16_t idx = 0;
    uintptr_t pool = 0, actor = 0, T = 0, tuning = 0;
    if (!rd(info + 0x5ec, &idx) || !rd(anchors::addr(anchors::Id::ActorPool), &pool) || !pool || !rd(pool + idx * 0x10u, &actor) ||
        !actor || !rd(actor + 0xb8, &T) || !T || !rd(T + 0x57c0, &tuning) || !tuning)
        return;
    if (wr<uintptr_t>(T + 0x57c0, 0)) g_assist_off.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void init() {
    g_barrel = config::get_bool("Hands", "BarrelAim", true);
    g_fire = config::get_bool("Hands", "FireInFirstPerson", true);
    g_assist = config::get_bool("Hands", "AimAssist", false);
    g_tracer = config::get_bool("Hands", "TracerFromMuzzle", true);
    g_perfect = config::get_bool("Aim", "PerfectAccuracy", true);
    g_pattern = config::get_bool("Aim", "ShotgunPattern", true);
    g_reticle = config::get_bool("Hands", "Reticle", false);
    g_ret_smooth = config::get_bool("Hands", "ReticleSmooth", true);  // run 10 item 8
    g_reticle_dot = config::get_string("Hands", "ReticleStyle", "ring") == "dot";
    {
        const float s = config::get_float("Hands", "ReticleSize", 1.2f);
        g_reticle_deg = !(s >= 0.2f) ? 0.2f : s > 5.0f ? 5.0f : s;
    }
    log::info("[aim] shots from the barrel %d, fire in first person %d, aim assist %d, tracer from the muzzle %d", g_barrel.load() ? 1 : 0,
              g_fire.load() ? 1 : 0, g_assist.load() ? 1 : 0, g_tracer.load() ? 1 : 0);
}

// Round 8: "the weapon is thrown a moment after I let go" and "a delay between when I swing and when the hit lands".
// Both are the game's own clip timing: a throw leaves the hand at its clip's release phase (FUN_140d198f0 case 13
// against G +0x558), a punch hits in its strike window (M +0x11c .. +0x120, frames / 30 / rate). For the player only,
// [Gestures] QuickThrow scales the release phase (ThrowRelease) and the clip's rate (ThrowRate) at the throw's start, and
// MeleeStrike scales the strike window at the punch's start (1 = the game's timing). Game thread, inside the game's
// own calls: the arguments changed, or the fields the game just wrote rewritten (SEH-guarded).
using ThrowStart_t = uint8_t (*)(uintptr_t g, void* clip, float release, float rate, float phase_in, float phase_out, float draw_model, uint32_t focus);
ThrowStart_t o_throw_start = nullptr;
using MeleeStart_t = uint64_t (*)(uintptr_t self);
MeleeStart_t o_melee_start = nullptr;
std::atomic<bool> g_quick_throw{true};
std::atomic<float> g_throw_release{0.15f}, g_throw_rate{1.5f}, g_melee_strike{0.35f};
std::atomic<uint64_t> g_quick_throws{0}, g_quick_punches{0};
std::atomic<uintptr_t> g_probe_m{0};     // the punch's melee controller, until its strike is seen (melee_probe)
std::atomic<double> g_probe_ms{0.0};     // the punch's start (log::now_ms)
std::atomic<float> g_probe_k{1.0f};      // the strike scale that punch used
std::atomic<uint64_t> g_strikes_seen{0};

// Round 13: "when John gets close to an enemy while holding a gun and you press right trigger, a third person animation
// plays with an execution style attack". The shot request FUN_140d1f960 asks FUN_140adebe0 (the player's phys) first,
// in the local player's branch only; true runs a scan for a target within 2.5 m in front, and a hit sets G +0x5dc (the
// action tree's ConditionExecuteTargetSet: the executions, the pistol whip, the butt strike) and skips the shot. False
// at that one call goes straight to the normal shot, the scan's own "no target" path (research\round13\execution-block.md).
using ExecGate_t = bool (*)(uintptr_t phys);
ExecGate_t o_exec_gate = nullptr;
std::atomic<bool> g_block_exec{true};
std::atomic<uint64_t> g_exec_seen{0}, g_exec_blocked{0};

// Run 7 item 1e ("unable to shoot when right behind cover"; research\run7\cover-climb.md A.2): the shot request
// FUN_140d1f960 drops a request (no shot, no ammo) at its end (a) for the arm block, the game's "gun against a wall":
// FUN_140d1e640 sweeps from John's shoulder height toward the barrel ray's target point, as far as John's animated gun
// reaches, and a hit sets G +0x400 = 1 (reason G +0x410 = 2); once G +0x404 has eased above 0 the request is dropped;
// (b) in the game's cover (C +0x18 != 0), when the target point is less than 1 m beyond John's root along the barrel
// (FUN_14039dee0). The gate hook runs inside the request before both: the fields read there (SEH), the drops
// predicted as the game's code would take them, counted and logged, only while "aim cover log on" (and only with
// BlockExecutions on: see rdrvr_exec_gate_impl).
struct ReqDiag {
    uint64_t seq = 0;
    double ms = 0;
    float g400 = 0, g404 = 0, g40c = 0, g3c4 = 0;
    uint8_t g410 = 0, g450 = 0, g5d4 = 0, g5d5 = 0, g5d6 = 0, g5d8 = 0, g5ea = 0, t57e4 = 0, t6088 = 0;
    int32_t g3bc = -1, cover = -1;
    uint32_t cbc = 0;
    float origin[3] = {}, c[3] = {}, target[3] = {}, start[3] = {}, root[3] = {};
    float t3840 = 0, depth = 0;
    bool arm_drop = false, cover_drop = false;
};
ReqDiag g_req;
std::mutex g_req_mutex;
std::atomic<bool> g_req_log{false};
std::atomic<uint64_t> g_reqs{0}, g_arm_drops{0}, g_cover_drops{0};

void note_shot_request() {
    ReqDiag d;
    const uintptr_t actor = player_actor();
    uintptr_t ped = 0, comp = 0, G = 0, C = 0, T = 0, phys = 0, pm = 0;
    if (!actor || !rd(actor + 0x38, &ped) || !ped || !rd(ped + 0xaa8, &comp) || !comp || !rd(comp + 8, &G) || !G) return;
    rd(G + 0x400, &d.g400);
    rd(G + 0x404, &d.g404);
    rd(G + 0x40c, &d.g40c);
    rd(G + 0x3c4, &d.g3c4);
    rd(G + 0x410, &d.g410);
    rd(G + 0x450, &d.g450);
    rd(G + 0x5d4, &d.g5d4);
    rd(G + 0x5d5, &d.g5d5);
    rd(G + 0x5d6, &d.g5d6);
    rd(G + 0x5d8, &d.g5d8);
    rd(G + 0x5ea, &d.g5ea);
    rd(G + 0x3bc, &d.g3bc);
    if (rd(comp + 0x18, &C) && C) {
        rd(C + 0x18, &d.cover);
        rd(C + 0xbc, &d.cbc);
    }
    if (rd(actor + 0xb8, &T) && T) {
        rd(T + 0x57e4, &d.t57e4);
        rd(T + 0x6088, &d.t6088);
        raw(T + 0x3740, d.origin, sizeof(d.origin));
        raw(T + 0x3730, d.c, sizeof(d.c));
        raw(T + 0x3810, d.target, sizeof(d.target));
        raw(T + 0x3830, d.start, sizeof(d.start));
        rd(T + 0x3840, &d.t3840);
    }
    if (rd(actor + 0xb0, &phys) && phys && rd(phys + 0x18, &pm) && pm) raw(pm + 0x30, d.root, sizeof(d.root));
    // FUN_14039dee0: depth(p) = -(p - origin) . c; the rule wants depth(target) - depth(root) >= 1.0
    d.depth = -((d.target[0] - d.root[0]) * d.c[0] + (d.target[1] - d.root[1]) * d.c[1] + (d.target[2] - d.root[2]) * d.c[2]);
    d.arm_drop = d.g404 != 0.0f && !(d.g450 & 0x10) && d.g400 > 0.0f;
    d.cover_drop = !d.arm_drop && !(d.g5ea & 1) && d.cover != 0 && !(d.t57e4 && d.depth >= 1.0f);
    d.seq = g_reqs.fetch_add(1, std::memory_order_relaxed) + 1;
    d.ms = log::now_ms();
    if (d.arm_drop) g_arm_drops.fetch_add(1, std::memory_order_relaxed);
    if (d.cover_drop) g_cover_drops.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_req_mutex);
        g_req = d;
    }
    if (g_req_log.load(std::memory_order_relaxed) || d.arm_drop || d.cover_drop)
        log::info("[aim] shot request %llu: %s | arm %.2f/%.2f reason %u reach %.2f flags 450=0x%x | cover %d (bc 0x%x, 5ea 0x%x) depth %.2f | "
                  "gun state %d raise %.2f 5d4-6/8 %02x %02x %02x %02x | T armed %u no-shoot %u | origin (%.2f %.2f %.2f) target (%.2f %.2f %.2f) "
                  "start (%.2f %.2f %.2f) %.2f | root (%.2f %.2f %.2f)",
                  static_cast<unsigned long long>(d.seq), d.arm_drop ? "DROPPED by the arm block" : d.cover_drop ? "DROPPED by the 1 m cover rule" : "goes on",
                  d.g400, d.g404, d.g410, d.g40c, d.g450, d.cover, d.cbc, d.g5ea, d.depth, d.g3bc, d.g3c4, d.g5d4, d.g5d5, d.g5d6, d.g5d8, d.t57e4,
                  d.t6088, d.origin[0], d.origin[1], d.origin[2], d.target[0], d.target[1], d.target[2], d.start[0], d.start[1], d.start[2],
                  d.t3840, d.root[0], d.root[1], d.root[2]);
}

// The gate's hook is reached through exec_gate_thunk.asm (rdrvr_exec_gate_stub): the gate is a leaf the shot request
// keeps r9 across (0x140d1fa28), so the stub saves the volatile registers around this C++ part and passes the game's
// return address (run 7: with the readback added here, r9 was clobbered and the game crashed on the gate's true path).
extern "C" void rdrvr_exec_gate_stub();
extern "C" bool rdrvr_exec_gate_impl(uintptr_t phys, uintptr_t ret) {
    if (ret == anchors::addr(anchors::Id::ExecGateRet)) {
        g_exec_seen.fetch_add(1, std::memory_order_relaxed);
        // run 7 item 1e's readback, only while a test asks for it ("aim cover log on") and only on the blocked path:
        // with it on the gate's true path (BlockExecutions off) the game hung in the simulator (cycle N: the main
        // thread waiting on the render thread, cause not traced); without it the true path passes (cycle O). Off,
        // this hook is as it was before run 7, the stub keeping the registers
        const bool block = g_block_exec.load(std::memory_order_relaxed);
        if (block && g_req_log.load(std::memory_order_relaxed)) note_shot_request();
        if (block) {
            g_exec_blocked.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return o_exec_gate(phys);
}

// [Hands] ShootPastArmBlock (run 7 item 1e): FUN_140d1e640(G), the arm block, sweeps from John's (the game body's)
// shoulder toward the barrel ray's target point; in VR the drawn gun is the player's, held over a wall John's
// shoulder line is inside (BodyFollowsHead=0 leaves him where he stands), so every pull was dropped there. After the
// game's own update, a block from that sweep (reason G +0x410 = 2) is cleared for the player while the barrel ray is
// fresh; the game's other reasons (1: a friendly's "ArmUp" or the no-shoot target, 3, 4: its own states) are kept.
using ArmBlock_t = uint64_t (*)(uintptr_t G);
ArmBlock_t o_arm_block = nullptr;
std::atomic<bool> g_past_arm_block{false};
std::atomic<uint64_t> g_arm_cleared{0};
int player_melee_state() {  // run 10 item 7: the player's melee controller's state ([[ped +0xaa8] +0x80] +0xc0): 1 a punch, 2 blocking
    const uintptr_t actor = player_actor();
    uintptr_t ped = 0, comp = 0, m = 0;
    int32_t st = -1;
    return actor && rd(actor + 0x38, &ped) && ped && rd(ped + 0xaa8, &comp) && comp && rd(comp + 0x80, &m) && m && rd(m + 0xc0, &st) ? st : -1;
}

LassoProbe lasso_probe() {  // (item 12) read only; see aim.h
    LassoProbe p;
    uintptr_t info = 0;
    if (rd(anchors::addr(anchors::Id::PlayerInfo), &info) && info) rd(info + 0x244, &p.aim_state);
    const uintptr_t actor = player_actor();
    uintptr_t ped = 0, comp = 0, L = 0, lasso = 0, T = 0;
    if (!actor) return p;
    p.ok = true;
    if (rd(actor + 0x38, &ped) && ped && rd(ped + 0xaa8, &comp) && comp && rd(comp + 0x40, &L) && L) {
        rd(L + 0x18, &p.user_state);
        if (rd(L + 0x30, &lasso) && lasso) {
            rd(lasso + 0x3d0, &p.lasso_state);
            rd(lasso + 0xf0, &p.target);
            rd(lasso + 0xf8, &p.candidate);
        }
    }
    if (rd(actor + 0xb8, &T) && T) {
        rd(T + 0x3900, &p.reticle);
        rd(T + 0x3914, &p.lock);
        rd(T + 0x3920, &p.soft);
    }
    return p;
}

uintptr_t player_gun_ctl() {  // the player's G: [[actor +0x38 (ped)] +0xaa8] +8
    uintptr_t ped = 0, comp = 0, G = 0;
    const uintptr_t actor = player_actor();
    return actor && rd(actor + 0x38, &ped) && ped && rd(ped + 0xaa8, &comp) && comp && rd(comp + 8, &G) ? G : 0;
}
uint64_t hk_arm_block(uintptr_t G) {
    const uint64_t r = o_arm_block(G);
    if (!g_past_arm_block.load(std::memory_order_relaxed)) return r;
    uint8_t reason = 0;
    float up = 0.0f;
    if (rd(G + 0x410, &reason) && reason == 2 && rd(G + 0x400, &up) && up > 0.0f && pose::anchor_active() &&
        log::now_ms() - g_last_override_ms.load(std::memory_order_relaxed) < 500.0 && G == player_gun_ctl() && wr<float>(G + 0x400, 0.0f))
        g_arm_cleared.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// Run 7 item 1f ("can't seem to climb over things"; research\run7\cover-climb.md B): the game vaults when its traversal
// probe (along the stick's heading, else John's facing) finds an edge it accepts (L +0x1d0 bit 0, VaultIncoming; from
// farther than its tune's distance only at a jog or faster) and the jump node turns that into VaultRequested (bit 1);
// L +0xf8 is then the ledge state (8 the vault). K = [ped +0xaa8]: L = K +0x48, the locomotion K +0x58 (+0x16f4 the
// movement state, +0x17b8 the speed), G = K +8 (+0x5d6 bit 6: the aim pose); ped +0xc6f: jumping; the mover actor
// +0x88 (+0x930 an edge near, +0x70 the position). Read on demand ("aim ledge") and, traced, at the frame end.
struct LedgeRead {
    bool ok = false;
    int32_t state = -1, move = -1;
    uint8_t b1d0 = 0, b1d1 = 0, b1d2 = 0, jumping = 0, aim = 0;
    float dist = 0, speed = 0, root[3] = {}, normal[3] = {}, point[3] = {};
    bool edge_near = false;
};
LedgeRead read_ledge() {
    LedgeRead r;
    const uintptr_t actor = player_actor();
    uintptr_t ped = 0, K = 0, L = 0, loco = 0, G = 0, mover = 0, edge = 0;
    if (!actor || !rd(actor + 0x38, &ped) || !ped || !rd(ped + 0xaa8, &K) || !K || !rd(K + 0x48, &L) || !L) return r;
    rd(L + 0xf8, &r.state);
    rd(L + 0x1d0, &r.b1d0);
    rd(L + 0x1d1, &r.b1d1);
    rd(L + 0x1d2, &r.b1d2);
    rd(L + 0x1a4, &r.dist);
    raw(L + 0x110, r.normal, sizeof(r.normal));
    raw(L + 0x120, r.point, sizeof(r.point));
    if (rd(K + 0x58, &loco) && loco) {
        rd(loco + 0x16f4, &r.move);
        rd(loco + 0x17b8, &r.speed);
    }
    uint8_t fl = 0;
    if (rd(K + 8, &G) && G && rd(G + 0x5d6, &fl)) r.aim = (fl & 0x40) ? 1 : 0;
    rd(ped + 0xc6f, &r.jumping);
    if (rd(actor + 0x88, &mover) && mover) {
        raw(mover + 0x70, r.root, sizeof(r.root));
        r.edge_near = rd(mover + 0x930, &edge) && edge;
    }
    r.ok = true;
    return r;
}
std::atomic<bool> g_ledge_trace{false};
std::atomic<uint64_t> g_ledge_frames{0}, g_vault_incoming{0}, g_vault_requested{0}, g_ledge_states{0};
std::atomic<float> g_ledge_rise{0.0f}, g_ledge_step{0.0f};  // the root's rise in a traced window, its largest step a frame
void ledge_frame() {
    if (!g_ledge_trace.load(std::memory_order_relaxed)) return;
    static uint64_t last_x = 0, win_x = 0;
    static float y0 = 0, last[3] = {};
    static bool in = false;
    const uint64_t xt = xinput::x_press_tick(), now = GetTickCount64();
    const LedgeRead r = read_ledge();
    if (!r.ok) return;
    const bool window = (xt && now - xt < 2000) || r.state != 0;
    if (!window) {
        in = false;
        return;
    }
    if (!in || xt != win_x) {  // a new window (a press): its start
        in = true;
        win_x = xt;
        y0 = r.root[1];
        std::memcpy(last, r.root, sizeof(last));
        g_ledge_rise = 0.0f;
        g_ledge_step = 0.0f;
        log::info("[ledge] X pressed (%llu so far): state %d vault bits 0x%02x 0x%02x 0x%02x, edge near %d dist %.2f, move %d speed %.2f, aim %u, "
                  "root (%.3f %.3f %.3f)",
                  static_cast<unsigned long long>(xinput::x_presses()), r.state, r.b1d0, r.b1d1, r.b1d2, r.edge_near ? 1 : 0, r.dist, r.move, r.speed,
                  r.aim, r.root[0], r.root[1], r.root[2]);
    }
    (void)last_x;
    const float dx = r.root[0] - last[0], dy = r.root[1] - last[1], dz = r.root[2] - last[2];
    const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
    std::memcpy(last, r.root, sizeof(last));
    // the view's jump in a climb: the root's vertical step a frame (the walk's own travel is not one)
    if (std::fabs(dy) > g_ledge_step.load(std::memory_order_relaxed)) g_ledge_step = std::fabs(dy);
    if (r.root[1] - y0 > g_ledge_rise.load(std::memory_order_relaxed)) g_ledge_rise = r.root[1] - y0;
    g_ledge_frames.fetch_add(1, std::memory_order_relaxed);
    if (r.b1d0 & 1) g_vault_incoming.fetch_add(1, std::memory_order_relaxed);
    if (r.b1d0 & 2) g_vault_requested.fetch_add(1, std::memory_order_relaxed);
    if (r.state != 0) g_ledge_states.fetch_add(1, std::memory_order_relaxed);
    log::info("[ledge] +%llu ms: state %d bits 0x%02x 0x%02x 0x%02x edge %d %.2f move %d %.2f jump %u aim %u y %+.3f step %.3f",
              static_cast<unsigned long long>(now - xt), r.state, r.b1d0, r.b1d1, r.b1d2, r.edge_near ? 1 : 0, r.dist, r.move, r.speed, r.jumping,
              r.aim, r.root[1] - y0, step);
}

bool is_player_ped(uintptr_t ped) {
    uintptr_t actor = 0;
    uint8_t fl = 0;
    return ped && rd(ped + 0x10, &actor) && actor && rd(actor + 0x118, &fl) && (fl & 3) == 3;
}

// [Gestures] ThrowByGrip: the throw held from its start (the trigger, the grip held) until the grip is let go
std::atomic<bool> g_grip_throw{false};
std::atomic<uintptr_t> g_hold_G{0};
std::atomic<bool> g_armed_pub{false};  // the pad thread's g_armed (below) for grip_throw_busy (another reader)
std::atomic<double> g_player_throw_ms{-1e12};  // (the off-hand build step 2, the review) the player's last throw clip start, whatever the options
std::atomic<float> g_hold_phase{0.0f};
std::atomic<double> g_hold_since{0.0};
float g_hold_t = -1.0f;  // the clip's time pinned (the game thread)
std::atomic<uint64_t> g_holds{0}, g_hold_releases{0}, g_hold_timeouts{0};
constexpr double kHoldMax = 15000.0;  // a held throw let go by itself after this (ms)
std::atomic<double> g_aim_tail_until{0.0};  // the game's LT held after a held throw's let-go (its release, the launch)
constexpr double kAimTail = 900.0;
std::atomic<uint64_t> g_aim_frames{0}, g_arms{0};
std::atomic<int> g_hold_weapon{-1};  // the eWeapon of the held throw
// the throwing knife by its tip while its throw is held ([Gestures] KnifeTip, KnifeTipTurn, KnifeTipPivot)
std::atomic<bool> g_knife_tip{true};
std::atomic<int> g_tip_force{0};  // the test: 1 always turned, -1 never, 0 by the held throw
std::atomic<int> g_tip_axis{2};
float g_tip_pivot[3] = {0.0f, 0.1f, 0.0f};
std::atomic<double> g_armed_ms{0.0};  // when the trigger armed the throw (the pad's thread)
std::mutex g_tip_mutex;
std::atomic<uint64_t> g_tip_frames{0};
using ThrowPhase_t = float (*)(uintptr_t ped);

float call_phase(uintptr_t fn, uintptr_t ped) {  // the game's own getter (a read only), SEH-guarded
    __try {
        return reinterpret_cast<ThrowPhase_t>(fn)(ped);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

bool grip_held() {
    const hands::Hand hh = hands::get(controls::gun_hand());
    return hh.grip > 0.5f;
}

// once a frame on the game thread (the visibility build, after the game's update): a held throw's clip pinned before
// its release; the grip let go: the release at G's next update. No hook on G's update itself: with one there (a pure
// pass-through while nothing was held) the hip holster's second draw failed (reg2).
double game_throw_launch_ms() { return g_game_throw_launch_ms.load(std::memory_order_relaxed); }
std::string throw_flight() {
    std::lock_guard lock(g_track_mx);
    return g_track_last;
}
void throw_hold_tick() {
    track_tick();  // (C13) a thrown projectile's flight
    const uintptr_t g = g_hold_G.load(std::memory_order_relaxed);
    if (g) {
        uint32_t state = 0;
        uintptr_t ped = 0, cp = 0, player = 0;
        const bool live = rd(g + 0x3bc, &state) && state == 13 && rd(g + 8, &ped) && ped && rd(ped + 800, &cp) && cp && rd(cp + 0x10, &player) && player;
        const bool timeout = log::now_ms() - g_hold_since.load(std::memory_order_relaxed) > kHoldMax;
        if (!live) {
            g_hold_G.store(0, std::memory_order_relaxed);  // the throw over (or not the throw): nothing held
            g_hold_t = -1.0f;
            g_aim_tail_until.store(log::now_ms() + kAimTail, std::memory_order_relaxed);
        } else if (!grip_held() || timeout) {
            wr(g + 0x558, 0.0f);  // the release at this update's check
            g_hold_G.store(0, std::memory_order_relaxed);
            g_hold_t = -1.0f;
            g_aim_tail_until.store(log::now_ms() + kAimTail, std::memory_order_relaxed);
            (timeout ? g_hold_timeouts : g_hold_releases).fetch_add(1, std::memory_order_relaxed);
            g_grip_letgo_ms.store(timeout ? 0.0 : log::now_ms(), std::memory_order_relaxed);
            log::info("[gestures] the held throw let go%s", timeout ? " (held too long)" : " (the grip)");
        } else {
            const float ph = call_phase(anchors::addr(anchors::Id::ThrowPhase), ped);
            if (g_hold_t < 0.0f) {
                if (ph >= g_hold_phase.load(std::memory_order_relaxed)) rd(player + 0x38, &g_hold_t);  // pinned from here
            } else {
                wr(player + 0x38, g_hold_t);
            }
        }
    }
}

// [Gestures] ThrowByGrip: the gun hand's grip is the throwable's (not the game's button) while one is in that hand
bool grip_throw_wanted(int ctrl) {
    if (!g_grip_throw.load(std::memory_order_relaxed) || ctrl != controls::gun_hand() || !pose::anchor_active()) return false;
    RdrvrActorState st{};
    return api::actor_state(&st) && gestures::is_thrown(st.weapon);
}
bool grip_throw() { return g_grip_throw.load(std::memory_order_relaxed); }
bool grip_throw_busy() {
    const double now = log::now_ms();
    return g_armed_pub.load(std::memory_order_relaxed) || g_hold_G.load(std::memory_order_relaxed) != 0 ||
           now < g_aim_tail_until.load(std::memory_order_relaxed) || now - g_throw_start_ms.load(std::memory_order_relaxed) < 1500.0 ||
           now - g_player_throw_ms.load(std::memory_order_relaxed) < 1500.0;
}
// 2026-10-13 (the user: the Gatling "spinning around with no way to stop"): the player shot a mounted gun (the Gatling,
// the Browning, the cannon) within `ms`: the turret is the game's (GET_WEAPON_IN_HAND may not name it)
uint64_t player_shots() { return g_player_shot_n.load(std::memory_order_relaxed); }
bool mounted_gun_fired(double ms) { return log::now_ms() - g_mounted_shot_ms.load(std::memory_order_relaxed) < ms; }
bool mounted_shot(float d[3], double* ms) {
    std::lock_guard lock(g_mounted_mutex);
    if (g_mounted_dir_ms < 0.0) return false;
    std::memcpy(d, g_mounted_dir, sizeof(g_mounted_dir));
    *ms = g_mounted_dir_ms;
    return true;
}
bool knife_tip(int* axis, float pivot[3]) {
    const int f = g_tip_force.load(std::memory_order_relaxed);
    const bool on = f > 0 || (f == 0 && g_knife_tip.load(std::memory_order_relaxed) && g_grip_throw.load(std::memory_order_relaxed) &&
                              g_hold_G.load(std::memory_order_relaxed) != 0 && g_hold_weapon.load(std::memory_order_relaxed) == 25);
    if (!on) return false;
    *axis = g_tip_axis.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(g_tip_mutex);
        for (int k = 0; k < 3; ++k) pivot[k] = g_tip_pivot[k];
    }
    g_tip_frames.fetch_add(1, std::memory_order_relaxed);
    return true;
}
std::string tip_command(const std::string& w, const std::string& v) {
    if (w == "tip") g_tip_force.store(v == "on" ? 1 : v == "off" ? -1 : 0);
    if (w == "tipaxis") g_tip_axis.store(std::atoi(v.c_str()) % 3);
    if (w == "tippivot") {
        float p[3] = {};
        if (std::sscanf(v.c_str(), "%f,%f,%f", &p[0], &p[1], &p[2]) == 3) {
            std::lock_guard lock(g_tip_mutex);
            for (int k = 0; k < 3; ++k) g_tip_pivot[k] = p[k];
        }
    }
    char b[200];
    std::lock_guard lock(g_tip_mutex);
    std::snprintf(b, sizeof(b), " | grip throw %d, arms %llu holds %llu let go %llu timeouts %llu, aim frames %llu | knife tip %d force %d axis %d pivot (%.3f %.3f %.3f) frames %llu",
                  g_grip_throw.load() ? 1 : 0, static_cast<unsigned long long>(g_arms.load()), static_cast<unsigned long long>(g_holds.load()), static_cast<unsigned long long>(g_hold_releases.load()),
                  static_cast<unsigned long long>(g_hold_timeouts.load()), static_cast<unsigned long long>(g_aim_frames.load()), g_knife_tip.load() ? 1 : 0,
                  g_tip_force.load(), g_tip_axis.load(), g_tip_pivot[0], g_tip_pivot[1], g_tip_pivot[2], static_cast<unsigned long long>(g_tip_frames.load()));
    return b;
}
// the pad's thread: the grip holds the throwable unlit; the trigger with the grip held arms it (the game's LT: its aim,
// which lights dynamite and fire bottles), its RT held back for kRtLead (the throw starts only from the aim) and then a
// press of kRtPress at least; LT kept through the held throw and its tail
bool g_armed = false;
double g_armed_at = 0.0;

constexpr double kRtLead = 120.0, kRtPulse = 100.0, kRtTries = 2000.0;
void grip_throw_input(float* lt, float* rt) {
    if (!g_grip_throw.load(std::memory_order_relaxed)) {
        g_armed = false;
        g_armed_pub.store(false, std::memory_order_relaxed);
        return;
    }
    const double now = log::now_ms();
    const bool live = g_hold_G.load(std::memory_order_relaxed) != 0 || now < g_aim_tail_until.load(std::memory_order_relaxed);
    const bool held = grip_held() && grip_throw_wanted(controls::gun_hand());
    if (held && !g_armed && *rt > 0.15f) {
        g_armed = true;
        g_armed_at = now;
        g_armed_ms.store(now, std::memory_order_relaxed);
        g_arms.fetch_add(1, std::memory_order_relaxed);
    }
    if (!held) g_armed = false;  // let go (a held throw goes on by `live`), or no throwable
    g_armed_pub.store(g_armed, std::memory_order_relaxed);
    if (!g_armed && !live) return;
    *lt = 1.0f;
    g_aim_frames.fetch_add(1, std::memory_order_relaxed);
    // RT: held back for the lead, then pressed in pulses until the game's throw starts (its light comes first)
    if (g_armed && !g_hold_G.load(std::memory_order_relaxed)) {
        const double t = now - g_armed_at;
        if (t < kRtLead) *rt = 0.0f;
        else if (t < kRtLead + kRtTries) *rt = std::fmod(t - kRtLead, 2.0 * kRtPulse) < kRtPulse ? 1.0f : 0.0f;
    }
}
void set_grip_throw(bool on, bool save) {
    if (g_grip_throw.exchange(on) != on) log::info("[gestures] throw by letting go of the grip: %s", on ? "on" : "off");
    if (save) config::set("Gestures", "ThrowByGrip", on ? "1" : "0");
}

uint8_t hk_throw_start(uintptr_t g, void* clip, float release, float rate, float phase_in, float phase_out, float draw_model, uint32_t focus) {
    uintptr_t ped = 0;
    if (rd(g + 8, &ped) && is_player_ped(ped)) g_player_throw_ms.store(log::now_ms(), std::memory_order_relaxed);  // (grip_throw_busy: any player throw)
    ped = 0;
    // (2026-11-01, GOAL-offhand-fixes F1; round 62 check 3: the lasso's throw pinned at 0.45 by this, never released, its
    // start's sound over and over) only a throwable's throw is held by the grip: the lasso's (21, held by HoldItems' grip)
    // goes as the game's
    RdrvrActorState tst{};
    const bool throwable = g_armed_pub.load(std::memory_order_relaxed) || (api::actor_state(&tst) && offhand::kind_of(tst.weapon) == offhand::kThrown);  // (the review: armed too)
    if (g_grip_throw.load(std::memory_order_relaxed) && pose::anchor_active() && rd(g + 8, &ped) && is_player_ped(ped) && grip_held() && throwable &&
        release > 0.0f && release <= 1.0f) {
        // held: pinned just before the game's own release and after its model change (the lighting), released by the grip
        const float hold = std::fmax(std::fmax(0.02f, release * 0.9f), draw_model > 0.0f && draw_model < release ? draw_model + 0.02f : 0.0f);
        g_hold_phase.store(std::fmin(hold, release - 0.01f), std::memory_order_relaxed);
        g_hold_since.store(log::now_ms(), std::memory_order_relaxed);
        g_hold_t = -1.0f;
        {
            RdrvrActorState st{};
            g_hold_weapon.store(api::actor_state(&st) ? st.weapon : -1, std::memory_order_relaxed);
        }
        g_hold_G.store(g, std::memory_order_relaxed);
        g_holds.fetch_add(1, std::memory_order_relaxed);
        g_throw_start_ms.store(log::now_ms(), std::memory_order_relaxed);
        log::info("[gestures] the throw held (the grip): release phase %.2f -> out of reach, held at %.2f (the model's change at %.2f), %.0f ms "
                  "after the trigger", release, g_hold_phase.load(), draw_model, log::now_ms() - g_armed_ms.load());
        return o_throw_start(g, clip, 5.0f, rate, phase_in, phase_out, draw_model, focus);
    }
    // (2026-11-01, GOAL-offhand-changes C12, C11b; round 65 checks 14 and 18, the user: the lasso kills as soon as it touches
    // them, and most throws stop before hitting anything; cycle I1, r96: every lasso throw went through this, released at 0.08
    // of its clip for 0.50 at 1.5 times its speed, its loop taking a stranger's handle in flight or the stranger's health 0 at
    // once) a throwable's only, as [Gestures] QuickThrow was made for (round 8: "the weapon leaves a moment after I let go"):
    // the lasso's throw on the game's own timing
    if (g_quick_throw.load(std::memory_order_relaxed) && pose::anchor_active() && throwable && tst.weapon != 21 && rd(g + 8, &ped) && is_player_ped(ped)) {
        const float r0 = release, k0 = rate;
        if (release > 0.0f && release <= 1.0f) {
            release *= g_throw_release.load(std::memory_order_relaxed);
            if (release < 0.02f) release = 0.02f;
        }
        if (rate > 0.0f && rate < 10.0f) rate *= g_throw_rate.load(std::memory_order_relaxed);
        g_throw_start_ms.store(log::now_ms(), std::memory_order_relaxed);
        g_quick_throws.fetch_add(1, std::memory_order_relaxed);
        log::info("[gestures] throw start: release phase %.2f -> %.2f, rate %.2f -> %.2f", r0, release, k0, rate);
    }
    return o_throw_start(g, clip, release, rate, phase_in, phase_out, draw_model, focus);
}

uint64_t hk_melee_start(uintptr_t self) {
    const uint64_t r = o_melee_start(self);
    const float k = g_melee_strike.load(std::memory_order_relaxed);
    if (!(r & 0xff) || !(k > 0.0f && k <= 1.0f) || !pose::anchor_active()) return r;
    uintptr_t def = 0, ctx = 0, ped = 0, comp = 0, m = 0;
    int32_t type = -1, st = 0;
    float s = -1.0f, e = 1.0f;
    if (!rd(self + 0x18, &def) || !def || !rd(def + 0x18, &type) || type != 0 || !rd(self + 0x20, &ctx) || !ctx || !rd(ctx + 0x30, &ped) ||
        !is_player_ped(ped) || !rd(ped + 0xaa8, &comp) || !comp || !rd(comp + 0x80, &m) || !m || !rd(m + 0xc0, &st) || st != 1 ||
        !rd(m + 0x11c, &s) || !(s >= 0.0f && s <= 1.0f) || !rd(m + 0x120, &e) || !(e >= s && e <= 1.0f))
        return r;
    gun_melee::note_punch(m);  // run 7 item 2: the game's melee force scale at a punch, logged (GunMelee on only)
    g_probe_ms.store(log::now_ms(), std::memory_order_relaxed);  // the probe: this punch's strike, timed (melee_probe)
    g_probe_k.store(k, std::memory_order_relaxed);
    g_probe_m.store(m, std::memory_order_release);
    if (k >= 1.0f) {
        log::info("[gestures] punch: strike phase %.2f..%.2f (the game's)", s, e);
        return r;
    }
    const float s2 = s * k, e2 = e * k > s2 + 0.001f ? e * k : s2 + 0.001f;
    wr<float>(m + 0x11c, s2);
    wr<float>(m + 0x120, e2);
    g_quick_punches.fetch_add(1, std::memory_order_relaxed);
    log::info("[gestures] punch: strike phase %.2f..%.2f -> %.2f..%.2f", s, e, s2, e2);
    return r;
}

void melee_probe(double swing_ms) {
    const uintptr_t m = g_probe_m.load(std::memory_order_acquire);
    if (!m) return;
    const double now = log::now_ms(), t0 = g_probe_ms.load(std::memory_order_relaxed);
    uint8_t fl = 0;
    if (!rd(m + 0x554, &fl)) {  // gone
        g_probe_m.store(0, std::memory_order_relaxed);
        return;
    }
    // bit 0x04: the clip's phase reached the strike start (M +0x11c; FUN_140d007f0 sets it then). Bit 0x10 is set on the
    // player's first update (an early pick of the targets), not at the strike.
    if (fl & 0x04) {
        g_probe_m.store(0, std::memory_order_relaxed);
        g_strikes_seen.fetch_add(1, std::memory_order_relaxed);
        log::info("[gestures] strike %.0f ms after the punch's start, %.0f ms after the swing (MeleeStrike %.2f)", now - t0,
                  swing_ms > 0.0 && swing_ms <= t0 ? now - swing_ms : -1.0, g_probe_k.load(std::memory_order_relaxed));
    } else if (now - t0 > 2000.0) {
        g_probe_m.store(0, std::memory_order_relaxed);
        log::info("[gestures] no strike within 2 s of the punch's start");
    }
}
void set_melee_strike(float k) { g_melee_strike = !(k >= 0.1f) ? 0.1f : k > 1.0f ? 1.0f : k; }
float melee_strike() { return g_melee_strike.load(); }

bool launch_hooked() { return o_launch != nullptr; }

bool install() {
    api::set_tick_hook(reticle_tick);  // the reticle's barrel probe, each script tick
    g_quick_throw = config::get_bool("Gestures", "QuickThrow", true);
    g_grip_throw = config::get_bool("Gestures", "ThrowByGrip", false);
    g_drop_lit = config::get_bool("Gestures", "DropLitSlow", true);  // (2026-10-31, step 5)
    g_knife_tip = config::get_bool("Gestures", "KnifeTip", true);
    g_tip_axis = config::get_int("Gestures", "KnifeTipTurn", 2) % 3;
    {
        float p[3] = {};
        if (std::sscanf(config::get_string("Gestures", "KnifeTipPivot", "0 0.1 0").c_str(), "%f %f %f", &p[0], &p[1], &p[2]) == 3)
            for (int k = 0; k < 3; ++k) g_tip_pivot[k] = p[k];
    }
    g_throw_release = config::get_float("Gestures", "ThrowRelease", 0.15f);
    g_throw_rate = config::get_float("Gestures", "ThrowRate", 1.5f);
    g_melee_strike = config::get_float("Gestures", "MeleeStrike", 0.35f);
    g_block_exec = config::get_bool("Hands", "BlockExecutions", true);
    g_past_arm_block = config::get_bool("Hands", "ShootPastArmBlock", false);
    {
        const float a = g_throw_release.load(), b = g_throw_rate.load(), c = g_melee_strike.load();
        g_throw_release = !(a >= 0.05f) ? 0.05f : a > 1.0f ? 1.0f : a;
        g_throw_rate = !(b >= 0.5f) ? 0.5f : b > 3.0f ? 3.0f : b;
        g_melee_strike = !(c >= 0.1f) ? 0.1f : c > 1.0f ? 1.0f : c;
    }
    log::info("[gestures] the throw's release at %.2f of the game's phase, its clip x%.2f (%d); the punch's strike at %.2f of the game's",
              g_throw_release.load(), g_throw_rate.load(), g_quick_throw.load() ? 1 : 0, g_melee_strike.load());
    bool ok = hooks::install("RDR throw start (quick throw)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ThrowStart)), hk_throw_start,
                             &o_throw_start);
    ok &= hooks::install("RDR melee track start (quick punch)", reinterpret_cast<void*>(anchors::addr(anchors::Id::MeleeTrackStart)), hk_melee_start,
                         &o_melee_start);
    ok &= hooks::install("RDR reticle ray (aim)", reinterpret_cast<void*>(anchors::addr(anchors::Id::AimRay)), hk_aim_ray, &o_aim_ray);
    ok &= hooks::install("RDR fire trigger (aim)", reinterpret_cast<void*>(anchors::addr(anchors::Id::FireTrigger)), hk_fire, &o_fire);
    ok &= hooks::install("RDR execution gate (aim)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ExecGate)),
                         reinterpret_cast<void*>(&rdrvr_exec_gate_stub), reinterpret_cast<void**>(&o_exec_gate));
    log::info("[aim] close-range executions on the trigger blocked: %d", g_block_exec.load() ? 1 : 0);
    ok &= hooks::install("RDR arm block (aim)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ArmBlock)), hk_arm_block, &o_arm_block);
    log::info("[aim] shots past the game's arm block (its shoulder-line probe): %d", g_past_arm_block.load() ? 1 : 0);
    d3d::add_frame_end_listener([](uint64_t) { ledge_frame(); });  // run 7 item 1f: "aim ledge trace" (off: one flag check)
    ok &= hooks::install("RDR soft-lock tuning (aim)", reinterpret_cast<void*>(anchors::addr(anchors::Id::SoftLockSelect)), hk_soft_lock,
                         &o_soft_lock);
    ok &= hooks::install("RDR projectile launch (aim)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ProjectileLaunch)), hk_launch,
                         &o_launch);
    // run 3 item 3: the spawn (one shot's projectiles), for PerfectAccuracy and ShotgunPattern; without it both are unavailable
    const bool sp = hooks::install("RDR projectile spawn (perfect accuracy)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ProjectileSpawn)),
                                   hk_spawn, &o_spawn);
    g_spawn_hooked = sp;
    log::info("[aim] perfect accuracy %d, shotgun pattern %d (the spawn hook %s)", g_perfect.load() ? 1 : 0, g_pattern.load() ? 1 : 0,
              sp ? "installed" : "NOT installed: both unavailable");
    ok &= sp;
    return ok;
}

bool barrel_aim() { return g_barrel.load(); }

bool reticle_on() { return g_reticle.load(std::memory_order_relaxed); }
void set_reticle_on(bool on) {
    if (g_reticle.exchange(on) != on) log::info("[aim] the reticle where the shot lands: %s", on ? "on" : "off");
    config::set("Hands", "Reticle", on ? "1" : "0");
}

bool reticle_dot() { return g_reticle_dot.load(std::memory_order_relaxed); }
void set_reticle_dot(bool dot) {
    if (g_reticle_dot.exchange(dot) != dot) log::info("[aim] the reticle's style: %s", dot ? "a dot" : "a ring and a dot");
    config::set("Hands", "ReticleStyle", dot ? "dot" : "ring");
}

// run 10 item 8 (the user: "Reticle is a bit shaky, and only one appears when dual wielding"): the probe's ray is a script
// tick or two old (one probe every other tick) and was drawn as it was, in steps behind the hand, the hand's own tremor
// carried 300 m out. [Hands] ReticleSmooth: each frame the barrel's line from the drawn gun hand's IK target (its axes and
// the barrel's measured axis in them, barrel_in_target) through the muzzle moved with the hand since the probe, the
// direction smoothed by speed (50 ms at rest down to 4 ms past 90 deg/s: a sweep keeps up), the distance eased to each
// probe's (80 ms). The second gun (dual wield) gets its own line: its hand's target and the barrel axis mirrored, at the
// same distance. "reticle stats": each frame's turn of the drawn reticle (its jitter) and its angle off the barrel now.
struct RetFilter {
    bool ok = false;
    float dir[3] = {0, 0, -1}, raw[3] = {0, 0, -1}, dist = 0.0f, pos[3] = {};
    double ms = 0.0;
    bool have_pos = false;
};
RetFilter g_rf[2];  // written by the XR thread (advance) and read by the others, both under g_rp_mutex
std::atomic<bool> g_ret2_line_on{true};  // "aim reticle line on|off" (the session): off, the old mirrored hold (the tests' control)
std::atomic<uint64_t> g_ret2_line{0};  // frames the second reticle took its shot's own line (the copy's T, the second gun's barrel)
struct RetStats {
    uint64_t n = 0;
    double jit_sum2 = 0.0, jit_max = 0.0, off_sum = 0.0, off_max = 0.0;
    float last_dir[3] = {};
    bool have = false;
};
RetStats g_rs[2];
float ang_deg(const float* a, const float* b) {
    const float d = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0f, 1.0f);
    return std::acos(d) * 57.29578f;
}
void norm3(float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f)
        for (int k = 0; k < 3; ++k) v[k] /= l;
}
// (2026-10-28, round 38 check 2) "aim barrelstep2 [on|off|reset]" (the session; reads only): each XR frame of the second
// reticle, its direction against the second gun (or the copy's prop) as DRAWN in the same body frame (body::drawn_second,
// captured only while this is on): the new way's (the drawn gun's barrel carried by its hand's target, below) and the
// old line's (the copy's: always, it reads no game memory; another slot's gun: only while the old line is the one in
// use, "aim reticle drawn2 off", as it reads W +0xa0 from this thread), each one's largest step from the frame before,
// and the second hand's target's largest turn a frame (is the hand still?); the new way's muzzle against the drawn one's.
std::atomic<bool> g_bs2_on{false};
struct BarrelSteps2 {
    uint64_t frames = 0, compared = 0, skipped = 0, old_frames = 0, old_compared = 0, steps = 0, old_steps = 0;
    uint64_t new_over = 0, old_over = 0;  // (over: a step over 2 degrees)
    double new_off = 0.0, old_off = 0.0, new_mm = 0.0, new_step = 0.0, old_step = 0.0, hand = 0.0;
    float d_new[3] = {}, d_old[3] = {}, R[9] = {};
    bool have = false, have_old = false;
    // (2026-10-29, the review: on a frame the target and the draw share, the new way is the published gun itself, so its
    // carry by the hand's turn is not compared there) the carry's own figure: each frame, the frame before's published
    // gun carried by the hand's target now (carry_drawn, the reticle's own code) against this frame's draw; beside it the
    // same gun uncarried (the control: off by the turn) and the target's turn between the two frames
    uint64_t carry_n = 0, carry_mm_n = 0;  // (the muzzle's own count: measured only with the probe's offset, drawn2 on)
    double carry_off = 0.0, carry_mm = 0.0, carry0_off = 0.0, carry0_mm = 0.0, carry_turn = 0.0;  // (carry0_mm: moved only, not turned)
    // the last two frames' published guns, the carry's pair (here, so "reset" clears them; cleared too while the new way's
    // line is not in use: never a stale pair)
    body::SecondDrawn last, prev;
};
BarrelSteps2 g_bs2;  // under g_rp_mutex (the XR thread writes, the command reads)
std::atomic<float> g_bt2[3];  // the second gun's barrel in its own hand's IK target's axes (the last frame's)
std::atomic<uint64_t> g_ret2_drawn{0};  // frames the second reticle took the drawn gun's line
// The second gun as drawn (sd) carried by its hand's target now (R: the target's axes as columns, hand its place): the
// barrel in sd's target axes (b2, g_bt2's) turned by R; with mo, the muzzle (that offset on the drawn gun) rigid with the
// hand: m = hand + R R_sd^T (muzzle_sd - t_sd), its turn as well as its move (2026-10-29: the move alone left the turn's
// swing of the muzzle about the wrist, some mm a frame in a sweep). False when not finite. The second reticle's line, and
// barrelstep2's check of it
bool carry_drawn(const body::SecondDrawn& sd, const float* R, const float* hand, const float* mo, float d[3], float m[3], float b2[3]) {
    float bd[3] = {-sd.m[8], -sd.m[9], -sd.m[10]};
    norm3(bd);
    for (int i = 0; i < 3; ++i) b2[i] = sd.R[0 * 3 + i] * bd[0] + sd.R[1 * 3 + i] * bd[1] + sd.R[2 * 3 + i] * bd[2];  // R_sd^T bd
    for (int k = 0; k < 3; ++k) d[k] = R[k * 3] * b2[0] + R[k * 3 + 1] * b2[1] + R[k * 3 + 2] * b2[2];
    norm3(d);
    if (!std::isfinite(d[0] + d[1] + d[2])) return false;
    if (!mo) return true;
    float off[3], q[3];  // the drawn muzzle from the hand's target then, in that target's axes
    for (int k = 0; k < 3; ++k) off[k] = sd.m[12 + k] + sd.m[k] * mo[0] + sd.m[4 + k] * mo[1] + sd.m[8 + k] * mo[2] - sd.t[k];
    for (int i = 0; i < 3; ++i) q[i] = sd.R[0 * 3 + i] * off[0] + sd.R[1 * 3 + i] * off[1] + sd.R[2 * 3 + i] * off[2];
    for (int k = 0; k < 3; ++k) m[k] = hand[k] + R[k * 3] * q[0] + R[k * 3 + 1] * q[1] + R[k * 3 + 2] * q[2];
    return std::isfinite(m[0] + m[1] + m[2]);
}
double turn_deg(const float* A, const float* B) {  // the turn between two rotations: acos((tr(A^T B) - 1) / 2)
    double tr = 0.0;
    for (int i = 0; i < 9; ++i) tr += static_cast<double>(A[i]) * B[i];
    return std::acos(std::clamp((tr - 1.0) * 0.5, -1.0, 1.0)) * 57.29577951308232;
}
// one frame of barrelstep2 (under g_rp_mutex): d_new, m_new the new way's line (m_new null: not in use, its muzzle not
// compared), d_old the old line's when have_old, sd the draw published, mo the muzzle offset on it (null with m_new null)
void steps2_note(int j, const body::BodyPoints& bp, const float* d_new, const float* m_new, const float* d_old, bool have_old,
                 const body::SecondDrawn& sd, const float* mo) {
    float DR[9], Do[3];
    uint64_t fr = 0;
    bool cp = false;
    const bool drawn = body::drawn_second(DR, Do, &fr, &cp) && cp == sd.copy;
    // (the new way and the old line against the draw only when the hand's target is the same publish's as it: two reads
    // straddling a publish carry a moving hand's frame of turn; the carry itself is checked below, against bp's own frame)
    const bool dok = drawn && fr == sd.frame && bp.frame == sd.frame;
    BarrelSteps2& s = g_bs2;
    {  // the carry: the frame before's published gun (same gun, same hand) carried by this frame's target, on this frame's draw,
       // only while the new way's line is in use (mo: drawn2), from a frame one or two before (the review: no stale pair)
        body::SecondDrawn& s_last = s.last;
        body::SecondDrawn& s_prev = s.prev;
        if (!mo) {
            s_last = s_prev = body::SecondDrawn{};
        } else if (sd.frame != s_last.frame) {
            s_prev = s_last;
            s_last = sd;
        }
        if (mo && drawn && fr == bp.frame && s_prev.frame && s_prev.frame < bp.frame && bp.frame - s_prev.frame <= 2 && s_prev.W == sd.W &&
            s_prev.john == j) {
            float dc[3], mc[3], b2[3];
            const float bz[3] = {-DR[2], -DR[5], -DR[8]};
            if (carry_drawn(s_prev, bp.target_rot[j], bp.hand[j], mo, dc, mc, b2)) {
                float d0[3] = {-s_prev.m[8], -s_prev.m[9], -s_prev.m[10]};
                norm3(d0);
                ++s.carry_n;
                s.carry_off = std::fmax(s.carry_off, step_deg(dc, bz));
                s.carry0_off = std::fmax(s.carry0_off, step_deg(d0, bz));
                s.carry_turn = std::fmax(s.carry_turn, turn_deg(s_prev.R, bp.target_rot[j]));
                float mz[3], m0[3];  // the draw's muzzle; the control: the old carry, the muzzle moved with the hand only (not turned)
                for (int k = 0; k < 3; ++k) {
                    mz[k] = Do[k] + DR[k * 3] * mo[0] + DR[k * 3 + 1] * mo[1] + DR[k * 3 + 2] * mo[2];
                    m0[k] = s_prev.m[12 + k] + s_prev.m[k] * mo[0] + s_prev.m[4 + k] * mo[1] + s_prev.m[8 + k] * mo[2] + (bp.hand[j][k] - s_prev.t[k]);
                }
                auto mm_off = [&](const float* a) {
                    return 1000.0 * std::sqrt(static_cast<double>(a[0] - mz[0]) * (a[0] - mz[0]) + static_cast<double>(a[1] - mz[1]) * (a[1] - mz[1]) +
                                              static_cast<double>(a[2] - mz[2]) * (a[2] - mz[2]));
                };
                ++s.carry_mm_n;
                s.carry_mm = std::fmax(s.carry_mm, mm_off(mc));
                s.carry0_mm = std::fmax(s.carry0_mm, mm_off(m0));
            }
        }
    }
    ++s.frames;
    if (have_old) ++s.old_frames;
    if (!dok) {
        ++s.skipped;
    } else {
        const float bz[3] = {-DR[2], -DR[5], -DR[8]};  // the drawn root's -z (axis 2's components)
        ++s.compared;
        s.new_off = std::fmax(s.new_off, step_deg(d_new, bz));
        if (m_new) {
            float mz[3];
            for (int k = 0; k < 3; ++k) mz[k] = Do[k] + DR[k * 3] * mo[0] + DR[k * 3 + 1] * mo[1] + DR[k * 3 + 2] * mo[2];
            const double e = std::sqrt(static_cast<double>(m_new[0] - mz[0]) * (m_new[0] - mz[0]) + static_cast<double>(m_new[1] - mz[1]) * (m_new[1] - mz[1]) +
                                       static_cast<double>(m_new[2] - mz[2]) * (m_new[2] - mz[2]));
            s.new_mm = std::fmax(s.new_mm, e * 1000.0);
        }
        if (have_old) {
            ++s.old_compared;
            s.old_off = std::fmax(s.old_off, step_deg(d_old, bz));
        }
    }
    const float* R = bp.target_rot[j];
    if (s.have) {
        ++s.steps;
        const double sn = step_deg(d_new, s.d_new);
        s.new_step = std::fmax(s.new_step, sn);
        if (sn > 2.0) ++s.new_over;
        s.hand = std::fmax(s.hand, turn_deg(R, s.R));  // the turn between the two targets
    }
    if (have_old && s.have_old) {
        ++s.old_steps;
        const double so = step_deg(d_old, s.d_old);
        s.old_step = std::fmax(s.old_step, so);
        if (so > 2.0) ++s.old_over;
    }
    std::memcpy(s.d_new, d_new, sizeof(s.d_new));
    if (have_old) std::memcpy(s.d_old, d_old, sizeof(s.d_old));
    std::memcpy(s.R, R, sizeof(s.R));
    s.have = true;
    s.have_old = have_old;
}
// rp2: the second gun's own probe (round 38 check 2; which 0 does not take it)
// drawn_only ([Hands] DualWieldBesideTorch, the review of 471af5a): the second reticle on the drawn gun and its own probe
// only, no old-line fallback (that eases to the main probe's distance, stale with the torch in hand: no gun, no main probe)
bool reticle_smoothed(int which, const ReticleProbe& rp, bool advance, float pos[3], const ReticleProbe* rp2 = nullptr, bool drawn_only = false) {
    body::BodyPoints bp;
    if (!body::body_points(&bp)) return false;
    int j = bp.gun;
    if (which == 1) {
        const dual::State ds = dual::state();
        if (!ds.on || ds.carry) return false;  // (a carried long gun has no reticle: it never fires)
        j = dual::copy_john();
        if (j < 0 || j > 1 || j == bp.gun) j = 1 - bp.gun;
    }
    if (!bp.hand_ok[j]) return false;
    const double now = log::now_ms();
    float bt[3] = {0.0f, 0.174f, -0.985f};
    barrel_in_target(bt);
    float d[3], m[3];
    // (2026-10-28, round 38 check 2) the second reticle on the second gun (or the copy) as drawn: its barrel in its own
    // hand's IK target's axes as the frame drew it (g_bt2), turned by that target now; its muzzle the probe's offset on the
    // drawn gun, moved with the hand since that frame; the second probe's own held distance. Not known (no draw published,
    // no fresh second probe, "aim reticle drawn2 off"): the old line below
    body::SecondDrawn sd;
    bool drawn2 = false, have_drawn = false;
    float ddrawn[3] = {0.0f, 0.0f, -1.0f}, mdrawn[3] = {};
    if (which == 1 && body::second_drawn(&sd) && sd.john == j) {
        float b2[3], dm[3];
        have_drawn = carry_drawn(sd, bp.target_rot[j], bp.hand[j], nullptr, ddrawn, dm, b2);
        if (have_drawn)
            for (int k = 0; k < 3; ++k) g_bt2[k].store(b2[k], std::memory_order_relaxed);
        if (have_drawn && rp2 && g_ret2_drawn_on.load(std::memory_order_relaxed) && rp2->ms > 0.0 && now - rp2->ms <= 250.0 && rp2->john == j &&
            rp2->W == sd.W) {
            if (carry_drawn(sd, bp.target_rot[j], bp.hand[j], rp2->mo, dm, mdrawn, b2)) {
                std::memcpy(d, ddrawn, sizeof(d));
                std::memcpy(m, mdrawn, sizeof(m));
                drawn2 = true;
                g_ret2_drawn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (which == 1 && drawn_only && !drawn2) return false;  // (beside the torch: not on its own probe, none drawn)
    // 2026-10-12 (round 16 check 56, the user: "Reticle on second gun in left hand is off aim"): the second reticle on
    // the line its shot takes, not the gun hand's hold mirrored: the copy's shot is the gun in hand's moved by T onto the
    // copy (aim.cpp's launch), so its line is the first reticle's moved by T; a second gun of another slot fires along
    // its own barrel (-W +0xa0). The mirror only when neither is known yet. (Since round 38 check 2: the fallback, and
    // barrelstep2's control.) Another slot's line reads W +0xa0 from this thread: only when it is the line in use
    bool line = drawn2;
    float dl[3] = {}, ml[3] = {};
    bool have_old = false;
    const bool steps2 = which == 1 && advance && g_bs2_on.load(std::memory_order_relaxed) && have_drawn;
    if (which == 1 && g_ret2_line_on.load(std::memory_order_relaxed) && (!drawn2 || steps2)) {
        const dual::State ds = dual::state();
        alignas(16) float T[16];
        const int g = bp.gun;
        if (ds.copy && dual::copy_T(T) && bp.hand_ok[g]) {
            float d0[3], m0[3];
            for (int k = 0; k < 3; ++k)
                d0[k] = bp.target_rot[g][k * 3] * bt[0] + bp.target_rot[g][k * 3 + 1] * bt[1] + bp.target_rot[g][k * 3 + 2] * bt[2];
            norm3(d0);
            if (rp.hand_ok)
                for (int k = 0; k < 3; ++k) m0[k] = rp.muzzle[k] + (bp.hand[g][k] - rp.hand[k]);
            else
                for (int k = 0; k < 3; ++k) m0[k] = bp.hand[g][k] + d0[k] * 0.2f;
            for (int c = 0; c < 3; ++c) {  // p' = p T (row vectors): a point takes the translation row, a direction not
                dl[c] = d0[0] * T[c] + d0[1] * T[4 + c] + d0[2] * T[8 + c];
                ml[c] = m0[0] * T[c] + m0[1] * T[4 + c] + m0[2] * T[8 + c] + T[12 + c];
            }
            norm3(dl);
            have_old = std::isfinite(dl[0] + dl[1] + dl[2] + ml[0] + ml[1] + ml[2]);
        } else if (!ds.copy && !drawn2) {
            int sj = -1;
            const uintptr_t W = dual::secondary_W(&sj);
            float bz[3] = {};
            if (W && raw(W + 0xa0, bz, sizeof(bz))) {
                for (int k = 0; k < 3; ++k) dl[k] = -bz[k];
                norm3(dl);
                for (int k = 0; k < 3; ++k) ml[k] = bp.hand[j][k] + dl[k] * 0.2f;  // a barrel's length past the hand
                have_old = std::isfinite(dl[0] + dl[1] + dl[2]) && dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2] > 0.5f;
            }
        }
        if (!drawn2 && have_old) {
            std::memcpy(d, dl, sizeof(d));
            std::memcpy(m, ml, sizeof(m));
            line = true;
            g_ret2_line.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (which == 1 && !line) bt[0] = -bt[0];  // the other hand's: the mirror of the gun hand's hold
    if (!line) {
        for (int k = 0; k < 3; ++k) d[k] = bp.target_rot[j][k * 3] * bt[0] + bp.target_rot[j][k * 3 + 1] * bt[1] + bp.target_rot[j][k * 3 + 2] * bt[2];
        norm3(d);
        if (!std::isfinite(d[0] + d[1] + d[2])) return false;
        if (which == 0 && rp.hand_ok)
            for (int k = 0; k < 3; ++k) m[k] = rp.muzzle[k] + (bp.hand[j][k] - rp.hand[k]);
        else
            for (int k = 0; k < 3; ++k) m[k] = bp.hand[j][k] + d[k] * 0.2f;  // a barrel's length past the hand
    }
    // the distance eased toward: the second gun's own probe's on the drawn line, else (as before) the main barrel's, and
    // then only while that probe is fresh (the review: reticle_target lets the second through on its own probe alone)
    if (which == 1 && !drawn2 && now - rp.ms > 250.0) return false;
    const float pdist = drawn2 ? rp2->dist : rp.dist;
    std::lock_guard lock(g_rp_mutex);  // the filter and its stats: the XR thread writes, the commands read (the review's)
    if (steps2) steps2_note(j, bp, ddrawn, drawn2 ? mdrawn : nullptr, dl, have_old, sd, drawn2 ? rp2->mo : nullptr);
    else if (which == 1 && advance && g_bs2_on.load(std::memory_order_relaxed)) g_bs2.last = g_bs2.prev = body::SecondDrawn{};  // (no draw: no pair across it)
    RetFilter& f = g_rf[which];
    if (advance) {
        const double dt = (now - f.ms) * 0.001;
        if (!f.ok || dt <= 0.0 || dt > 0.5) {
            std::memcpy(f.dir, d, sizeof(d));
            f.dist = pdist;
        } else {
            const float speed = ang_deg(d, f.raw) / static_cast<float>(dt);  // deg/s
            const float t = std::clamp((speed - 10.0f) / 80.0f, 0.0f, 1.0f);
            const float tau = 0.05f + (0.004f - 0.05f) * t;
            const float a = static_cast<float>(1.0 - std::exp(-dt / tau));
            for (int k = 0; k < 3; ++k) f.dir[k] += (d[k] - f.dir[k]) * a;
            norm3(f.dir);
            const float ad = static_cast<float>(1.0 - std::exp(-dt / 0.08));
            f.dist += (pdist - f.dist) * (std::fabs(pdist - f.dist) > 0.5f * f.dist ? 1.0f : ad);  // a new surface: at once
        }
        std::memcpy(f.raw, d, sizeof(d));
        f.ms = now;
        f.ok = true;
        for (int k = 0; k < 3; ++k) f.pos[k] = m[k] + f.dir[k] * f.dist;
        f.have_pos = true;
        RetStats& s = g_rs[which];
        float rd_[3] = {f.pos[0] - m[0], f.pos[1] - m[1], f.pos[2] - m[2]};
        norm3(rd_);
        if (s.have) {
            const double jit = ang_deg(rd_, s.last_dir);
            s.jit_sum2 += jit * jit;
            s.jit_max = std::fmax(s.jit_max, jit);
        }
        // "off the barrel": the second reticle against the second gun as drawn (carried by its hand's target) when that is
        // known, whichever line it took
        const double off = ang_deg(rd_, which == 1 && have_drawn ? ddrawn : d);
        s.off_sum += off;
        s.off_max = std::fmax(s.off_max, off);
        ++s.n;
        std::memcpy(s.last_dir, rd_, sizeof(rd_));
        s.have = true;
    }
    if (!f.have_pos) return false;
    std::memcpy(pos, f.pos, sizeof(f.pos));
    return true;
}
// the unsmoothed reticle's stats (the control: the probe's own place each frame)
void stats_raw(int which, const float* m, const float* hit) {
    body::BodyPoints bp;
    if (!body::body_points(&bp) || !bp.hand_ok[bp.gun]) return;
    float bt[3] = {0.0f, 0.174f, -0.985f};
    barrel_in_target(bt);
    float d[3];
    const int j = bp.gun;
    for (int k = 0; k < 3; ++k) d[k] = bp.target_rot[j][k * 3] * bt[0] + bp.target_rot[j][k * 3 + 1] * bt[1] + bp.target_rot[j][k * 3 + 2] * bt[2];
    norm3(d);
    std::lock_guard lock(g_rp_mutex);
    RetStats& s = g_rs[which];
    float rd_[3] = {hit[0] - m[0], hit[1] - m[1], hit[2] - m[2]};
    norm3(rd_);
    if (s.have) {
        const double jit = ang_deg(rd_, s.last_dir);
        s.jit_sum2 += jit * jit;
        s.jit_max = std::fmax(s.jit_max, jit);
    }
    const double off = ang_deg(rd_, d);
    s.off_sum += off;
    s.off_max = std::fmax(s.off_max, off);
    ++s.n;
    std::memcpy(s.last_dir, rd_, sizeof(rd_));
    s.have = true;
}

bool reticle_target(float pos[3], bool* on_actor, float* size_deg, int which, bool advance) {
    if (!g_reticle.load(std::memory_order_relaxed) || !g_barrel.load(std::memory_order_relaxed)) return false;
    RdrvrActorState st{};
    // not the lasso, a thrown weapon or the fists; [Hands] DualWieldBesideTorch: the second's (which 1) beside the torch too
    // (the main reticle stays off with the torch: its probe runs for a gun only)
    if (!api::actor_state(&st) || !(reload::is_gun(st.weapon) || (which == 1 && dual::beside_torch(st.weapon)))) return false;
    const bool torch2 = which == 1 && !reload::is_gun(st.weapon);  // the second beside the torch: its own probe only
    // the barrel's own probe (any time a gun is in hand: lowered, raised, in Dead Eye), fresh within 250 ms
    ReticleProbe rp, rp2;
    {
        std::lock_guard lock(g_rp_mutex);
        rp = g_rp;
        if (which == 1) rp2 = g_rp2;
    }
    const double now = log::now_ms();
    // (round 38 check 2: the second reticle also on its own probe alone)
    if (now - rp.ms > 250.0 && (which != 1 || now - rp2.ms > 250.0)) return false;
    if (torch2 && now - rp2.ms > 250.0) return false;  // (the main probe is no gun's now: never its distance)
    float hit[3];
    for (int k = 0; k < 3; ++k) hit[k] = rp.muzzle[k] + rp.dir[k] * rp.dist;
    if (which == 1) {  // the second gun's: smoothed only, never red
        if (!g_ret_smooth.load(std::memory_order_relaxed) || !reticle_smoothed(1, rp, advance, pos, &rp2, torch2)) return false;
        if (on_actor) *on_actor = false;
        if (size_deg) *size_deg = g_reticle_deg.load(std::memory_order_relaxed);
        return true;
    }
    if (g_ret_smooth.load(std::memory_order_relaxed) && reticle_smoothed(0, rp, advance, pos)) {
        // (red below judged on the probe's own hit, as before)
    } else {
        std::memcpy(pos, hit, sizeof(hit));
        if (advance) stats_raw(0, rp.muzzle, hit);
    }
    // red: only while the game aims along the barrel (its aim ray fresh), at its current target handle, and with the
    // probe's hit on that actor: within 1.2 m of its position across, from 0.3 m below it to 2.3 m above (y up: a
    // person's or an animal's body). A stale handle, a lock elsewhere, a wall: white
    bool red = false;
    if (on_actor && rp.hit && now - g_last_override_ms.load(std::memory_order_relaxed) < 150.0) {
        const uintptr_t actor = player_actor();
        uintptr_t T = 0;
        int32_t handle = 0;
        if (actor && rd(actor + 0xb8, &T) && T && rd(T + 0x3900, &handle) && handle != 0 && handle != -1) {
            const float dx = hit[0] - rp.tgt_pos[0], dy = hit[1] - rp.tgt_pos[1], dz = hit[2] - rp.tgt_pos[2];
            red = handle == rp.tgt && now - rp.tgt_ms < 300.0 && std::isfinite(dx + dy + dz) && dx * dx + dz * dz < 1.2f * 1.2f && dy > -0.3f &&
                  dy < 2.3f;
            (red ? g_rp_red : g_rp_unred).fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (on_actor) *on_actor = red;
    if (size_deg) *size_deg = g_reticle_deg.load(std::memory_order_relaxed);
    g_reticle_shown.fetch_add(1, std::memory_order_relaxed);
    return true;
}
bool block_executions() { return g_block_exec.load(std::memory_order_relaxed); }
void set_block_executions(bool on) {
    if (g_block_exec.exchange(on) != on) log::info("[aim] close-range executions on the trigger blocked: %d", on ? 1 : 0);
    config::set("Hands", "BlockExecutions", on ? "1" : "0");
}
bool shoot_past_arm_block() { return g_past_arm_block.load(std::memory_order_relaxed); }
void set_shoot_past_arm_block(bool on, bool save) {
    if (g_past_arm_block.exchange(on) != on) log::info("[aim] shots past the game's arm block (its shoulder-line probe): %d", on ? 1 : 0);
    if (save) config::set("Hands", "ShootPastArmBlock", on ? "1" : "0");
}

bool held_item_matrix(float m[16], bool* left, uintptr_t* Wout, uintptr_t* wmgr_out) {
    const uintptr_t actor = player_actor();
    uintptr_t wmgr = 0, item = 0;
    int32_t state = 0;
    uint8_t lf = 0;
    if (wmgr_out) *wmgr_out = 0;
    if (!actor || !rd(actor + 0x70, &wmgr) || !wmgr) return false;
    if (wmgr_out) *wmgr_out = wmgr;
    if (!rd(wmgr + 0x80, &item) || !item || !rd(item + 0x24, &state) || state != 3) return false;  // +0x24 == 3: in the hand
    const uintptr_t W = player_gun(actor);
    if (!W || !raw(W + 0x80, m, 64)) return false;
    rd(item + 0x98, &lf);
    *left = lf != 0;
    if (Wout) *Wout = W;
    return true;
}

bool weapon_ik_offsets(uintptr_t W, float ik[3], float ik_hold[3]) {
    uintptr_t info = 0;
    return W && rd(W + 0x28, &info) && info && raw(info + 0x2f0, ik, 12) && raw(info + 0x300, ik_hold, 12);
}

bool weapon_muzzle_offset(uintptr_t W, float mo[3]) {
    uintptr_t info = 0;
    if (!W || !rd(W + 0x28, &info) || !info || !raw(info + 0x310, mo, 12)) return false;
    const float len2 = mo[0] * mo[0] + mo[1] * mo[1] + mo[2] * mo[2];
    return len2 > 1e-6f && len2 < 1.0f;  // also catches NaN
}

bool aiming() {
    uintptr_t info = 0, R = 0;
    uint32_t fl = 0;
    return rd(anchors::addr(anchors::Id::PlayerInfo), &info) && info && rd(info + 0x410, &R) && R && rd(R + 0xc, &fl) && (fl & 8);
}

bool barrel_in_target(float b[3]) {
    const double ms = g_bt_ms.load(std::memory_order_acquire);
    if (!ms || log::now_ms() - ms > 500.0) return false;
    for (int k = 0; k < 3; ++k) b[k] = g_bt[k].load(std::memory_order_relaxed);
    const float l = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (l < 0.5f) return false;
    for (int k = 0; k < 3; ++k) b[k] /= l;
    return true;
}

bool last_ray(float muzzle[3], float dir[3]) {
    const double ms = g_last_override_ms.load(std::memory_order_relaxed);
    if (!ms || log::now_ms() - ms > 500.0) return false;
    for (int k = 0; k < 3; ++k) muzzle[k] = g_last_muzzle[k], dir[k] = g_last_dir[k];
    const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(l > 0.5f && l < 2.0f)) return false;
    for (int k = 0; k < 3; ++k) dir[k] /= l;
    return true;
}

bool perfect_accuracy() { return g_perfect.load(); }
void on_exit() {
    if (g_npc_spawns.load(std::memory_order_relaxed) != g_npc_logged.load(std::memory_order_relaxed)) log_npc_shots(" (at the exit)");
}
void set_perfect_accuracy(bool on, bool save) {
    if (g_perfect.exchange(on) != on) log::info("[aim] perfect accuracy: %s", on ? "on" : "off");
    if (save) config::set("Aim", "PerfectAccuracy", on ? "1" : "0");
}
bool shotgun_pattern() { return g_pattern.load(); }
void set_shotgun_pattern(bool on, bool save) {
    if (g_pattern.exchange(on) != on) log::info("[aim] shotgun pattern: %s", on ? "the game's cone" : "every pellet on one line");
    if (save) config::set("Aim", "ShotgunPattern", on ? "1" : "0");
}
bool spawn_hooked() { return g_spawn_hooked.load(); }

void set_barrel_aim(bool on) {
    if (g_barrel.exchange(on) != on) log::info("[aim] shots from the barrel: %s", on ? "on" : "off");
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w, v;
    in >> c;
    if (line.find(" frames") != std::string::npos) {
        char f[300];
        std::snprintf(f, sizeof(f), "barrel in target (%.3f %.3f %.3f) in drawn wrist (%.3f %.3f %.3f) | game gun in animated wrist (%.3f %.3f %.3f)",
                      g_dir_target[0], g_dir_target[1], g_dir_target[2], g_dir_wrist[0], g_dir_wrist[1], g_dir_wrist[2], g_gun_anim[0],
                      g_gun_anim[1], g_gun_anim[2]);
        return f;
    }
    // (2026-10-28, round 38 check 2) the second reticle's: aim reticle drawn2 [on|off] (its line on the drawn second gun, its
    // own probe: the session), aim reticle dist2 (its probe's hold), aim barrelstep2 [on|off|reset], aim shot2 [on|off] (the
    // second gun's and the copy's shots on the drawn map: the session). Before the plain forms (" reticle dist" is in
    // " reticle dist2")
    if (line.find(" reticle drawn2") != std::string::npos) {
        if (line.find(" drawn2 on") != std::string::npos) g_ret2_drawn_on = true;
        if (line.find(" drawn2 off") != std::string::npos) g_ret2_drawn_on = false;
        ReticleProbe rp2;
        {
            std::lock_guard lock(g_rp_mutex);
            rp2 = g_rp2;
        }
        uint64_t pubs = 0, tw = 0, us0 = 0, n0 = 0, us1 = 0, n1 = 0;
        body::second_drawn_counts(&pubs, &tw);
        api::native_time(0, &us0, &n0);
        api::native_time(1, &us1, &n1);
        char e[900];
        std::snprintf(e, sizeof(e),
                      "reticle drawn2 %d: the second reticle on the drawn second gun %llu frames, on the old line %llu | the second probe: probes %llu "
                      "hits %llu lost %llu, no draw %llu, another gun %llu, now (%.3f %.3f %.3f) along (%.4f %.4f %.4f) hit %d at %.2f m, %.0f ms old, "
                      "the copy %d, hand %d | the draws published %llu (the copy's Tw %llu) | the probes' runs: the main's %.1f us (%llu), the "
                      "second's %.1f us (%llu) | the barrel in its hand's target (%.3f %.3f %.3f) | shot2 %d: on the drawn map %llu, not %llu",
                      g_ret2_drawn_on.load() ? 1 : 0, static_cast<unsigned long long>(g_ret2_drawn.load()),
                      static_cast<unsigned long long>(g_ret2_line.load()), static_cast<unsigned long long>(g_rp2_probes.load()),
                      static_cast<unsigned long long>(g_rp2_hits.load()), static_cast<unsigned long long>(g_rp2_lost.load()),
                      static_cast<unsigned long long>(g_rp2_nodrawn.load()), static_cast<unsigned long long>(g_rp2_otherW.load()), rp2.muzzle[0],
                      rp2.muzzle[1], rp2.muzzle[2], rp2.dir[0], rp2.dir[1], rp2.dir[2], rp2.hit ? 1 : 0, rp2.dist,
                      rp2.ms > 0.0 ? log::now_ms() - rp2.ms : -1.0, rp2.copy ? 1 : 0, rp2.john, static_cast<unsigned long long>(pubs),
                      static_cast<unsigned long long>(tw), n0 ? static_cast<double>(us0) / static_cast<double>(n0) : 0.0,
                      static_cast<unsigned long long>(n0), n1 ? static_cast<double>(us1) / static_cast<double>(n1) : 0.0,
                      static_cast<unsigned long long>(n1), g_bt2[0].load(), g_bt2[1].load(), g_bt2[2].load(), g_shot2_drawn.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_shot2_mapped.load()), static_cast<unsigned long long>(g_shot2_unmapped.load()));
        return e;
    }
    if (line.find(" reticle dist2") != std::string::npos) {  // aim reticle dist2: the second probe's hold, its last probes (ms:raw:held)
        std::lock_guard lock(g_rp_mutex);
        char e[200];
        std::snprintf(e, sizeof(e), "reticle dist2: %llu probes far off the held one, %llu changes of it, %llu probes traced | ms:raw:held",
                      static_cast<unsigned long long>(g_dist2_far.load()), static_cast<unsigned long long>(g_dist2_changes.load()),
                      static_cast<unsigned long long>(g_dist2_trace_n));
        std::string o = e;
        const uint64_t n = g_dist2_trace_n < kDistTraceN ? g_dist2_trace_n : kDistTraceN;
        for (uint64_t i = g_dist2_trace_n - n; i < g_dist2_trace_n; ++i) {
            const DistTrace& t = g_dist2_trace[i % kDistTraceN];
            std::snprintf(e, sizeof(e), " %.1f:%.2f:%.2f", t.ms, t.raw, t.held);
            o += e;
        }
        return o;
    }
    if (line.find(" barrelstep2") != std::string::npos) {  // aim barrelstep2 [on|off|reset]: round 38 check 2's measure (the session)
        if (line.find(" barrelstep2 on") != std::string::npos) g_bs2_on = true;
        if (line.find(" barrelstep2 off") != std::string::npos) g_bs2_on = false;
        body::set_drawn_second_capture(g_bs2_on.load());  // (the second gun's draw captured only while it is on)
        std::lock_guard lock(g_rp_mutex);
        if (line.find(" reset") != std::string::npos) g_bs2 = BarrelSteps2{};
        const BarrelSteps2& s = g_bs2;
        char e[1000];
        std::snprintf(e, sizeof(e),
                      "barrelstep2 %d: frames %llu, compared with the drawn gun %llu (skipped %llu) | the new way off the drawn gun at most %.3f deg "
                      "%.1f mm, its largest step %.3f deg (over 2 deg %llu) | the old line: frames %llu (compared %llu), off the drawn gun at most %.3f "
                      "deg, its largest step %.3f deg (over 2 deg %llu) | the hand's target turned at most %.3f deg a frame | the carry (the frame "
                      "before's gun by the hand's turn since): %llu frames, off the drawn gun at most %.3f deg %.1f mm (%llu muzzle frames; moved "
                      "only, not turned: %.1f mm), uncarried %.3f deg, the turn at most %.3f deg",
                      g_bs2_on.load() ? 1 : 0, static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.compared),
                      static_cast<unsigned long long>(s.skipped), s.new_off, s.new_mm, s.new_step, static_cast<unsigned long long>(s.new_over),
                      static_cast<unsigned long long>(s.old_frames), static_cast<unsigned long long>(s.old_compared), s.old_off, s.old_step,
                      static_cast<unsigned long long>(s.old_over), s.hand, static_cast<unsigned long long>(s.carry_n), s.carry_off, s.carry_mm,
                      static_cast<unsigned long long>(s.carry_mm_n), s.carry0_mm, s.carry0_off, s.carry_turn);
        return e;
    }
    if (line.find(" shot2") != std::string::npos) {  // aim shot2 [on|off]: the second gun's and the copy's shots on the drawn map (the session)
        if (line.find(" shot2 on") != std::string::npos) g_shot2_drawn = true;
        if (line.find(" shot2 off") != std::string::npos) g_shot2_drawn = false;
        char e[160];
        std::snprintf(e, sizeof(e), "shot2 %d: the second gun's and the copy's shots on the drawn map %llu, not %llu", g_shot2_drawn.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_shot2_mapped.load()), static_cast<unsigned long long>(g_shot2_unmapped.load()));
        return e;
    }
    if (line.find(" reticle stats") != std::string::npos || line.find(" reticle smooth") != std::string::npos ||
        line.find(" reticle line") != std::string::npos) {
        // aim reticle stats [reset] | aim reticle smooth on|off (the session): run 10 item 8's jitter and lag
        if (line.find(" smooth on") != std::string::npos) g_ret_smooth = true;
        if (line.find(" smooth off") != std::string::npos) g_ret_smooth = false;
        if (line.find(" line on") != std::string::npos) g_ret2_line_on = true;
        if (line.find(" line off") != std::string::npos) g_ret2_line_on = false;
        std::lock_guard lock(g_rp_mutex);
        char e[400];
        std::snprintf(e, sizeof(e), "reticle smooth %d, the second on its shot's line %d (%llu frames)", g_ret_smooth.load() ? 1 : 0,
                      g_ret2_line_on.load() ? 1 : 0, static_cast<unsigned long long>(g_ret2_line.load()));
        std::string o = e;
        for (int wi = 0; wi < 2; ++wi) {
            const RetStats& s = g_rs[wi];
            std::snprintf(e, sizeof(e), " | %s: frames %llu, jitter rms %.4f max %.4f deg, off the barrel mean %.4f max %.4f deg", wi ? "second" : "main",
                          static_cast<unsigned long long>(s.n), s.n > 1 ? std::sqrt(s.jit_sum2 / static_cast<double>(s.n - 1)) : 0.0, s.jit_max,
                          s.n ? s.off_sum / static_cast<double>(s.n) : 0.0, s.off_max);
            o += e;
        }
        if (g_rf[1].have_pos) {
            std::snprintf(e, sizeof(e), " | second at (%.3f %.3f %.3f)", g_rf[1].pos[0], g_rf[1].pos[1], g_rf[1].pos[2]);
            o += e;
        }
        if (line.find(" reset") != std::string::npos) g_rs[0] = g_rs[1] = RetStats{};
        return o;
    }
    if (line.find(" barrelstep") != std::string::npos) {  // aim barrelstep [on|off|reset]: item 1b's measure (the session)
        if (line.find(" barrelstep on") != std::string::npos) g_bs_on = true;
        if (line.find(" barrelstep off") != std::string::npos) g_bs_on = false;
        body::set_drawn_gun_capture(g_bs_on.load());  // (the drawn gun captured only while it is on)
        std::lock_guard lock(g_bs_mutex);
        if (line.find(" reset") != std::string::npos) {
            g_bs = BarrelSteps{};
            body::held_match_counts(&g_bs.newest0, &g_bs.older0);
        }
        const BarrelSteps& s = g_bs;
        uint64_t hn = 0, ho = 0;
        body::held_match_counts(&hn, &ho);
        char e[1100];
        std::snprintf(e, sizeof(e),
                      "barrelstep %d: ticks %llu (the drawn build's matrix %llu), steps %llu | the drawn build's barrel: largest step %.3f deg, over 2 deg "
                      "%llu | the old (live W +0x80): largest step %.3f deg, over 2 deg %llu | apart at most %.3f deg | the hand's target turned at most "
                      "%.3f deg a tick | barrel_ray built %llu live %llu, the aim ray built %llu live %llu | against the drawn gun: %llu ticks (skipped %llu, "
                      "older-build draws %llu), the new way off the corrected draw at most %.3f deg %.1f mm, off the seen draw %.3f deg %.1f mm | the "
                      "old way off the corrected draw %.3f deg %.1f mm, off the seen draw %.3f deg %.1f mm | held draws since the reset: newest %llu "
                      "older %llu",
                      g_bs_on.load() ? 1 : 0, static_cast<unsigned long long>(s.ticks), static_cast<unsigned long long>(s.built),
                      static_cast<unsigned long long>(s.steps), s.max_new, static_cast<unsigned long long>(s.new_over), s.max_old,
                      static_cast<unsigned long long>(s.old_over), s.max_apart, s.max_hand, static_cast<unsigned long long>(g_br_built.load()),
                      static_cast<unsigned long long>(g_br_live.load()), static_cast<unsigned long long>(g_ray_built.load()),
                      static_cast<unsigned long long>(g_ray_live.load()), static_cast<unsigned long long>(s.drawn),
                      static_cast<unsigned long long>(s.drawn_skip), static_cast<unsigned long long>(s.drawn_older), s.dw_deg[0][0], s.dw_mm[0][0],
                      s.dw_deg[0][1], s.dw_mm[0][1], s.dw_deg[1][0], s.dw_mm[1][0], s.dw_deg[1][1], s.dw_mm[1][1],
                      static_cast<unsigned long long>(hn - s.newest0), static_cast<unsigned long long>(ho - s.older0));
        return e;
    }
    if (line.find(" reticle dist") != std::string::npos) {  // aim reticle dist: item 1b's hold, the last probes (ms:raw:held)
        std::lock_guard lock(g_rp_mutex);
        char e[200];
        std::snprintf(e, sizeof(e), "reticle dist: %llu probes far off the held one, %llu changes of it, %llu probes traced | ms:raw:held",
                      static_cast<unsigned long long>(g_dist_far.load()), static_cast<unsigned long long>(g_dist_changes.load()),
                      static_cast<unsigned long long>(g_dist_trace_n));
        std::string o = e;
        const uint64_t n = g_dist_trace_n < kDistTraceN ? g_dist_trace_n : kDistTraceN;
        for (uint64_t i = g_dist_trace_n - n; i < g_dist_trace_n; ++i) {
            const DistTrace& t = g_dist_trace[i % kDistTraceN];
            std::snprintf(e, sizeof(e), " %.1f:%.2f:%.2f", t.ms, t.raw, t.held);
            o += e;
        }
        return o;
    }
    if (line.find(" reticle") != std::string::npos) {  // aim reticle [on|off|dot|ring]: where it is drawn now, its counters (the session)
        if (line.find(" reticle on") != std::string::npos) g_reticle = true;
        if (line.find(" reticle off") != std::string::npos) g_reticle = false;
        if (line.find(" reticle dot") != std::string::npos) g_reticle_dot = true;
        if (line.find(" reticle ring") != std::string::npos) g_reticle_dot = false;
        float p[3] = {0, 0, 0}, sz = 0;
        bool act = false;
        const bool ok = reticle_target(p, &act, &sz);
        char e[700];
        std::snprintf(e, sizeof(e), "reticle %d (%s), now %s (%.2f %.2f %.2f)%s, size %.1f deg | shown %llu, (the old barrel-line rejects %llu) | muzzle (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) | probes %llu hits %llu lost %llu, no gun %llu, no barrel %llu, red %llu, not red (the game's point elsewhere) %llu | distance: %llu probes far off the held one, %llu changes of it",
                      g_reticle.load() ? 1 : 0, g_reticle_dot.load() ? "dot" : "ring", ok ? "at" : "none", p[0], p[1], p[2], act ? " on an actor" : "", sz,
                      static_cast<unsigned long long>(g_reticle_shown.load()), static_cast<unsigned long long>(g_reticle_offline.load()),
                      g_last_muzzle[0], g_last_muzzle[1], g_last_muzzle[2], g_last_dir[0], g_last_dir[1], g_last_dir[2],
                      static_cast<unsigned long long>(g_rp_probes.load()), static_cast<unsigned long long>(g_rp_hits.load()),
                      static_cast<unsigned long long>(g_rp_lost.load()), static_cast<unsigned long long>(g_rp_nogun.load()),
                      static_cast<unsigned long long>(g_rp_nobarrel.load()), static_cast<unsigned long long>(g_rp_red.load()),
                      static_cast<unsigned long long>(g_rp_unred.load()), static_cast<unsigned long long>(g_dist_far.load()),
                      static_cast<unsigned long long>(g_dist_changes.load()));
        // the barrel's own probe now (the "muzzle ... dir" above is the last shot's): its ray, its hit's distance, its age
        // (run 28 item 1c's test places a stranger on it)
        ReticleProbe rp;
        {
            std::lock_guard lock(g_rp_mutex);
            rp = g_rp;
        }
        char r2[200];
        std::snprintf(r2, sizeof(r2), " | probe ray (%.3f %.3f %.3f) along (%.4f %.4f %.4f) hit %d at %.2f m, %.0f ms old", rp.muzzle[0], rp.muzzle[1],
                      rp.muzzle[2], rp.dir[0], rp.dir[1], rp.dir[2], rp.hit ? 1 : 0, rp.dist, rp.ms > 0.0 ? log::now_ms() - rp.ms : -1.0);
        return std::string(e) + r2;
    }
    if (line.find(" ledge") != std::string::npos) {  // aim ledge [trace on|off|reset]: the climb state (run 7 item 1f)
        if (line.find(" trace on") != std::string::npos) g_ledge_trace = true;
        if (line.find(" trace off") != std::string::npos) g_ledge_trace = false;
        if (line.find(" reset") != std::string::npos) {
            g_ledge_frames = g_vault_incoming = g_vault_requested = g_ledge_states = 0;
            g_ledge_rise = g_ledge_step = 0.0f;
        }
        const LedgeRead r = read_ledge();
        char e[600];
        std::snprintf(e, sizeof(e),
                      "ledge: read %d state %d vault bits 0x%02x 0x%02x 0x%02x edge near %d dist %.2f normal (%.2f %.2f %.2f) point (%.2f %.2f %.2f) | "
                      "move %d speed %.2f jumping %u aim %u root (%.3f %.3f %.3f) | X presses %llu | trace %d: frames %llu, incoming %llu, requested "
                      "%llu, ledge states %llu, the last window's rise %.3f m, largest step %.3f m",
                      r.ok ? 1 : 0, r.state, r.b1d0, r.b1d1, r.b1d2, r.edge_near ? 1 : 0, r.dist, r.normal[0], r.normal[1], r.normal[2], r.point[0],
                      r.point[1], r.point[2], r.move, r.speed, r.jumping, r.aim, r.root[0], r.root[1], r.root[2],
                      static_cast<unsigned long long>(xinput::x_presses()), g_ledge_trace.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_ledge_frames.load()), static_cast<unsigned long long>(g_vault_incoming.load()),
                      static_cast<unsigned long long>(g_vault_requested.load()), static_cast<unsigned long long>(g_ledge_states.load()),
                      g_ledge_rise.load(), g_ledge_step.load());
        return e;
    }
    if (line.find(" armblock") != std::string::npos) {  // aim armblock [on|off]: [Hands] ShootPastArmBlock for the session
        if (line.find(" armblock on") != std::string::npos) g_past_arm_block = true;
        if (line.find(" armblock off") != std::string::npos) g_past_arm_block = false;
        return std::string("shoot past the arm block ") + (g_past_arm_block.load() ? "on" : "off") + ", cleared " + std::to_string(g_arm_cleared.load());
    }
    if (line.find(" cover") != std::string::npos) {  // aim cover [log on|off]: the last shot request's arm block, cover and depth
        if (line.find(" log on") != std::string::npos) g_req_log = true;
        if (line.find(" log off") != std::string::npos) g_req_log = false;
        ReqDiag d;
        {
            std::lock_guard lock(g_req_mutex);
            d = g_req;
        }
        char e[700];
        std::snprintf(e, sizeof(e),
                      "cover: requests %llu, dropped by the arm block %llu, by the 1 m cover rule %llu | muzzle-block flips %llu (left in, PerfectAccuracy "
                      "off: %llu) | RB presses %llu, LB %llu | log %d | last %llu (%.0f ms ago): %s, arm %.2f/%.2f reason %u reach %.2f 450=0x%x, cover %d "
                      "bc 0x%x, depth %.2f, raise %.2f, gun state %d, no-shoot %u, target (%.2f %.2f %.2f), root (%.2f %.2f %.2f), start %.2f",
                      static_cast<unsigned long long>(g_reqs.load()), static_cast<unsigned long long>(g_arm_drops.load()),
                      static_cast<unsigned long long>(g_cover_drops.load()), static_cast<unsigned long long>(g_flips_seen.load()),
                      static_cast<unsigned long long>(g_flips_left.load()), static_cast<unsigned long long>(xinput::rb_presses()),
                      static_cast<unsigned long long>(xinput::lb_presses()), g_req_log.load() ? 1 : 0, static_cast<unsigned long long>(d.seq),
                      d.ms > 0 ? log::now_ms() - d.ms : -1.0, d.arm_drop ? "dropped (arm)" : d.cover_drop ? "dropped (cover)" : "went on", d.g400,
                      d.g404, d.g410, d.g40c, d.g450, d.cover, d.cbc, d.depth, d.g3c4, d.g3bc, d.t6088, d.target[0], d.target[1], d.target[2],
                      d.root[0], d.root[1], d.root[2], d.t3840);
        return e;
    }
    if (line.find(" executions") != std::string::npos) {  // aim executions: the gate's calls from the shot request
        char e[160];
        std::snprintf(e, sizeof(e), "executions blocked %d | the shot request asked %llu, answered false %llu", g_block_exec.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_exec_seen.load()), static_cast<unsigned long long>(g_exec_blocked.load()));
        return e;
    }
    if (line.find(" spawns") != std::string::npos) {
        char sp[420];
        std::snprintf(sp, sizeof(sp),
                      "spawn hook %d, perfect accuracy %d, shotgun pattern %d | spawns %llu, the player's %llu: bloom zeroed %llu, muzzle "
                      "fixes %llu, aligned %llu, speed left out %llu, pellets straightened %llu | others' %llu, with the game's bloom %llu (last %.3f)",
                      g_spawn_hooked.load() ? 1 : 0, g_perfect.load() ? 1 : 0, g_pattern.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_spawns.load()), static_cast<unsigned long long>(g_player_spawns.load()),
                      static_cast<unsigned long long>(g_bloom_zeroed.load()), static_cast<unsigned long long>(g_block_fixes.load()),
                      static_cast<unsigned long long>(g_aligned.load()), static_cast<unsigned long long>(g_speed_drops.load()),
                      static_cast<unsigned long long>(g_straightened.load()), static_cast<unsigned long long>(g_npc_spawns.load()),
                      static_cast<unsigned long long>(g_npc_bloomed.load()), static_cast<double>(g_npc_sigma.load()));
        return sp;
    }
    while (in >> w >> v) {
        if (w == "barrel") set_barrel_aim(v == "on");
        if (w == "fire") g_fire = v == "on";
        if (w == "lassoray") g_lasso_ray = v == "off" ? 0 : v == "game" ? 1 : 2;
        if (w == "accuracy") set_perfect_accuracy(v == "on", false);  // the session only
        if (w == "pattern") set_shotgun_pattern(v == "on", false);    // the session only

        if (w == "assist") g_assist = v == "on";
        if (w == "execute") g_block_exec = v != "on";  // aim execute on|off: the game's executions allowed (the session only)
        if (w == "paint") {
            // research: a Dead Eye paint request (T +0x6084 now, +0x6085 0 = the exact probe hit, +0x6086 1 = no rate
            // limit, +0x6087 1 = another mark on the same bone allowed), consumed by the game's probe
            const uintptr_t actor = player_actor();
            uintptr_t T = 0;
            if (actor && rd(actor + 0xb8, &T) && T) wr<uint32_t>(T + 0x6084, 0x01010001u);
        }
    }
    uintptr_t info = 0, R = 0;
    uint32_t hide_bits = 0;
    float alpha = -1;
    int32_t de_mode = -1, de_phase = -1;
    uint32_t marks = 0;
    if (rd(anchors::addr(anchors::Id::PlayerInfo), &info) && info && rd(info + 0x410, &R) && R) {
        rd(R + 0xc, &hide_bits);
        rd(R + 0x10, &alpha);
        rd(R + 0x1c, &de_mode);
        rd(R + 0x20, &de_phase);
    }
    {
        const uintptr_t actor = player_actor();
        uintptr_t T = 0;
        if (actor && rd(actor + 0xb8, &T) && T) rd(T + 0x6060, &marks);
    }
    char b[600];
    std::snprintf(b, sizeof(b), "dead eye mode %d phase %d marks %u (ray skips %llu) | reticle hide bits %#x alpha %.2f | ", de_mode, de_phase,
                  marks, static_cast<unsigned long long>(g_de_skips.load()), hide_bits, alpha);
    std::snprintf(b + std::strlen(b), sizeof(b) - std::strlen(b),
                  "barrel %d fire %d assist %d (cleared %llu) | rays %llu overrides %llu hand %llu (lasso at the lock %llu at (%.2f %.2f %.2f)) no-hand %llu no-gun %llu | fire calls %llu swaps %llu | "
                  "muzzle (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) offset (%.3f %.3f %.3f) | shots %llu tracers %llu last %.2f deg off",
                  g_barrel.load() ? 1 : 0, g_fire.load() ? 1 : 0, g_assist.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_assist_off.load()), static_cast<unsigned long long>(g_rays.load()),
                  static_cast<unsigned long long>(g_overrides.load()), static_cast<unsigned long long>(g_hand_rays.load()),
                  static_cast<unsigned long long>(g_lasso_at_lock.load()), g_lock_chest[0].load(), g_lock_chest[1].load(), g_lock_chest[2].load(),
                  static_cast<unsigned long long>(g_no_hand.load()),
                  static_cast<unsigned long long>(g_no_gun.load()), static_cast<unsigned long long>(g_fires.load()),
                  static_cast<unsigned long long>(g_swaps.load()), g_last_muzzle[0], g_last_muzzle[1], g_last_muzzle[2], g_last_dir[0],
                  g_last_dir[1], g_last_dir[2], g_last_offset[0], g_last_offset[1], g_last_offset[2],
                  static_cast<unsigned long long>(g_shots.load()), static_cast<unsigned long long>(g_tracers.load()), g_shot_err);
    return b;
}

}  // namespace rdrvr::aim
