#include "core/dual.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/anchors.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/d3d_hooks.h"
#include "core/hands.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/pose.h"
#include "core/reload.h"
#include "core/whistle.h"

namespace rdrvr::dual {
namespace {

// Game memory read and written with SEH as the backstop (no system calls: the weapon tick runs every frame).
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

// FUN_1403b4660(item), the gun item's trigger (its vtable slot 6): 1 fired, 0 refused; ~0 a fault (SEH)
uint64_t call_trigger(uintptr_t fn, uintptr_t item) {
    __try {
        return reinterpret_cast<uint64_t (*)(uintptr_t)>(fn)(item);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ~0ull;
    }
}

std::atomic<bool> g_enabled{false};  // [Hands] DualWield
// the wanted second gun: the frame end writes (begin, end), the game thread reads; the generation is bumped last
std::atomic<int> g_want_slot{-1}, g_want_ctrl{-1}, g_want_john{-1};
std::atomic<uint32_t> g_want_gen{0};
char g_zone[24] = "";  // the frame end's (the log)
// the game thread's view, once a frame at the weapon tick
std::atomic<uintptr_t> g_W{0}, g_item{0};
std::atomic<int> g_john{-1}, g_weapon{-1};
std::atomic<float> g_clip{-1.0f};
std::atomic<uint32_t> g_valid_gen{0};  // the generation the game thread last looked at
std::atomic<int> g_invalid{0};         // why it could not use it then (0: found)
std::atomic<uint32_t> g_fire_req{0}, g_fire_done{0};
std::atomic<uint64_t> g_ticks{0}, g_shots{0}, g_refused{0}, g_faults{0}, g_begins{0}, g_ends{0}, g_clicks{0}, g_pulls{0};
std::atomic<int> g_last_ret{-1};
std::atomic<bool> g_shown{false};  // W +0x120 bit 6: its prop shown (a long gun not on display on the back is not drawn)
// the game thread's updates (the weapon tick counts them) and the one the second gun was last placed in the hand in
std::atomic<uint64_t> g_tick_n{0}, g_placed_n{~0ull};
std::atomic<uintptr_t> g_placed_W{0};
uint32_t g_tick_gen = 0;  // the game thread's: the generation of its last tick (a new one drops older pulls)
std::atomic<uint64_t> g_unplaced{0};  // pulls dropped because this update did not place it in the hand
// begin, end and the frame end's own state changes (the frame end and the test channel; review 2)
std::recursive_mutex g_mx;
std::atomic<int> g_forced{-1};     // the slot whose render the frame end forced on (undone at the end), -1 none
// [Hands] DualWieldCopy (run 4 item 3): the same sidearm in the free hand, a copy of the gun in hand
std::atomic<bool> g_copy_en{false};
std::atomic<bool> g_same_at_its{true};  // [Hands] DualWieldSameAtItsHolster
std::atomic<bool> g_own_model{false};  // [Hands] DualWieldOwnModel (run 5 item 2)
std::atomic<int> g_copy_model{-1};
std::atomic<bool> g_copy_as_prop{true};  // [Hands] CopyAsProp (run 7 item 1)
std::atomic<uintptr_t> g_copy_W{0}, g_copy_item{0};  // the game thread's: the gun in hand's W and item while a copy is out
std::atomic<float> g_copy_clip{0.0f}, g_copy_max{0.0f};  // the copy's own rounds (the frame end's count) and its capacity
std::atomic<int> g_copy_weapon{-1};
std::atomic<uint64_t> g_copy_placed_n{~0ull}, g_copy_shots{0}, g_copy_loads{0}, g_copy_flashes{0};
float g_copy_T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // the game thread's (note_copy_placed)
bool g_copy_T_ok = false;
uintptr_t g_copy_armed = 0;  // the game thread's: the W whose shot in this weapon tick is the copy's
float g_copy_restore = 0.0f;  // the gun in hand's clip before the copy's shot (given back after it)
uintptr_t g_copy_flash_src = 0;  // the muzzle flash's own matrix (the prop's muzzle bone), followed through T
uint64_t g_copy_flash_until = 0;
alignas(16) float g_copy_bone[16] = {};  // the copy's muzzle flash follows this (MuzzleFxMatrix's pointer)
// the sidearms' ammo types (Enums.h AmmoType): the pistols 0-3 AMMO_TYPE_PISTOL 6, the revolvers 4-7 AMMO_TYPE_REVOLVER 7
int sidearm_ammo(int w) { return w >= 0 && w <= 3 ? 6 : w >= 4 && w <= 7 ? 7 : -1; }
uint64_t fbits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
// ACTOR_SET_INV_AMMO(actor, ammo type, count, 0): the spare rounds of that type (a script tick)
void set_spare(int32_t actor, int type, float count) {
    if (!actor || type < 0) return;
    const uint64_t args[4] = {static_cast<uint64_t>(static_cast<uint32_t>(actor)), static_cast<uint64_t>(type), fbits(count < 0 ? 0.0f : count), 0ull};
    api::queue_native(0x4372593E, args, 4, 0, nullptr);
}
// row-vector 4x4 affine product out = a b (the game's matrices: rows the axes, then the translation)
void mul44(const float* a, const float* bm, float* out) {
    float t[16];
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            t[i * 4 + k] = a[i * 4] * bm[k] + a[i * 4 + 1] * bm[4 + k] + a[i * 4 + 2] * bm[8 + k] + a[i * 4 + 3] * bm[12 + k];
    std::memcpy(out, t, sizeof(t));
}

const char* why_text(int why) {
    switch (why) {
        case 1: return "no player actor";
        case 2: return "no weapon manager";
        case 3: return "its slot is empty";
        case 4: return "not a gun item";
        case 5: return "not holstered (the game drew or dropped it)";
        case 6: return "it is the item in hand";
        case 7: return "no weapon object";
        case 8: return "its gun model is not loaded";
        case 9: return "not a firearm";
        case 10: return "a melee tuning";
        case 11: return "no gun in hand";
        case 12: return "not a sidearm";
        default: return "?";
    }
}

// The player's actor (the object's +0xb0 handle in the actor pool), as body.cpp finds it.
uintptr_t player_actor() {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.object) return 0;
    const uint32_t h = static_cast<uint32_t>(st.object);
    uintptr_t pool = 0, obj = 0, apool = 0, actor = 0;
    uint16_t gen = 0, idx = 0, agen = 0, sgen = 0;
    if (!rd(anchors::addr(anchors::Id::ObjectsPool), &pool) || !pool) return 0;
    const uintptr_t slot = pool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16) || !rd(slot, &obj) || !obj) return 0;
    if (!rd(obj + 0xb0, &idx) || !rd(obj + 0xb2, &agen) || !rd(anchors::addr(anchors::Id::ActorPool), &apool) || !apool) return 0;
    if (!rd(apool + idx * 0x10u + 8, &sgen) || sgen != agen || !rd(apool + idx * 0x10u, &actor)) return 0;
    return actor;
}

