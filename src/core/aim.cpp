#include "core/aim.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "core/anchors.h"
#include "core/api.h"
#include "core/body.h"
#include "core/gestures.h"
#include "core/config.h"
#include "core/dual.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/pose.h"
#include "core/reload.h"

namespace rdrvr::aim {
namespace {

std::atomic<bool> g_barrel{true};  // [Hands] BarrelAim
std::atomic<bool> g_fire{true};    // [Hands] FireInFirstPerson
std::atomic<bool> g_assist{false}; // [Hands] AimAssist (soft lock and the reticle magnet in first person)
std::atomic<bool> g_tracer{true};  // [Hands] TracerFromMuzzle
std::atomic<bool> g_perfect{false};  // [Aim] PerfectAccuracy
std::atomic<bool> g_pattern{true};   // [Aim] ShotgunPattern
std::atomic<bool> g_spawn_hooked{false};
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
// the last override, for the status line (game thread writes, the test channel reads: torn reads are harmless)
float g_last_muzzle[3] = {}, g_last_dir[3] = {}, g_last_offset[3] = {};
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

// The player's actor (the object's +0xb0 handle in the actor pool), as body.cpp finds it.
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
        if (api::actor_state(&st) && (st.weapon == 21 || gestures::is_thrown(st.weapon))) {
            if (!body::body_points(&bp) || !bp.hand_ok[bp.gun]) return r;
            const int gj = bp.gun;
            float bt[3] = {0.0f, 0.174f, -0.985f}, d[3], up[3] = {0, 1, 0}, rt[3], u2[3];
            barrel_in_target(bt);
            for (int k = 0; k < 3; ++k) d[k] = bp.target_rot[gj][k * 3] * bt[0] + bp.target_rot[gj][k * 3 + 1] * bt[1] + bp.target_rot[gj][k * 3 + 2] * bt[2];
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
                out[12 + k] = bp.hand[gj][k] + d[k] * 0.10f;
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
    if (!W || !raw(W + 0x80, gm, sizeof(gm))) {
        g_no_gun.fetch_add(1, std::memory_order_relaxed);
        return r;
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
        for (int k = 0; k < 3; ++k) g_bt[k].store(g_dir_target[k], std::memory_order_relaxed);
        g_bt_ms.store(log::now_ms(), std::memory_order_release);
    }
    g_overrides.fetch_add(1, std::memory_order_relaxed);
    g_last_override_ms.store(log::now_ms(), std::memory_order_relaxed);
    return r;
}

// FUN_1403237a0: the projectile launch, the one way player gun projectiles start (research\gc-aim.md 8-9):
// (projectile, matrix rows a b c d, weapon type, velocity, owner handle, launch position (the tracer's start), flags,
// ...). For the player's shots: logged against the barrel ray, and the tracer started at the drawn muzzle.
using Launch_t = void (*)(uintptr_t p, const float* mtx, int wtype, const float* vel, uint32_t owner, const float* launch, uint32_t flags,
                          char c8, float f9, const float* voff);
Launch_t o_launch = nullptr;

std::atomic<double> g_throw_start_ms{0};  // the player's last throw start (QuickThrow), for the launch's log
// [Hands] DualWield: the spawn's W (hk_spawn sets it around the game's call), and the second gun's pellets as seen
thread_local uintptr_t t_spawn_W = 0;
thread_local int t_sec_pellets = 0;
thread_local int t_copy_pellets = 0;  // [Hands] DualWieldCopy: the copy's shot's pellets (one log line)
thread_local double t_sec_first = -1.0, t_sec_max = -1.0;
thread_local float t_sec_start[3] = {}, t_sec_dir[3] = {}, t_sec_speed = 0.0f;
std::atomic<uint64_t> g_sec_shots{0};
std::atomic<float> g_sec_err{-1.0f};
void hk_launch(uintptr_t p, const float* mtx, int wtype, const float* vel, uint32_t owner, const float* launch, uint32_t flags, char c8,
               float f9, const float* voff) {
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
            return;
        }
        log::info("[aim] thrown weapon %d: the game's throw (%.1f m/s)", wtype, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
        o_launch(p, mtx, wtype, vel, owner, launch, flags, c8, f9, voff);
        return;
    }
    if (t_in_spawn && dual::copy_armed(t_spawn_W)) {  // [Hands] DualWieldCopy: the copy's shot, moved by T onto the copy
        alignas(16) float T[16], m[16];
        if (mtx && dual::copy_T(T) && raw(reinterpret_cast<uintptr_t>(mtx), m, sizeof(m))) {
            alignas(16) float mm[16], nv[4] = {0, 0, 0, v[3]}, at[4] = {0, 0, 0, 0};
            for (int row = 0; row < 4; ++row)  // p' = p T: the rows, the translation row also T's
                for (int col = 0; col < 4; ++col)
                    mm[row * 4 + col] = m[row * 4] * T[col] + m[row * 4 + 1] * T[4 + col] + m[row * 4 + 2] * T[8 + col] +
                                        (row == 3 ? T[12 + col] : m[row * 4 + 3] * T[12 + col]);
            for (int col = 0; col < 3; ++col) nv[col] = v[0] * T[col] + v[1] * T[4 + col] + v[2] * T[8 + col];
            if (launch && raw(reinterpret_cast<uintptr_t>(launch), at, sizeof(at))) {
                const float l0 = at[0], l1 = at[1], l2 = at[2];
                for (int col = 0; col < 3; ++col) at[col] = l0 * T[col] + l1 * T[4 + col] + l2 * T[8 + col] + T[12 + col];
            }
            const float sl = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
            if (t_copy_pellets++ == 0)
                log::info("[wield] the copy's shot: start (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) speed %.0f (moved from (%.2f %.2f %.2f))", mm[12],
                          mm[13], mm[14], sl > 1e-3f ? nv[0] / sl : 0.0f, sl > 1e-3f ? nv[1] / sl : 0.0f, sl > 1e-3f ? nv[2] / sl : 0.0f, sl, m[12],
                          m[13], m[14]);
            o_launch(p, mm, wtype, nv, owner, launch ? at : launch, flags, c8, f9, voff);
            return;
        }
    }
    if (t_in_spawn && dual::is_secondary_W(t_spawn_W)) {  // the second gun's: along its own barrel (-W +0xa0), its own tracer
        const float sl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float sd[3] = {0, 0, 0}, bz[3] = {0, 0, 0}, st[3] = {0, 0, 0};
        if (sl > 1e-3f)
            for (int k = 0; k < 3; ++k) sd[k] = v[k] / sl;
        if (mtx) raw(reinterpret_cast<uintptr_t>(mtx) + 48, st, sizeof(st));
        double err = -1.0;
        if (raw(t_spawn_W + 0xa0, bz, sizeof(bz))) {
            const double bl = std::sqrt(static_cast<double>(bz[0]) * bz[0] + static_cast<double>(bz[1]) * bz[1] + static_cast<double>(bz[2]) * bz[2]);
            if (bl > 1e-6) {
                const double b0 = -bz[0] / bl, b1 = -bz[1] / bl, b2 = -bz[2] / bl;
                const double cx = sd[1] * b2 - sd[2] * b1, cy = sd[2] * b0 - sd[0] * b2, cz = sd[0] * b1 - sd[1] * b0;
                err = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), sd[0] * b0 + sd[1] * b1 + sd[2] * b2) * 57.29577951308232;
            }
        }
        if (t_sec_pellets++ == 0) {
            t_sec_first = err;
            std::memcpy(t_sec_start, st, sizeof(st));
            std::memcpy(t_sec_dir, sd, sizeof(sd));
            t_sec_speed = sl;
        }
        if (err > t_sec_max) t_sec_max = err;
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
        o_spawn(W, proj, count, pellets, speed, u6, no_bloom, seed, turret);
        return;
    }
    g_player_spawns.fetch_add(1, std::memory_order_relaxed);
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
    o_spawn(W, proj, count, use, speed, u6, no_bloom, seed, turret);
    t_in_spawn = false;
    t_spawn_W = 0;
    if (t_sec_pellets > 0) {  // [Hands] DualWield: the second gun's shot, one line
        g_sec_shots.fetch_add(1, std::memory_order_relaxed);
        g_sec_err.store(static_cast<float>(t_sec_max), std::memory_order_relaxed);
        char extra[64] = "";
        if (t_sec_pellets > 1) std::snprintf(extra, sizeof(extra), ", %d pellets, the widest %.2f deg", t_sec_pellets, t_sec_max);
        log::info("[wield] second gun shot: start (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) speed %.0f; its barrel (%.2f deg off)%s | weapon %d, the "
                  "game's bloom %.3f deg, perfect %d",
                  t_sec_start[0], t_sec_start[1], t_sec_start[2], t_sec_dir[0], t_sec_dir[1], t_sec_dir[2], t_sec_speed, t_sec_first, extra, wtype,
                  sigma * 57.29578f, pa ? 1 : 0);
    }
    if (vel_zeroed)
        for (int k = 0; k < 3; ++k) wr<float>(W + 0x110 + 4 * k, vel_saved[k]);  // the owner's velocity back
    float off[2] = {};
    raw(W + 0x2b4, off, sizeof(off));  // this shot's bloom offsets (still there; FUN_1403019b0 clears them later)
    // one line per shot ("player shot: ... (X deg off)" as before, the first pellet's), the widest pellet for a shotgun
    if (t_pellets > 0) {
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
Fire_t o_fire = nullptr;

void hk_fire(uintptr_t info, uintptr_t wmgr) {
    g_fires.fetch_add(1, std::memory_order_relaxed);
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
    g_perfect = config::get_bool("Aim", "PerfectAccuracy", false);
    g_pattern = config::get_bool("Aim", "ShotgunPattern", true);
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

bool hk_exec_gate(uintptr_t phys) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::ExecGateRet)) {
        g_exec_seen.fetch_add(1, std::memory_order_relaxed);
        if (g_block_exec.load(std::memory_order_relaxed)) {
            g_exec_blocked.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return o_exec_gate(phys);
}

bool is_player_ped(uintptr_t ped) {
    uintptr_t actor = 0;
    uint8_t fl = 0;
    return ped && rd(ped + 0x10, &actor) && actor && rd(actor + 0x118, &fl) && (fl & 3) == 3;
}

uint8_t hk_throw_start(uintptr_t g, void* clip, float release, float rate, float phase_in, float phase_out, float draw_model, uint32_t focus) {
    uintptr_t ped = 0;
    if (g_quick_throw.load(std::memory_order_relaxed) && pose::anchor_active() && rd(g + 8, &ped) && is_player_ped(ped)) {
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

bool install() {
    g_quick_throw = config::get_bool("Gestures", "QuickThrow", true);
    g_throw_release = config::get_float("Gestures", "ThrowRelease", 0.15f);
    g_throw_rate = config::get_float("Gestures", "ThrowRate", 1.5f);
    g_melee_strike = config::get_float("Gestures", "MeleeStrike", 0.35f);
    g_block_exec = config::get_bool("Hands", "BlockExecutions", true);
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
    ok &= hooks::install("RDR execution gate (aim)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ExecGate)), hk_exec_gate, &o_exec_gate);
    log::info("[aim] close-range executions on the trigger blocked: %d", g_block_exec.load() ? 1 : 0);
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
bool block_executions() { return g_block_exec.load(std::memory_order_relaxed); }
void set_block_executions(bool on) {
    if (g_block_exec.exchange(on) != on) log::info("[aim] close-range executions on the trigger blocked: %d", on ? 1 : 0);
    config::set("Hands", "BlockExecutions", on ? "1" : "0");
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

bool perfect_accuracy() { return g_perfect.load(); }
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
    if (line.find(" executions") != std::string::npos) {  // aim executions: the gate's calls from the shot request
        char e[160];
        std::snprintf(e, sizeof(e), "executions blocked %d | the shot request asked %llu, answered false %llu", g_block_exec.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_exec_seen.load()), static_cast<unsigned long long>(g_exec_blocked.load()));
        return e;
    }
    if (line.find(" spawns") != std::string::npos) {
        char sp[320];
        std::snprintf(sp, sizeof(sp),
                      "spawn hook %d, perfect accuracy %d, shotgun pattern %d | spawns %llu, the player's %llu: bloom zeroed %llu, muzzle "
                      "fixes %llu, aligned %llu, speed left out %llu, pellets straightened %llu",
                      g_spawn_hooked.load() ? 1 : 0, g_perfect.load() ? 1 : 0, g_pattern.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_spawns.load()), static_cast<unsigned long long>(g_player_spawns.load()),
                      static_cast<unsigned long long>(g_bloom_zeroed.load()), static_cast<unsigned long long>(g_block_fixes.load()),
                      static_cast<unsigned long long>(g_aligned.load()), static_cast<unsigned long long>(g_speed_drops.load()),
                      static_cast<unsigned long long>(g_straightened.load()));
        return sp;
    }
    while (in >> w >> v) {
        if (w == "barrel") set_barrel_aim(v == "on");
        if (w == "fire") g_fire = v == "on";
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
                  "barrel %d fire %d assist %d (cleared %llu) | rays %llu overrides %llu hand %llu no-hand %llu no-gun %llu | fire calls %llu swaps %llu | "
                  "muzzle (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) offset (%.3f %.3f %.3f) | shots %llu tracers %llu last %.2f deg off",
                  g_barrel.load() ? 1 : 0, g_fire.load() ? 1 : 0, g_assist.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_assist_off.load()), static_cast<unsigned long long>(g_rays.load()),
                  static_cast<unsigned long long>(g_overrides.load()), static_cast<unsigned long long>(g_hand_rays.load()),
                  static_cast<unsigned long long>(g_no_hand.load()),
                  static_cast<unsigned long long>(g_no_gun.load()), static_cast<unsigned long long>(g_fires.load()),
                  static_cast<unsigned long long>(g_swaps.load()), g_last_muzzle[0], g_last_muzzle[1], g_last_muzzle[2], g_last_dir[0],
                  g_last_dir[1], g_last_dir[2], g_last_offset[0], g_last_offset[1], g_last_offset[2],
                  static_cast<unsigned long long>(g_shots.load()), static_cast<unsigned long long>(g_tracers.load()), g_shot_err);
    return b;
}

}  // namespace rdrvr::aim