// The slot's item as a second gun (research\run3\dualwield.md 6.2): a gun item (its vtable), holstered (+0x24 == 2),
// not the item in hand (wmgr +0x80), with a weapon object (+0xa0) whose prop exists (W +0x2c0 == 2: the placement
// hook is called only then), a firearm (W +0x28 -> +8) without a melee tuning (TD +0xec: its trigger hides the prop).
int resolve(int slot, uintptr_t* item_out, uintptr_t* W_out, int* weapon_out, float* clip_out) {
    const uintptr_t actor = player_actor();
    if (!actor) return 1;
    uintptr_t wmgr = 0, hand = 0, item = 0, vt = 0, W = 0, info = 0, td = 0;
    if (!rd(actor + 0x70, &wmgr) || !wmgr) return 2;
    rd(wmgr + 0x80, &hand);
    if (!rd(wmgr + 0xa8 + static_cast<uintptr_t>(slot) * 0x70, &item) || !item) return 3;
    if (!rd(item, &vt) || vt != anchors::addr(anchors::Id::GunItemVtbl)) return 4;
    int32_t state = 0;
    if (!rd(item + 0x24, &state) || state != 2) return 5;
    if (item == hand) return 6;
    if (!rd(item + 0xa0, &W) || !W) return 7;
    int32_t prop = 0;
    if (!rd(W + 0x2c0, &prop) || prop != 2) return 8;
    int16_t wt = -1;
    if (!rd(W + 0x28, &info) || !info || !rd(info + 8, &wt) || !reload::is_gun(wt)) return 9;
    int32_t var = 0;
    if (!rd(W + 0x30, &var) || var < 0 || var > 7 || !rd(info + 0x30 + static_cast<uintptr_t>(var) * 8, &td) || !td) rd(info + 0x30, &td);
    uint8_t melee = 1;
    if (!td || !rd(td + 0xec, &melee) || melee) return 10;
    float clip = -1.0f;
    rd(W + 0x12c, &clip);
    uint32_t wf = 0;
    rd(W + 0x120, &wf);
    g_shown.store((wf & 0x40) != 0, std::memory_order_relaxed);
    *item_out = item;
    *W_out = W;
    *weapon_out = wt;
    *clip_out = clip;
    return 0;
}

// The copy's source: the item in hand (wmgr +0x80, in the hand: +0x24 == 3), a gun item whose W has its prop, a
// sidearm (eWeapon 0-7) without a melee tuning
int resolve_copy(uintptr_t* item_out, uintptr_t* W_out, int* weapon_out, float* clip_out) {
    const uintptr_t actor = player_actor();
    if (!actor) return 1;
    uintptr_t wmgr = 0, item = 0, vt = 0, W = 0, info = 0, td = 0;
    if (!rd(actor + 0x70, &wmgr) || !wmgr) return 2;
    if (!rd(wmgr + 0x80, &item) || !item) return 11;
    if (!rd(item, &vt) || vt != anchors::addr(anchors::Id::GunItemVtbl)) return 4;
    int32_t state = 0;
    if (!rd(item + 0x24, &state) || state != 3) return 11;
    if (!rd(item + 0xa0, &W) || !W) return 7;
    int32_t prop = 0;
    if (!rd(W + 0x2c0, &prop) || prop != 2) return 8;
    int16_t wt = -1;
    if (!rd(W + 0x28, &info) || !info || !rd(info + 8, &wt) || sidearm_ammo(wt) < 0) return 12;
    int32_t var = 0;
    if (!rd(W + 0x30, &var) || var < 0 || var > 7 || !rd(info + 0x30 + static_cast<uintptr_t>(var) * 8, &td) || !td) rd(info + 0x30, &td);
    uint8_t melee = 1;
    if (!td || !rd(td + 0xec, &melee) || melee) return 10;
    float clip = -1.0f;
    rd(W + 0x12c, &clip);
    *item_out = item;
    *W_out = W;
    *weapon_out = wt;
    *clip_out = clip;
    return 0;
}

// The copy's shot: the gun in hand's own trigger, W +0x9b8 set (its shot along its barrel, no target), a round lent
// to its clip if it is empty (the game refuses an empty gun and mutes its gunshot), its clip given back after the weapon
// tick (the copy's round is the mod's count). The shot's launch and its muzzle flash are moved onto the copy by T.
void fire_copy(uintptr_t item, uintptr_t W, int weapon) {
    const uintptr_t base = anchors::base();
    uintptr_t vt = 0, fn = 0;
    float c0 = 0.0f;
    if (!rd(item, &vt) || vt != anchors::addr(anchors::Id::GunItemVtbl) || !rd(vt + 0x30, &fn) || fn < base || fn >= base + 0x4000000 ||
        !rd(W + 0x12c, &c0)) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_copy_clip.load(std::memory_order_relaxed) < 0.5f) return;
    uint8_t b8 = 0;
    rd(W + 0x9b8, &b8);
    if (c0 < 1.0f && !wr<float>(W + 0x12c, 1.0f)) return;
    if (!wr<uint8_t>(W + 0x9b8, 1)) {
        wr<float>(W + 0x12c, c0);
        return;
    }
    const uint64_t r = call_trigger(fn, item);
    if (r == ~0ull) {
        wr<uint8_t>(W + 0x9b8, b8);
        wr<float>(W + 0x12c, c0);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        g_copy_en.store(false);
        log::error("[wield] the copy's trigger faulted (weapon %d): the copy off for this session", weapon);
        return;
    }
    g_last_ret.store(static_cast<int>(r & 0xff), std::memory_order_relaxed);
    if ((r & 0xff) == 0) {
        wr<uint8_t>(W + 0x9b8, b8);
        wr<float>(W + 0x12c, c0);
        g_refused.fetch_add(1, std::memory_order_relaxed);
        log::info("[wield] the copy's trigger: refused by the game (weapon %d; the fire interval or a reload)", weapon);
        return;
    }
    g_copy_armed = W;
    g_copy_restore = c0;
    g_copy_clip.fetch_sub(1.0f, std::memory_order_relaxed);  // the game thread's shot and the frame end's load: atomic (review)
    g_copy_shots.fetch_add(1, std::memory_order_relaxed);
    log::info("[wield] the copy fired (weapon %d; its clip %.0f left, the gun in hand's %.0f kept)", weapon,
              g_copy_clip.load(std::memory_order_relaxed), c0);
}

// A pull of the second gun's trigger, on the game thread before the weapons tick: W +0x9b8 set (the trigger then
// latches W +0x9b1: the shot leaves its muzzle along its own barrel, -W +0xa0, with no target), then the item's own
// trigger. The tick that follows fires it (the ammo cost, the projectiles, the muzzle flash, the gunshot).
void fire(uintptr_t item, uintptr_t W, int weapon, float clip) {
    const uintptr_t base = anchors::base();
    uintptr_t vt = 0, fn = 0;
    if (!rd(item, &vt) || vt != anchors::addr(anchors::Id::GunItemVtbl) || !rd(vt + 0x30, &fn) || fn < base || fn >= base + 0x4000000) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        log::warn("[wield] the second gun's trigger: no trigger function (item %p)", reinterpret_cast<void*>(item));
        return;
    }
    uint8_t b8 = 0;
    rd(W + 0x9b8, &b8);
    if (!wr<uint8_t>(W + 0x9b8, 1)) return;
    const uint64_t r = call_trigger(fn, item);
    if (r == ~0ull) {
        wr<uint8_t>(W + 0x9b8, b8);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        g_enabled.store(false);  // never again this session (a fault inside its listener lock; the frame end ends it)
        log::error("[wield] the second gun's trigger faulted (weapon %d): dual wield off for this session", weapon);
        return;
    }
    g_last_ret.store(static_cast<int>(r & 0xff), std::memory_order_relaxed);
    if ((r & 0xff) == 0) {
        wr<uint8_t>(W + 0x9b8, b8);  // not fired: as it was
        g_refused.fetch_add(1, std::memory_order_relaxed);
        log::info("[wield] the second gun's trigger: refused by the game (weapon %d, clip %.0f)", weapon, clip);
        return;
    }
    g_shots.fetch_add(1, std::memory_order_relaxed);
    log::info("[wield] the second gun fired (weapon %d, clip %.0f before)", weapon, clip);
}

void tick() {
    g_ticks.fetch_add(1, std::memory_order_relaxed);
    const uint64_t n = g_tick_n.load(std::memory_order_relaxed);
    g_tick_n.store(n + 1, std::memory_order_relaxed);  // the placements of the next update are numbered n + 1
    const uint32_t gen = g_want_gen.load(std::memory_order_acquire);
    if (gen != g_tick_gen) {  // a begin or an end since the last tick: the pulls made before it are not this gun's
        g_tick_gen = gen;
        g_fire_done.store(g_fire_req.load(std::memory_order_acquire), std::memory_order_relaxed);
    }
    const int slot = g_want_slot.load(std::memory_order_relaxed), john = g_want_john.load(std::memory_order_relaxed);
    auto drop = [&](int why) {
        g_fire_done.store(g_fire_req.load(std::memory_order_acquire), std::memory_order_relaxed);  // no pull outlives it
        g_W.store(0, std::memory_order_relaxed);
        g_copy_W.store(0, std::memory_order_relaxed);
        g_copy_item.store(0, std::memory_order_relaxed);
        g_item.store(0, std::memory_order_relaxed);
        g_john.store(-1, std::memory_order_relaxed);
        g_invalid.store(why, std::memory_order_relaxed);
        g_valid_gen.store(gen, std::memory_order_release);
    };
    if (slot == kCopySlot && john >= 0 && john <= 1 && g_copy_en.load(std::memory_order_relaxed)) {  // the copy
        uintptr_t item = 0, W = 0;
        int weapon = -1;
        float clip = -1.0f;
        if (const int why = resolve_copy(&item, &W, &weapon, &clip)) {
            drop(why);
            return;
        }
        g_W.store(0, std::memory_order_relaxed);
        g_copy_item.store(item, std::memory_order_relaxed);
        g_copy_W.store(W, std::memory_order_relaxed);
        g_john.store(john, std::memory_order_relaxed);
        g_copy_weapon.store(weapon, std::memory_order_relaxed);
        g_invalid.store(0, std::memory_order_relaxed);
        g_valid_gen.store(gen, std::memory_order_release);
        const uint32_t req = g_fire_req.load(std::memory_order_acquire);
        if (req != g_fire_done.load(std::memory_order_relaxed)) {
            g_fire_done.store(req, std::memory_order_relaxed);
            if (g_copy_placed_n.load(std::memory_order_relaxed) == n && g_copy_T_ok) {
                fire_copy(item, W, weapon);
            } else {
                g_unplaced.fetch_add(1, std::memory_order_relaxed);
                log::info("[wield] the copy's pull dropped: not placed in the hand this update");
            }
        }
        return;
    }
    if (!g_enabled.load(std::memory_order_relaxed) || slot < 0 || slot > 7 || john < 0 || john > 1) {
        drop(0);
        return;
    }
    uintptr_t item = 0, W = 0;
    int weapon = -1;
    float clip = -1.0f;
    if (const int why = resolve(slot, &item, &W, &weapon, &clip)) {
        drop(why);
        return;
    }
    g_item.store(item, std::memory_order_relaxed);
    g_W.store(W, std::memory_order_relaxed);
    g_john.store(john, std::memory_order_relaxed);
    g_weapon.store(weapon, std::memory_order_relaxed);
    g_clip.store(clip, std::memory_order_relaxed);
    g_invalid.store(0, std::memory_order_relaxed);
    g_valid_gen.store(gen, std::memory_order_release);
    const uint32_t req = g_fire_req.load(std::memory_order_acquire);
    if (req != g_fire_done.load(std::memory_order_relaxed)) {
        g_fire_done.store(req, std::memory_order_relaxed);
        // only when this update's item placement put it in the hand (its W +0x80 and +0x920 the hand's): else the shot
        // would leave from its holster
        if (g_placed_n.load(std::memory_order_relaxed) == n && g_placed_W.load(std::memory_order_relaxed) == W) {
            fire(item, W, weapon, clip);
        } else {
            g_unplaced.fetch_add(1, std::memory_order_relaxed);
            log::info("[wield] the second gun's pull dropped: not placed in the hand this update");
        }
    }
}

// FUN_14031a260(): every weapon object's tick (the update, after the items' placements and the player's aim tick)
using WeaponTick_t = void (*)();
WeaponTick_t o_weapon_tick = nullptr;
void hk_weapon_tick() {
    if (g_enabled.load(std::memory_order_relaxed) || g_W.load(std::memory_order_relaxed) || g_copy_en.load(std::memory_order_relaxed) ||
        g_copy_W.load(std::memory_order_relaxed))
        tick();
    o_weapon_tick();
    if (const uintptr_t W = g_copy_armed) {  // the copy's shot is done: the gun in hand's clip as it was (the copy paid)
        wr<float>(W + 0x12c, g_copy_restore);
        g_copy_armed = 0;
    }
    if (g_copy_flash_src && g_tick_n.load(std::memory_order_relaxed) <= g_copy_flash_until) {  // the flash follows the copy
        alignas(16) float src[16];
        if (raw(g_copy_flash_src, src, sizeof(src)) && g_copy_T_ok) mul44(src, g_copy_T, g_copy_bone);
    } else {
        g_copy_flash_src = 0;
    }
}

// FUN_140301680(W, &matrix ptr, &local): the muzzle flash's followed matrix. For the copy's shot it is ours, kept on
// the copy (the prop's muzzle bone through T, every weapon tick while the flash lives)
using MuzzleFx_t = void (*)(uintptr_t W, uintptr_t* mptr, void* local);
MuzzleFx_t o_muzzle_fx = nullptr;
void hk_muzzle_fx(uintptr_t W, uintptr_t* mptr, void* local) {
    o_muzzle_fx(W, mptr, local);
    if (!W || W != g_copy_armed || !mptr || !g_copy_T_ok) return;
    uintptr_t src = 0;
    alignas(16) float m[16];
    if (!raw(reinterpret_cast<uintptr_t>(mptr), &src, sizeof(src)) || !src || !raw(src, m, sizeof(m))) return;
    mul44(m, g_copy_T, g_copy_bone);
    g_copy_flash_src = src;
    g_copy_flashes.fetch_add(1, std::memory_order_relaxed);
    g_copy_flash_until = g_tick_n.load(std::memory_order_relaxed) + 40;
    wr<uintptr_t>(reinterpret_cast<uintptr_t>(mptr), reinterpret_cast<uintptr_t>(g_copy_bone));  // the caller's local
}

// The frame end: the second gun's trigger (its controller's, which controls.cpp keeps off the pad while it is out),
// one shot a pull; an empty gun clicks. Ended when the game thread could not find it, or first person ended.
void frame() {
    std::lock_guard lock(g_mx);
    static bool down = false;
    const int ctrl = g_want_ctrl.load(std::memory_order_relaxed);
    if (ctrl < 0) {
        down = false;
        return;
    }
    if (g_want_slot.load(std::memory_order_relaxed) == kCopySlot ? !g_copy_en.load(std::memory_order_relaxed)
                                                                  : !g_enabled.load(std::memory_order_relaxed)) {
        end(g_want_slot.load(std::memory_order_relaxed) == kCopySlot ? "the copy turned off" : "dual wield off");
        return;
    }
    const uint32_t gen = g_want_gen.load(std::memory_order_acquire);
    if (g_valid_gen.load(std::memory_order_acquire) == gen) {
        if (const int why = g_invalid.load(std::memory_order_relaxed)) {
            end(why_text(why));
            return;
        }
    }
    if (!pose::anchor_active()) {
        end("first person ended");
        return;
    }
    if (g_W.load(std::memory_order_relaxed) && !g_shown.load(std::memory_order_relaxed) && g_forced.load() < 0) {
        // its prop is not on display (the back shows one long gun): ACTOR_FORCE_WEAPON_RENDER(actor, slot, 1), 3 arguments
        // (FUN_1403bc2b0: FUN_140343a70 shows that slot's gun, the slot's force flag set); 0 at the end clears the flag
        RdrvrActorState st{};
        const int slot = g_want_slot.load(std::memory_order_relaxed);
        if (slot >= 0 && api::actor_state(&st) && st.actor) {
            const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(slot), 1ull};
            api::queue_native(0x1511D111, args, 3, 0, nullptr);
            g_forced.store(slot);
            log::info("[wield] the second gun's prop shown (ACTOR_FORCE_WEAPON_RENDER slot %d)", slot);
        }
    }
    const float t = whistle::suppressed(ctrl) ? 0.0f : hands::get(ctrl).trigger;  // run 8: the whistle's trigger fires nothing
    const bool was = down;
    down = t > 0.6f ? true : t < 0.4f ? false : was;
    const bool copy = g_want_slot.load(std::memory_order_relaxed) == kCopySlot;
    if (!down || was || menu::visible() || !(copy ? g_copy_W.load(std::memory_order_relaxed) : g_W.load(std::memory_order_relaxed))) return;
    RdrvrActorState st{};
    if (api::actor_state(&st) && (st.weapon_flags & RDRVR_WEAPON_DEADEYE)) return;  // Dead Eye fires the gun in hand
    if (copy) {  // the copy shares the gun in hand's weapon object: not while the gun hand's own trigger is down
        g_pulls.fetch_add(1, std::memory_order_relaxed);
        if (hands::get(1 - ctrl).trigger > 0.4f) {
            log::info("[wield] the copy's pull held: the gun hand's trigger is down");
            return;
        }
        if (g_copy_clip.load(std::memory_order_relaxed) < 0.5f) {
            if (reload::hand_reload()) audio::empty_click(ctrl, g_copy_weapon.load(std::memory_order_relaxed));
            g_clicks.fetch_add(1, std::memory_order_relaxed);
            log::info("[wield] the copy is empty: a click");
            return;
        }
        g_fire_req.fetch_add(1, std::memory_order_release);
        controllers::pulse(ctrl, 0.6f, 25);
        return;
    }
    g_pulls.fetch_add(1, std::memory_order_relaxed);
    const float clip = g_clip.load(std::memory_order_relaxed);
    if (clip >= 0.0f && clip < 0.5f) {
        if (reload::hand_reload()) audio::empty_click(ctrl, g_weapon.load(std::memory_order_relaxed));
        g_clicks.fetch_add(1, std::memory_order_relaxed);
        log::info("[wield] the second gun is empty: a click");
        return;
    }
    g_fire_req.fetch_add(1, std::memory_order_release);
    controllers::pulse(ctrl, 0.6f, 25);
}

}  // namespace

void init() {
    g_enabled = config::get_bool("Hands", "DualWield", false);
    log::info("[wield] dual wield %d: the free hand at another slot's holster takes that gun as a second", g_enabled.load() ? 1 : 0);
    g_copy_en = config::get_bool("Hands", "DualWieldCopy", false);
    g_own_model = config::get_bool("Hands", "DualWieldOwnModel", false);
    g_same_at_its = config::get_bool("Hands", "DualWieldSameAtItsHolster", true);
    g_copy_as_prop = config::get_bool("Hands", "CopyAsProp", true);
    log::info("[wield] the copy %d: the free hand at the other hip, a sidearm in hand, takes the same sidearm", g_copy_en.load() ? 1 : 0);
    d3d::add_frame_end_listener([](uint64_t) { frame(); });
}

bool install() {
    bool ok = hooks::install("RDR weapon tick (dual wield)", reinterpret_cast<void*>(anchors::addr(anchors::Id::WeaponTick)), hk_weapon_tick,
                             &o_weapon_tick);
    ok &= hooks::install("RDR muzzle flash matrix (the copy)", reinterpret_cast<void*>(anchors::addr(anchors::Id::MuzzleFxMatrix)), hk_muzzle_fx,
                         &o_muzzle_fx);
    return ok;
}
bool copy_enabled() { return g_copy_en.load(); }
bool own_model() { return g_own_model.load(std::memory_order_relaxed); }
bool same_at_its_holster() { return g_same_at_its.load(std::memory_order_relaxed); }
void set_same_at_its_holster(bool on) {
    if (g_same_at_its.exchange(on) != on) log::info("[wield] the gun's own holster gives a second of it: %s", on ? "on" : "off");
    config::set("Hands", "DualWieldSameAtItsHolster", on ? "1" : "0");
}
void set_own_model(bool on) {
    if (g_own_model.exchange(on) != on) log::info("[wield] the copy shows the other sidearm's model: %s", on ? "on" : "off");
    config::set("Hands", "DualWieldOwnModel", on ? "1" : "0");
}
void set_copy_model(int weapon) {
    g_copy_model.store(weapon, std::memory_order_relaxed);
    if (weapon >= 0) log::info("[wield] the copy's model: weapon %d (%s)", weapon, sidearm_fragment(weapon) ? sidearm_fragment(weapon) : "?");
}
int copy_model() { return g_copy_model.load(std::memory_order_relaxed); }
bool copy_as_prop() { return g_copy_as_prop.load(std::memory_order_relaxed); }
void set_copy_as_prop(bool on, bool save) {
    if (g_copy_as_prop.exchange(on) != on) log::info("[wield] the copy of the same model shown as a prop of that model: %s", on ? "on" : "off");
    if (save) config::set("Hands", "CopyAsProp", on ? "1" : "0");
}
const char* sidearm_fragment(int weapon) {
    static const char* const kFrag[8] = {"pistol_volcanic01x", "pistol_semiauto01x", "pistol_highpower01x", "pistol_mauser01x",
                                         "revolver_cattleman01x", "revolver_schofield01x", "revolver_doubleaction01x", "revolver_lemat01x"};
    return weapon >= 0 && weapon < 8 ? kFrag[weapon] : nullptr;
}
bool sidearm_muzzle(int weapon, float out[3]) {
    // each model's "muzzle_locator" (on its root bone), read from fragments.rpf in run 5: the barrel's end ahead (-z) and
    // above (+y) the grip
    static const float kMuzzle[8][3] = {{0.0f, 0.0521f, -0.1871f}, {0.0f, 0.0581f, -0.1905f}, {0.0f, 0.0698f, -0.1558f}, {-0.0016f, 0.0629f, -0.1881f},
                                        {0.0f, 0.0640f, -0.1919f}, {0.0f, 0.0549f, -0.2190f}, {0.0f, 0.0607f, -0.2209f}, {0.0f, 0.0572f, -0.2152f}};
    if (weapon < 0 || weapon >= 8) return false;
    std::memcpy(out, kMuzzle[weapon], sizeof(kMuzzle[weapon]));
    return true;
}
void set_copy_enabled(bool on) {
    std::lock_guard lock(g_mx);
    if (g_copy_en.exchange(on) != on) log::info("[wield] the copy %d", on ? 1 : 0);
    if (!on && g_want_slot.load() == kCopySlot) end("the copy turned off");
    config::set("Hands", "DualWieldCopy", on ? "1" : "0");
}
uintptr_t copy_W() { return g_copy_W.load(std::memory_order_relaxed); }
void note_copy_placed(uintptr_t W, const float T[16]) {
    if (!W || W != g_copy_W.load(std::memory_order_relaxed)) return;
    std::memcpy(g_copy_T, T, sizeof(g_copy_T));
    g_copy_T_ok = true;
    g_copy_placed_n.store(g_tick_n.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
bool copy_armed(uintptr_t W) { return W && W == g_copy_armed; }
bool copy_T(float T[16]) {
    if (!g_copy_T_ok) return false;
    std::memcpy(T, g_copy_T, sizeof(g_copy_T));
    return true;
}
bool load_copy_round(int weapon, float spare, int32_t actor) {
    std::lock_guard lock(g_mx);
    if (g_want_slot.load() != kCopySlot || g_copy_clip.load() + 0.5f > g_copy_max.load() || spare < 1.0f) return false;
    set_spare(actor, sidearm_ammo(weapon), spare - 1.0f);
    g_copy_clip.fetch_add(1.0f);
    g_copy_loads.fetch_add(1, std::memory_order_relaxed);
    log::info("[wield] the copy squeezed at the chest: one round (clip %.0f of %.0f, spare %.0f)", g_copy_clip.load(), g_copy_max.load(), spare - 1.0f);
    return true;
}

bool enabled() { return g_enabled.load(); }
void set_enabled_session(bool on) {
    std::lock_guard lock(g_mx);
    if (g_enabled.exchange(on) != on) log::info("[wield] dual wield %d", on ? 1 : 0);
    if (!on && g_want_slot.load() != kCopySlot) end("dual wield turned off");  // a copy keeps its own switch
}
void set_enabled(bool on) {
    set_enabled_session(on);
    config::set("Hands", "DualWield", on ? "1" : "0");
}

void begin(int ctrl, int john, int slot, const char* zone) {
    std::lock_guard lock(g_mx);
    const bool copy = slot == kCopySlot;
    if (copy ? !g_copy_en.load() : !g_enabled.load()) return;
    if (ctrl < 0 || ctrl > 1 || john < 0 || john > 1 || (!copy && (slot < 0 || slot > 7))) return;
    if (copy) {  // its own clip, loaded from the spare rounds (as many as the gun in hand holds)
        RdrvrActorState st{};
        if (!api::actor_state(&st) || sidearm_ammo(st.weapon) < 0) return;
        const float n = st.spare < st.clip_max ? (st.spare > 0 ? st.spare : 0.0f) : st.clip_max;
        set_spare(st.actor, sidearm_ammo(st.weapon), st.spare - n);
        g_copy_clip.store(n);
        g_copy_max.store(st.clip_max);
        g_copy_weapon.store(st.weapon);
        g_copy_T_ok = false;
        log::info("[wield] the copy loaded: %.0f of %.0f from the spare rounds (%.0f left)", n, st.clip_max, st.spare - n);
    }
    g_want_slot.store(slot, std::memory_order_relaxed);
    g_want_john.store(john, std::memory_order_relaxed);
    g_want_ctrl.store(ctrl, std::memory_order_relaxed);
    std::snprintf(g_zone, sizeof(g_zone), "%s", zone ? zone : "?");
    g_want_gen.fetch_add(1, std::memory_order_release);
    g_begins.fetch_add(1, std::memory_order_relaxed);
    if (copy)
        log::info("[wield] %s hand at the %s: a copy of the sidearm in hand as the second (John's %s hand)", ctrl ? "right" : "left", g_zone,
                  john ? "right" : "left");
    else
        log::info("[wield] %s hand at the %s: the gun of slot %d as the second (John's %s hand)", ctrl ? "right" : "left", g_zone, slot,
                  john ? "right" : "left");
}

void end(const char* why) {
    std::lock_guard lock(g_mx);
    if (g_want_ctrl.load() < 0) return;
    const int ctrl = g_want_ctrl.load(), slot = g_want_slot.load();
    if (slot == kCopySlot) {  // the copy's rounds back to the spare
        RdrvrActorState st{};
        const float left = g_copy_clip.exchange(0.0f);
        const int w = g_copy_weapon.load();
        if (left > 0.0f && api::actor_state(&st) && st.actor) set_spare(st.actor, sidearm_ammo(w), st.spare + left);
    }
    g_want_ctrl.store(-1, std::memory_order_relaxed);
    g_want_slot.store(-1, std::memory_order_relaxed);
    g_want_john.store(-1, std::memory_order_relaxed);
    g_want_gen.fetch_add(1, std::memory_order_release);
    g_ends.fetch_add(1, std::memory_order_relaxed);
    if (const int fs = g_forced.exchange(-1); fs >= 0) {  // the slot's force flag cleared again (the gun stays shown on its holster)
        RdrvrActorState st{};
        if (api::actor_state(&st) && st.actor) {
            const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(fs), 0ull};
            api::queue_native(0x1511D111, args, 3, 0, nullptr);
        }
    }
    log::info("[wield] the second gun (slot %d, the %s hand) put back: %s", slot, ctrl ? "right" : "left", why ? why : "?");
}

State state() {
    State s;
    s.ctrl = g_want_ctrl.load(std::memory_order_relaxed);
    s.on = s.ctrl >= 0;
    if (!s.on) return s;
    s.john = g_want_john.load(std::memory_order_relaxed);
    s.slot = g_want_slot.load(std::memory_order_relaxed);
    s.copy = s.slot == kCopySlot;
    s.ready = (s.copy ? g_copy_W.load(std::memory_order_relaxed) : g_W.load(std::memory_order_relaxed)) != 0 &&
              g_valid_gen.load(std::memory_order_acquire) == g_want_gen.load(std::memory_order_acquire);
    if (s.copy) {
        s.weapon = g_copy_weapon.load(std::memory_order_relaxed);
        s.clip = g_copy_clip.load(std::memory_order_relaxed);
        s.copy_max = g_copy_max.load(std::memory_order_relaxed);
    } else if (s.ready) {
        s.weapon = g_weapon.load(std::memory_order_relaxed);
        s.clip = g_clip.load(std::memory_order_relaxed);
    }
    return s;
}

int copy_john() { return g_want_john.load(std::memory_order_relaxed); }

uintptr_t secondary_W(int* john, uintptr_t* item) {
    const uintptr_t W = g_W.load(std::memory_order_relaxed);
    if (john) *john = g_john.load(std::memory_order_relaxed);
    if (item) *item = g_item.load(std::memory_order_relaxed);
    return W;
}
void note_placed(uintptr_t W) {
    g_placed_W.store(W, std::memory_order_relaxed);
    g_placed_n.store(g_tick_n.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
bool is_secondary_W(uintptr_t W) { return W && W == g_W.load(std::memory_order_relaxed); }

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    bool copy_status = false;
    while (in >> w) {
        if (w == "on" || w == "off") {
            set_enabled_session(w == "on");
        } else if (w == "begin") {  // dual begin <slot> [left|right]: the free hand (the other than the gun's) takes that slot's gun
            int slot = -1;
            std::string side;
            in >> slot >> side;
            const int ctrl = side == "left" ? 0 : side == "right" ? 1 : 0;
            begin(ctrl, ctrl, slot, "test");
        } else if (w == "copy") {  // dual copy [left|right]: that hand takes a copy of the sidearm in hand
            std::string side;
            in >> side;
            const int ctrl = side == "right" ? 1 : 0;
            begin(ctrl, ctrl, kCopySlot, "test");
        } else if (w == "copyst") {
            copy_status = true;
        } else if (w == "own") {  // dual own on|off: the copy shows the other sidearm's model (this session; DualWieldOwnModel)
            std::string v;
            in >> v;
            g_own_model.store(v != "off");
        } else if (w == "asprop") {  // dual asprop on|off: CopyAsProp for this session (run 7 item 1)
            std::string v;
            in >> v;
            set_copy_as_prop(v != "off", false);
        } else if (w == "sameits") {  // dual sameits on|off: DualWieldSameAtItsHolster for this session
            std::string v;
            in >> v;
            g_same_at_its.store(v != "off");
        } else if (w == "copyen") {
            std::string v;
            in >> v;
            g_copy_en.store(v != "off");
        } else if (w == "fire") {
            if (g_W.load() || g_copy_W.load()) g_fire_req.fetch_add(1, std::memory_order_release);
        } else if (w == "end") {
            end("the test channel");
        } else if (w == "show") {  // dual show on|off: ACTOR_FORCE_WEAPON_RENDER(actor, the second's slot, flag), 3 arguments
            std::string v;
            in >> v;
            RdrvrActorState st{};
            const int slot = g_want_slot.load();
            if (slot >= 0 && api::actor_state(&st) && st.actor) {
                const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(slot), v == "off" ? 0ull : 1ull};
                api::queue_native(0x1511D111, args, 3, 0, nullptr);
                log::info("[wield] ACTOR_FORCE_WEAPON_RENDER(slot %d, %d) (the test channel)", slot, v == "off" ? 0 : 1);
            }
        }
    }
    if (copy_status) {  // dual copyst: the copy's own line (the full one runs past the channel's 512 characters)
        const State cs = state();
        char cb[300];
        std::snprintf(cb, sizeof(cb), "copy %d | second: %s, copy %d, ctrl %d, John's hand %d, weapon %d, clip %.0f/%.0f | shots %llu, loads %llu, refused %llu, "
                      "unplaced %llu, clicks %llu, placed %d, why %d, flashes %llu",
                      g_copy_en.load() ? 1 : 0, cs.on ? "out" : "none", cs.copy ? 1 : 0, cs.ctrl, cs.john, cs.weapon, cs.clip, cs.copy_max,
                      static_cast<unsigned long long>(g_copy_shots.load()), static_cast<unsigned long long>(g_copy_loads.load()),
                      static_cast<unsigned long long>(g_refused.load()), static_cast<unsigned long long>(g_unplaced.load()),
                      static_cast<unsigned long long>(g_clicks.load()), g_copy_T_ok ? 1 : 0, g_invalid.load(),
                      static_cast<unsigned long long>(g_copy_flashes.load()));
        return cb;
    }
    const State s = state();
    char b[480];
    std::snprintf(b, sizeof(b),
                  "dual %d | second: %s, ready %d, ctrl %d, John's hand %d, slot %d, weapon %d, clip %.0f, W %p | shots %llu, refused %llu "
                  "(last %d), faults %llu, pulls %llu (unplaced %llu), clicks %llu, begins %llu, ends %llu, ticks %llu, why %d",
                  g_enabled.load() ? 1 : 0, s.on ? "out" : "none", s.ready ? 1 : 0, s.ctrl, s.john, s.slot, s.weapon, s.clip,
                  reinterpret_cast<void*>(g_W.load()), static_cast<unsigned long long>(g_shots.load()),
                  static_cast<unsigned long long>(g_refused.load()), g_last_ret.load(), static_cast<unsigned long long>(g_faults.load()),
                  static_cast<unsigned long long>(g_pulls.load()), static_cast<unsigned long long>(g_unplaced.load()),
                  static_cast<unsigned long long>(g_clicks.load()),
                  static_cast<unsigned long long>(g_begins.load()), static_cast<unsigned long long>(g_ends.load()),
                  static_cast<unsigned long long>(g_ticks.load()), g_invalid.load());
    return b;
}

}  // namespace rdrvr::dual
