#include "core/dual.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/actions.h"
#include "core/aim.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/body.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/d3d_hooks.h"
#include "core/hands.h"
#include "core/holster.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/pose.h"
#include "core/offhand.h"
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
// (2026-10-27, [Hands] DualWieldCrossReload) the second gun's clip size and its ammo type's spare rounds, read by
// resolve() each tick (game memory only, as FUN_140300f40 reads them; -1 not read)
std::atomic<float> g_clip_max{-1.0f}, g_spare{-1.0f};
std::atomic<uint64_t> g_clip_tick{0};  // the script tick when the game thread last read them (load_second's pending)
// load_second's last ADD (under g_mx): its id, the tick queued, the tick it ran on (0 not known), the rounds not yet seen
uint64_t g_sec_id = 0, g_sec_q = 0, g_sec_ran = 0;
float g_sec_m = 0.0f;
// (the review) the copy's absolute spare writes waiting for a settled inventory (frame(): spare_writes), under g_mx:
// end()'s give-back of its rounds and begin()'s fill, each since when
// (the re-review) one give-back per ammo type (the copy's sidearms: 6 the pistols, 7 the revolvers), never merged across
// types; each, and the fill, with the player it was queued for (the actor handle and its object's): another one (a
// load, a new game) drops it, logged
constexpr int kGiveTypes = 17;  // (kAmmoTypes, below)
float g_giveback[kGiveTypes] = {};
double g_giveback_ms[kGiveTypes] = {};
int32_t g_giveback_actor[kGiveTypes] = {}, g_giveback_obj[kGiveTypes] = {};
// (2026-11-01, the off-hand build step 3, R17) the carried gun keeps its count: the game reloads a gun out of the hand from
// the spare (r313 B36: (clip, spare) (7, 90) -> (10, 87) a second after the carry, once; not the reload request
// reload.cpp gates), so with HandReload the game thread puts the clip back and hands the rounds to frame() for the spare
// (the copy's give-back); the mod's own loads (load_second) adopted for 1.5 s after each
std::atomic<int> g_sec_ammo_type{-1};      // the second gun's ammo type, as resolve read it
std::atomic<float> g_carry_give{0.0f};     // rounds the game thread took back from the carried gun, for the spare
std::atomic<int> g_carry_give_type{-1};
std::atomic<uint64_t> g_carry_adopt_tick{0};  // (the review) the mod's own load's rise adopted until this script tick
std::atomic<float> g_carry_adopt_n{0.0f};      // and no more than its rounds
std::atomic<float> g_carry_clip0{-1.0f};       // the clip at the hand-over (begin_carry's read of its W): the count it keeps
std::atomic<uint64_t> g_carry_kept{0};     // the refills undone
bool g_fill_pending = false;
double g_fill_ms = 0.0;
int32_t g_fill_actor = 0, g_fill_obj = 0;
std::atomic<bool> g_cross{false};  // [Hands] DualWieldCrossReload
std::atomic<bool> g_beside_torch{false};  // [Hands] DualWieldBesideTorch
// [Hands] DualWieldBesideItems (2026-10-31, the off-hand build step 6; the user's answer 3: a gun beside the knife or a
// throwable): the knife (22) or a throwable in hand takes a second gun as the torch does (the second gun's trigger is
// W-local: it needs no gun in hand). Never beside the lasso (answer 10: "No off hand weapon with the lasso")
std::atomic<bool> g_beside_items{true};
std::atomic<uint64_t> g_cross_loads{0}, g_cross_rounds{0};
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
// [Hands] CarryByForegrip: the carry. begin_carry (the frame end) writes P, the gun as drawn at the hand-over, while
// g_carry_seq is odd; the game thread makes T from it in the carried gun's first holstered update (carry_T_set: its own
// seqlock, g_carry_tseq) and every later update reads that T
std::atomic<bool> g_carry_en{true};
std::atomic<bool> g_carry{false}, g_carry_ret{false};
std::atomic<uint32_t> g_carry_seq{0}, g_carry_tseq{0};
float g_carry_P[16] = {};
float g_carry_hand0[3] = {};  // John's front hand's target when P was (body_points), in g_carry_seq with P
float g_carry_rot0[9] = {};   // and its axes (columns)
float g_carry_T[16] = {};
std::atomic<bool> g_carry_T_made{false};
std::atomic<double> g_carry_ms{0.0}, g_carry_ret_ms{0.0};  // begin_carry's time, carry_return's
std::atomic<int> g_carry_weapon{-1};
std::atomic<uint64_t> g_carries{0}, g_carry_takes{0};
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
std::atomic<int> g_shot_weapon{-1};  // [Hands] GunKick (2026-10-27): the eWeapon of the last second gun's or copy's shot
float g_copy_T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // the game thread's (note_copy_placed)
bool g_copy_T_ok = false;
uintptr_t g_copy_armed = 0;  // the game thread's: the W whose shot in this weapon tick is the copy's
float g_copy_restore = 0.0f;  // the gun in hand's clip before the copy's shot (given back after it)
// (2026-11-04, GOAL-older-items item 28; the user: "making the second gun equipped when dual weilding have its own damage
// and fire rate") [Hands] CopyOwnRate: the copy shoots through the gun in hand's weapon object, whose trigger holds a shot
// until its ChamberTime has passed since W +0x130, its last shot: the copy waited on the gun in hand's interval and each
// gun on the other's shots. With it on, the copy keeps its own last shot (game time) and waits the shown gun's
// ChamberTime (its weapon info's tuning, +0xd4; the gun in hand's own when it shows that gun); for its pull the gun in
// hand's last shot is put back by that gun's interval (the game's gate passed), and after the weapon tick that fired it,
// W +0x130 is the gun in hand's own last shot again (its next shot waits on its own shots only). Not in Dead Eye (the
// game's own gate, as before)
std::atomic<bool> g_copy_rate{true};
float g_copy_last = -1.0e9f;  // the game thread's: the copy's last shot (game time)
float g_copy_t130 = 0.0f;  // the gun in hand's W +0x130 before the copy's shot (put back after the weapon tick)
bool g_copy_t130_set = false;
std::atomic<float> g_copy_iv{-1.0f}, g_copy_hand_iv{-1.0f};  // the last intervals used (s): the copy's, the gun in hand's
std::atomic<int> g_copy_iv_of{-1};  // the gun whose ChamberTime the copy waited (the shown model, else the gun in hand)
std::atomic<uint64_t> g_copy_waits{0}, g_copy_own_shots{0};  // pulls held by the copy's own interval; shots by it
std::atomic<uint64_t> g_copy_same_tick{0};  // pulls held a tick: the gun in hand fired in it
uintptr_t g_copy_flash_src = 0;  // the muzzle flash's own matrix (the prop's muzzle bone), followed through T
uint64_t g_copy_flash_until = 0;
alignas(16) float g_copy_bone[16] = {};  // the copy's muzzle flash follows this (MuzzleFxMatrix's pointer)
// the sidearms' ammo types (Enums.h AmmoType): the pistols 0-3 AMMO_TYPE_PISTOL 6, the revolvers 4-7 AMMO_TYPE_REVOLVER 7
// (2026-11-01, the off-hand build step 4, P2) and the Sawed-off (it pairs as a sidearm): a copy of it, its shells (10)
int sidearm_ammo(int w) { return w >= 0 && w <= 3 ? 6 : w >= 4 && w <= 7 ? 7 : w == 15 ? 10 : -1; }
constexpr int kAmmoTypes = 17;  // the AmmoType enum's (Enums.h: 0-16): an inventory entry is read only within it
static_assert(kAmmoTypes == kGiveTypes, "one give-back slot per ammo type");
uint64_t fbits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
// ACTOR_SET_INV_AMMO(actor, ammo type, count, 0): the spare rounds of that type (a script tick)
// (2026-10-27, DualWieldCrossReload's review) not a NaN; noted for reload::spare_natives_done (an absolute write: the next
// one waits for it to have run)
void set_spare(int32_t actor, int type, float count) {
    if (!actor || type < 0 || !std::isfinite(count)) return;
    const uint64_t args[4] = {static_cast<uint64_t>(static_cast<uint32_t>(actor)), static_cast<uint64_t>(type), fbits(count < 0 ? 0.0f : count), 0ull};
    reload::note_spare_native(api::queue_native(0x4372593E, args, 4, 0, nullptr));
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

// (2026-10-27, DualWieldCrossReload's review: the actor state's spare is a tick old, and an absolute spare set from it
// after another queued change lost rounds) the player's spare rounds of an ammo type now, from the game's own inventory,
// as resolve reads it (wmgr +0x418 -> + type * 12: {float count, float max, byte infinite}): -1 not read, 9999 infinite.
// Reads only (SEH)
float inv_spare(int type) {
    if (type < 0 || type >= kAmmoTypes) return -1.0f;
    const uintptr_t actor = player_actor();
    uintptr_t wmgr = 0, inv = 0;
    if (!actor || !rd(actor + 0x70, &wmgr) || !wmgr || !rd(wmgr + 0x418, &inv) || !inv) return -1.0f;
    float c = -1.0f, mx = -1.0f;
    uint8_t inf = 0;
    const uintptr_t e = inv + static_cast<uintptr_t>(type) * 12;
    if (!rd(e, &c) || !rd(e + 4, &mx) || !rd(e + 8, &inf) || !std::isfinite(c) || c < 0.0f) return -1.0f;
    if (inf) return 9999.0f;
    // (the re-review) a recycled read is never written back as an absolute spare: a count over the entry's own max (+4)
    // is refused, not clamped (the third review: if +4 is not the live cap, a 0 when unset or a capacity upgrade raising
    // the count first, a clamp would write mx - n and delete rounds; a refusal only waits)
    if (!std::isfinite(mx) || mx < 0.0f || mx > 100000.0f || c > mx) return -1.0f;
    return c;
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
    {  // (2026-10-27, DualWieldCrossReload) its capacity and spare rounds, as ACTOR_ADD_WEAPON_AMMO's FUN_140300f40 reads
       // them (Ghidra, this session): the capacity TD +0xc4 (a float; +0xc8 only in multiplayer); the inventory
       // wmgr +0x410, its entries at [+0x418] + ammo type (def +0x10, s16; < 0 none) * 12: {float count, float max, byte
       // infinite}. Reads only (SEH), nothing written
        float cap = -1.0f, spare = -1.0f;
        if (!rd(td + 0xc4, &cap) || !std::isfinite(cap) || cap < 0.0f || cap > 500.0f) cap = -1.0f;
        int16_t at = -1;
        uintptr_t inv = 0;
        g_sec_ammo_type.store(rd(info + 0x10, &at) ? at : -1, std::memory_order_relaxed);
        if (rd(info + 0x10, &at) && at >= 0 && at < kAmmoTypes && rd(wmgr + 0x418, &inv) && inv) {
            float c = -1.0f;
            uint8_t inf = 0;
            if (rd(inv + static_cast<uintptr_t>(at) * 12, &c) && rd(inv + static_cast<uintptr_t>(at) * 12 + 8, &inf) && std::isfinite(c) && c >= 0.0f)
                spare = inf ? 9999.0f : c;
        }
        g_clip_max.store(cap, std::memory_order_relaxed);
        g_spare.store(spare, std::memory_order_relaxed);
    }
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

// (2026-10-29, the review of 471af5a: DualWieldBesideTorch makes the trigger with a melee item in hand a supported path)
// The trigger FUN_1403b4660's listener notify (0x1403b497e-0x1403b4a2a) reads the owner O = ActorPool[item +0x28] (its
// low 16 bits the index, the high 16 its generation, checked at 0x1403b46b3), then L = [O +0xb0] (0x1403b497e) and
// [L +0x8e8] (0x1403b49e5), takes a global recursive critical section (0x15a495680, by FUN_140c0ffd0 and FUN_140eb4070)
// and only then reads the list's head [[L +0x8e8]] (0x1403b49fc), unchecked. A fault there, caught by call_trigger's SEH,
// leaves that lock held: any other thread taking it hangs. So the same chain is read first (SEH-guarded reads), and the
// shot is refused unless every link is there: 0 ok; 1 no actor pool, 2 the owner's generation, 3 no owner, 4 no
// [O +0xb0], 5 no [L +0x8e8], 6 its head unreadable. The listeners' own objects past the head are the game's (not read)
std::atomic<uint64_t> g_notify_refused{0};
std::atomic<int> g_notify_why{0};
int notify_chain(uintptr_t item) {
    uint32_t h = 0;
    uintptr_t apool = 0, owner = 0, L = 0, list = 0, head = 0;
    uint16_t gen = 0;
    if (!rd(anchors::addr(anchors::Id::ActorPool), &apool) || !apool || !rd(item + 0x28, &h)) return 1;
    const uintptr_t slot = apool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16)) return 2;
    if (!rd(slot, &owner) || !owner) return 3;
    if (!rd(owner + 0xb0, &L) || !L) return 4;
    if (!rd(L + 0x8e8, &list) || !list) return 5;
    if (!rd(list, &head)) return 6;  // (a null head is the game's own empty list: its loop ends at once)
    return 0;
}
// true: the chain read whole (the trigger may be called); false: refused, counted, said once
bool notify_ok(uintptr_t item, const char* what, int weapon) {
    const int why = notify_chain(item);
    if (!why) return true;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    g_notify_refused.fetch_add(1, std::memory_order_relaxed);
    if (g_notify_why.exchange(why) != why)
        log::warn("[wield] %s's trigger not called (weapon %d): its owner's listener chain is not whole (%d), the shot refused", what, weapon, why);
    return false;
}
// a tuning's ChamberTime: FUN_140301270's (W +0x138 if not 0, else the variant's OpMode +0xd4); info alone (W 0): its
// first variant's. False: not read or not plausible (0..10 s)
bool chamber_time(uintptr_t W, uintptr_t info, float* out) {
    float iv = 0.0f;
    if (W && !rd(W + 0x138, &iv)) return false;
    if (!(iv > 0.0f)) {
        int32_t var = 0;
        uintptr_t td = 0;
        if (W && (!rd(W + 0x28, &info) || !info)) return false;
        if (!info) return false;
        if (!W || !rd(W + 0x30, &var) || var < 0 || var > 7 || !rd(info + 0x30 + static_cast<uintptr_t>(var) * 8, &td) || !td) rd(info + 0x30, &td);
        if (!td || !rd(td + 0xd4, &iv)) return false;
    }
    if (!std::isfinite(iv) || iv <= 0.0f || iv > 10.0f) return false;
    *out = iv;
    return true;
}

// The copy's shot: the gun in hand's own trigger, W +0x9b8 set (its shot along its barrel, no target), a round lent
// to its clip if it is empty (the game refuses an empty gun and mutes its gunshot), its clip given back after the weapon
// tick (the copy's round is the mod's count). The shot's launch and its muzzle flash are moved onto the copy by T.
bool fire_copy(uintptr_t item, uintptr_t W, int weapon) {  // true: held for the next tick (CopyOwnRate)
    const uintptr_t base = anchors::base();
    uintptr_t vt = 0, fn = 0;
    float c0 = 0.0f;
    if (!rd(item, &vt) || vt != anchors::addr(anchors::Id::GunItemVtbl) || !rd(vt + 0x30, &fn) || fn < base || fn >= base + 0x4000000 ||
        !rd(W + 0x12c, &c0)) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (g_copy_clip.load(std::memory_order_relaxed) < 0.5f) return false;
    if (!notify_ok(item, "the copy", weapon)) return false;
    // [Hands] CopyOwnRate: its own interval (above)
    float gt = 0.0f, t130 = 0.0f, hand_iv = 0.0f;
    bool own = false;
    if (g_copy_rate.load(std::memory_order_relaxed)) {
        RdrvrActorState st{};
        own = api::actor_state(&st) && !(st.weapon_flags & RDRVR_WEAPON_DEADEYE) && rd(anchors::addr(anchors::Id::GameTime), &gt) &&
              std::isfinite(gt) && rd(W + 0x130, &t130) && std::isfinite(t130) && chamber_time(W, 0, &hand_iv);
    }
    if (own) {
        const int shown = copy_model();
        float civ = hand_iv;
        const uintptr_t sinfo = shown >= 0 && shown != weapon ? offhand::info_of(shown) : 0;
        const bool by_shown = sinfo && chamber_time(0, sinfo, &civ);
        if (!by_shown) civ = hand_iv;
        const int of = by_shown ? shown : weapon;
        if (g_copy_iv_of.exchange(of, std::memory_order_relaxed) != of || g_copy_iv.load(std::memory_order_relaxed) != civ)
            log::info("[wield] the copy's own interval: %.3f s (%s weapon %d's ChamberTime; the gun in hand's %.3f s)", static_cast<double>(civ),
                      by_shown ? "the shown" : "the gun in hand,", of, static_cast<double>(hand_iv));
        g_copy_iv.store(civ, std::memory_order_relaxed);
        g_copy_hand_iv.store(hand_iv, std::memory_order_relaxed);
        if (gt >= g_copy_last && gt - g_copy_last < civ) {  // (the clock gone back, a load: not held)
            g_copy_waits.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // (the review) the gun in hand's own shot this tick (its trigger ran in the aim tick, before this): its round is
        // the tick's one shot; the copy's pull held for the next tick, not taking it over
        if (gt >= t130 && gt - t130 < 0.0005f) {
            g_copy_same_tick.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // the gun in hand's last shot put back by its interval for this pull (the game's gate passed); its own after the tick
        if (gt - t130 < hand_iv && !wr<float>(W + 0x130, gt - hand_iv - 0.001f)) own = false;
    }
    auto put_t130 = [&] {
        if (own) wr<float>(W + 0x130, t130);
    };
    uint8_t b8 = 0;
    rd(W + 0x9b8, &b8);
    if (c0 < 1.0f && !wr<float>(W + 0x12c, 1.0f)) {
        put_t130();
        return false;
    }
    if (!wr<uint8_t>(W + 0x9b8, 1)) {
        wr<float>(W + 0x12c, c0);
        put_t130();
        return false;
    }
    const uint64_t r = call_trigger(fn, item);
    if (r == ~0ull) {
        wr<uint8_t>(W + 0x9b8, b8);
        wr<float>(W + 0x12c, c0);
        put_t130();
        g_faults.fetch_add(1, std::memory_order_relaxed);
        g_copy_en.store(false);  // (later pulls only: a lock the fault left held stays held, as the second gun's below)
        log::error("[wield] the copy's trigger faulted (weapon %d): the copy off for this session", weapon);
        return false;
    }
    g_last_ret.store(static_cast<int>(r & 0xff), std::memory_order_relaxed);
    if ((r & 0xff) == 0) {
        wr<uint8_t>(W + 0x9b8, b8);
        wr<float>(W + 0x12c, c0);
        put_t130();
        g_refused.fetch_add(1, std::memory_order_relaxed);
        log::info("[wield] the copy's trigger: refused by the game (weapon %d; the fire interval or a reload)", weapon);
        return false;
    }
    g_copy_armed = W;
    g_copy_restore = c0;
    if (own) {  // (the trigger set W +0x130 to now: the gun in hand's own put back after the weapon tick)
        g_copy_last = gt;
        g_copy_t130 = t130;
        g_copy_t130_set = true;
        g_copy_own_shots.fetch_add(1, std::memory_order_relaxed);
    }
    g_copy_clip.fetch_sub(1.0f, std::memory_order_relaxed);  // the game thread's shot and the frame end's load: atomic (review)
    g_shot_weapon.store(weapon, std::memory_order_relaxed);
    g_copy_shots.fetch_add(1, std::memory_order_release);
    log::info("[wield] the copy fired (weapon %d; its clip %.0f left, the gun in hand's %.0f kept)", weapon,
              g_copy_clip.load(std::memory_order_relaxed), c0);
    return false;
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
    if (!notify_ok(item, "the second gun", weapon)) return;
    uint8_t b8 = 0;
    rd(W + 0x9b8, &b8);
    if (!wr<uint8_t>(W + 0x9b8, 1)) return;
    const uint64_t r = call_trigger(fn, item);
    if (r == ~0ull) {
        wr<uint8_t>(W + 0x9b8, b8);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        // never again this session; the frame end ends it. (The review of 471af5a: this stops later pulls only. A fault
        // inside the trigger's listener notify leaves its critical section held, which nothing here releases: notify_ok's
        // reads before the call are what keeps the known unchecked read from faulting there)
        g_enabled.store(false);
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
    g_shot_weapon.store(weapon, std::memory_order_relaxed);
    g_shots.fetch_add(1, std::memory_order_release);
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
                // ([Hands] CopyOwnRate, the review) the gun in hand fired this tick: the copy's pull the next (3 at most)
                static int held = 0;
                if (fire_copy(item, W, weapon) && ++held <= 3) {
                    g_fire_done.store(req - 1, std::memory_order_relaxed);  // (not taken: asked again)
                    return;
                }
                held = 0;
            } else {
                g_unplaced.fetch_add(1, std::memory_order_relaxed);
                log::info("[wield] the copy's pull dropped: not placed in the hand this update");
            }
        }
        return;
    }
    const bool carry = g_carry.load(std::memory_order_acquire);  // [Hands] CarryByForegrip: with DualWield off too
    if (!(g_enabled.load(std::memory_order_relaxed) || carry) || slot < 0 || slot > 7 || john < 0 || john > 1) {
        drop(0);
        return;
    }
    uintptr_t item = 0, W = 0;
    int weapon = -1;
    float clip = -1.0f;
    if (const int why = resolve(slot, &item, &W, &weapon, &clip)) {
        // the carry's first second: the put-away may not be done (5, 6: still in hand), begin_carry's view is kept. Not
        // while taken back: in hand again is its end
        if (carry && (why == 5 || why == 6) && !g_carry_ret.load(std::memory_order_relaxed) &&
            log::now_ms() - g_carry_ms.load(std::memory_order_relaxed) < 1000.0)
            return;
        drop(why);
        return;
    }
    if (carry && reload::hand_reload()) {  // (R17) the carried gun keeps its count: the game's refill undone, its rounds back
        static uint64_t ks = ~0ull;
        static float keep = -1.0f;
        static int fixes = 0;
        const uint64_t ser = g_carries.load(std::memory_order_acquire);
        if (ser != ks) {
            ks = ser;
            // (the review) the count at the hand-over, else this first read (the refill may come before the first resolve)
            const float c0 = g_carry_clip0.load(std::memory_order_relaxed);
            keep = c0 >= 0.0f && c0 <= clip ? c0 : clip;
            fixes = 0;
        }
        if (keep < 0.0f || clip < keep - 0.5f) keep = clip;  // (a lower count adopted)
        const float adopt = g_carry_adopt_n.load(std::memory_order_relaxed);
        if (adopt > 0.0f && api::script_ticks() <= g_carry_adopt_tick.load(std::memory_order_relaxed) && clip > keep + 0.5f) {
            const float a = std::fmin(adopt, std::floor(clip - keep + 0.01f));  // (the mod's own load: no more than its rounds)
            keep += a;
            g_carry_adopt_n.store(adopt - a, std::memory_order_relaxed);
        }
        if (keep >= 0.0f && clip > keep + 0.5f && fixes < 5) {
            const float d = std::floor(clip - keep + 0.01f);
            const int t = g_sec_ammo_type.load(std::memory_order_relaxed);
            if (d >= 1.0f && t >= 0 && t < kGiveTypes && wr<float>(W + 0x12c, keep)) {
                ++fixes;
                g_carry_give_type.store(t, std::memory_order_relaxed);
                g_carry_give.fetch_add(d, std::memory_order_release);  // (the review: frame() may take it meanwhile)
                g_carry_kept.fetch_add(1, std::memory_order_relaxed);
                log::info("[wield] the carried gun (weapon %d): the game's refill undone (%.0f -> %.0f, %.0f round%s back to the spare): it keeps its "
                          "count (R17)%s",
                          weapon, clip, keep, d, d == 1.0f ? "" : "s", fixes == 5 ? "; no more this carry" : "");
                clip = keep;
            }
        }
    }
    g_item.store(item, std::memory_order_relaxed);
    g_W.store(W, std::memory_order_relaxed);
    g_john.store(john, std::memory_order_relaxed);
    g_weapon.store(weapon, std::memory_order_relaxed);
    g_clip.store(clip, std::memory_order_relaxed);
    g_clip_tick.store(api::script_ticks(), std::memory_order_relaxed);
    g_invalid.store(0, std::memory_order_relaxed);
    g_valid_gen.store(gen, std::memory_order_release);
    const uint32_t req = g_fire_req.load(std::memory_order_acquire);
    if (req != g_fire_done.load(std::memory_order_relaxed)) {
        g_fire_done.store(req, std::memory_order_relaxed);
        // only when this update's item placement put it in the hand (its W +0x80 and +0x920 the hand's): else the shot
        // would leave from its holster. A carried gun is never fired
        if (carry) {
            g_unplaced.fetch_add(1, std::memory_order_relaxed);
        } else if (g_placed_n.load(std::memory_order_relaxed) == n && g_placed_W.load(std::memory_order_relaxed) == W) {
            fire(item, W, weapon, clip);
        } else {
            g_unplaced.fetch_add(1, std::memory_order_relaxed);
            log::info("[wield] the second gun's pull dropped: not placed in the hand this update");
        }
    }
}

// FUN_14031a260(): every weapon object's tick (the update, after the items' placements and the player's aim tick)
void idle_fx_tick();  // below
void offhand_torch_fx();  // below
using WeaponTick_t = void (*)();
WeaponTick_t o_weapon_tick = nullptr;
std::atomic<int> g_quick_req{-1};  // the weapon cocked by hand whose next shot goes at once (actions.cpp), -1 none
std::atomic<double> g_quick_ms{0.0};
std::atomic<uint64_t> g_quick_done{0}, g_quick_noneed{0}, g_quick_refused{0};
std::atomic<float> g_quick_chamber{0.0f}, g_quick_phase{0.0f};  // the last ChamberTime read (s) and John's fire clip phase then
int resolve_copy(uintptr_t* item_out, uintptr_t* W_out, int* weapon_out, float* clip_out);
void quick_tick() {  // the game thread, before the game's weapon tick
    const int want = g_quick_req.exchange(-1, std::memory_order_acquire);
    if (want < 0) return;
    // (stale; or a copy out: it shares the gun in hand's W, its pull would take the interval: the review)
    if (log::now_ms() - g_quick_ms.load(std::memory_order_relaxed) > 1000.0 || g_copy_W.load(std::memory_order_relaxed)) {
        g_quick_refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RdrvrActorState st{};
    if (!api::actor_state(&st) || (st.weapon_flags & RDRVR_WEAPON_DEADEYE) || st.weapon != want) {
        g_quick_refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uintptr_t item = 0, W = 0, info = 0, td = 0;
    int wt = -1;
    float clip = -1.0f;
    if (resolve_copy(&item, &W, &wt, &clip) != 0 || wt != want || !(clip >= 1.0f)) {
        g_quick_refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float gt = 0.0f, last = 0.0f, iv = 0.0f;
    if (!rd(anchors::addr(anchors::Id::GameTime), &gt) || !rd(W + 0x130, &last) || !rd(W + 0x138, &iv)) {
        g_quick_refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!(iv > 0.0f)) {  // its tuning's ChamberTime (the weapon's variant's table, else the first)
        int32_t var = 0;
        if (!rd(W + 0x28, &info) || !info) return;
        if (!rd(W + 0x30, &var) || var < 0 || var > 7 || !rd(info + 0x30 + static_cast<uintptr_t>(var) * 8, &td) || !td) rd(info + 0x30, &td);
        if (!td || !rd(td + 0xd4, &iv)) return;
    }
    if (!std::isfinite(gt) || !std::isfinite(last) || !std::isfinite(iv) || iv <= 0.0f || iv > 10.0f) return;
    g_quick_chamber.store(iv, std::memory_order_relaxed);
    bool did = false;
    if (gt - last < iv) did = wr<float>(W + 0x130, gt - iv - 0.001f) || did;  // the interval over
    uintptr_t G = 0;
    float phase = -1.0f;
    if ((G = aim::player_gun_ctl()) != 0 && rd(G + 0x24, &phase)) {
        g_quick_phase.store(phase, std::memory_order_relaxed);
        uint8_t f = 0;
        if (phase >= 0.0f && rd(G + 0x5d8, &f) && !(f & 4)) did = wr<uint8_t>(G + 0x5d8, static_cast<uint8_t>(f | 4)) || did;  // the clip may end early
    }
    (did ? g_quick_done : g_quick_noneed).fetch_add(1, std::memory_order_relaxed);
}

void hk_weapon_tick() {
    quick_tick();
    if (g_enabled.load(std::memory_order_relaxed) || g_W.load(std::memory_order_relaxed) || g_copy_en.load(std::memory_order_relaxed) ||
        g_copy_W.load(std::memory_order_relaxed) || g_carry.load(std::memory_order_relaxed))
        tick();
    o_weapon_tick();
    offhand_torch_fx();  // [Hands] OffHandTorchLit: the off-hand torch's flame asked for, as the game asks the item in hand's
    idle_fx_tick();  // the held effects not asked for this tick
    if (const uintptr_t W = g_copy_armed) {  // the copy's shot is done: the gun in hand's clip as it was (the copy paid)
        wr<float>(W + 0x12c, g_copy_restore);
        if (g_copy_t130_set) wr<float>(W + 0x130, g_copy_t130);  // [Hands] CopyOwnRate: its own last shot again
        g_copy_t130_set = false;
        g_copy_armed = 0;
    }
    if (g_copy_flash_src && g_tick_n.load(std::memory_order_relaxed) <= g_copy_flash_until) {  // the flash follows the copy
        alignas(16) float src[16];
        if (raw(g_copy_flash_src, src, sizeof(src)) && g_copy_T_ok) mul44(src, g_copy_T, g_copy_bone);
    } else {
        g_copy_flash_src = 0;
    }
}

// 2026-10-26 (the user: "The torch and the molotovs fire effects don't follow the weapon, they appear where they would be
// in the normal animation"; research run 28): the held weapon's idle effect (the fire bottle's flame, the torch's) is
// started by FUN_1406b8440 at each weapon tick, following the matrix FUN_140301680 hands it (the prop's muzzle bone, else
// W +0x80): the game's animated hand, where the mod does not draw the prop (body.cpp places it at the player's hand). Its
// pointer is pointed at a matrix of ours: the followed one moved as the prop was, M G^-1 P (G: W +0x80, P: the prop as
// placed this frame), refreshed at every call while the effect lives (the effect keeps the pointer it was created with;
// a W of ours not placed now gets M itself, so its flame never freezes). The player's item only (an NPC's torch: placed
// never), at that call site only (the guns' muzzle flashes are FUN_1406b6280's). [Hands] HeldEffectsFollow (on).
std::atomic<bool> g_fx_follow{true};
uintptr_t g_idlefx_ret = 0;  // FUN_1406b8440's return address from FUN_140301680 (IdleFxMatrixRet)
// (the review) an effect outliving its calls (a dropped torch, a cutscene) follows what it would have followed: the
// weapon tick copies the last source (W +0x80 or the bone; SEH-read) into the buffer for every entry not refreshed by
// a call this tick. A slot is reused least recently called first.
struct FollowBuf {
    uintptr_t W = 0;
    uintptr_t src = 0;      // the matrix the effect would follow (FUN_140301680's), as last seen
    uint64_t last = 0;      // the weapon tick of the last call
    bool ours = false;      // ([Hands] OffHandTorchLit) last called by offhand_torch_fx: not refreshed from src when stale
    double take_ms = 0.0;   // (round 80) this take's first ask (after a tick without one), its line told once
    bool told = false;
    alignas(16) float m[16] = {};
};
FollowBuf g_follow[8];
uint64_t g_fx_tick = 0;  // the weapon ticks (the game thread)
std::atomic<uint64_t> g_idlefx_calls{0}, g_idlefx_moved{0}, g_idlefx_plain{0}, g_idlefx_stale{0}, g_idlefx_bad{0}, g_idlefx_leaving{0};
// (2026-11-04, round 80, the user: the torch's flame "always where it would be in johns normal animations", held too; "dual
// fxwhere") the last held effect placed (not the off-hand torch's): its place from the drawn item's origin, the game's own
// from W +0x80, both in mm (a rigid carry: the two the same), and which it followed (1 W +0x80, 2 a bone)
std::atomic<float> g_fxw_ours_mm{-1.0f}, g_fxw_game_mm{-1.0f}, g_fxw_hand_mm{-1.0f};
std::atomic<int> g_fxw_src{0};
std::atomic<int16_t> g_fxw_weapon{-1};
std::atomic<uintptr_t> g_fxw_W{0};
// (2026-11-04, round 80: the torch's flame "always where it would be in johns normal animations", held too) FUN_1406b8440
// keeps the matrix FUN_140301680 hands it as the effect's followed matrix (fx +0x170) only when the effect is made
// (0x1406b8613; one found keyed by W keeps its own: 0x1406b85f0 jumps past it). Its first ask can come while the item
// is still being drawn, not yet placed at the hand: given the game's own matrix then, the flame followed the game's
// animated torch for the effect's life, whatever was placed after. So every ask for one of the player's own weapon
// objects (the item in hand's, an equip slot's) is given our buffer from the first, the game's own place in it while
// not placed. (bound: buffers made so, before the item was placed)
std::atomic<uint64_t> g_idlefx_bound{0};
std::atomic<bool> g_fx_bind{true};  // ("dual idlefx bind off": the test's, the old way for comparison)
// o = a b (4x4, rows; row vectors: a point v goes to v a b)
void mul44r(const float* a, const float* b, float* o) {
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    std::memcpy(o, t, sizeof(t));
}
// 2026-10-29, [Hands] OffHandTorchLit (off; Roo Code's change, ported in round 55; offhand.h; research run28
// torch_second_gun.md way (d'), run30 torch.md): the off-hand torch (our held_prop copy, the gun the item in hand) has no
// flame: FUN_140304120 asks FUN_1406b8440 for the idle effect only of a W both active and shown (W +0x120 bits 5 and 6),
// the item in hand's. Here, after the game's weapon tick (the game thread, where the game's own call runs), the same call
// the game makes for the torch (its default case: 0x14030417b jumps to the call at 0x140304464, no other check;
// arguments loaded at 0x140304453-0x14030445d): FUN_1406b8440(0, W, IdleFxOffset, IdleFxRot) with the torch's own W (its
// equip slot's item +0xa0; eWeapon 33 at [W +0x28] +8), every pointer it reads unchecked checked first ([W +0x28], its
// +0x3b8 and +0x78, that def's vtable). The effect is the game's, found or made keyed by W, kept alive by being asked
// each tick: an entry not asked in a pass is stopped at once by the reaper (FUN_14064a9e0 0x14064b36b-0x14064b3f5,
// timeout 0; its particles fade, the entry freed when finished) [V: the review's reading]. Its followed matrix is
// FUN_140301680's (the W's prop bone, else W +0x80: the torch at its holster), which idle_fx below points at the drawn
// torch (g_ohfx_P) as it points the item in hand's at its placed prop; after the let-go the fading flame stays where the
// drawn torch last was (FollowBuf::ours), not at the holster. No torch W (the torch in no equip slot): unlit, logged
// once a take.
// (g_ohfx_flips: the placement changing between ticks within a take: 2 and 3 are chosen by the bone's distance against
// 1 m with no hysteresis)
std::atomic<uint64_t> g_ohfx_calls{0}, g_ohfx_now{0}, g_ohfx_nobj{0}, g_ohfx_w80{0}, g_ohfx_bone{0}, g_ohfx_far{0}, g_ohfx_flips{0};
// the last reason not asked: 0 asked, 1 off (OffHandTorchLit or OffHandItems), 2 no torch drawn, 3 no player, 4 no torch
// W, 5 its effect def not readable, 6 not available (an anchor missing, HeldEffectsFollow off, a hook not installed)
std::atomic<int> g_ohfx_why{0};
std::atomic<int> g_ohfx_path{0};  // idle_fx's last placement of it: 0 none yet, 1 W +0x80 (no prop bone), 2 a bone within 1 m, 3 a bone over 1 m (the drawn torch's origin)
std::atomic<float> g_ohfx_bone_m{0.0f};  // that bone's distance from W +0x80 then (m)
thread_local uintptr_t g_ohfx_W = 0;  // the torch W being asked for now, on this thread (idle_fx's)
alignas(16) float g_ohfx_P[16];  // its drawn pose then
std::atomic<uintptr_t> g_ohfx_last_W{0};
using IdleFxStart_t = void (*)(uintptr_t unused, uintptr_t W, const float* offset, const float* rot);
// FUN_140301680(W, &matrix ptr, &local), hooked below (hk_muzzle_fx): its original, null until installed
using MuzzleFx_t = void (*)(uintptr_t W, uintptr_t* mptr, void* local);
MuzzleFx_t o_muzzle_fx = nullptr;
void offhand_torch_fx() {
    static int last_why = -1, last_path = 0;
    auto why = [&](int w) {
        g_ohfx_why.store(w, std::memory_order_relaxed);
        if (w != 0) {  // (the re-review) not asked now: the next ask is a new take (its "lit" and "placed" lines again)
            g_ohfx_last_W.store(0, std::memory_order_relaxed);
            last_path = 0;
            g_ohfx_path.store(0, std::memory_order_relaxed);
        }
        if (w == 4 && last_why != 4) {
            g_ohfx_nobj.fetch_add(1, std::memory_order_relaxed);
            log::info("[wield] the off-hand torch stays unlit: the torch is in no equip slot, so it has no weapon object (choosing the "
                      "torch on the weapon wheel may give it one: not measured)");
        } else if (w == 5 && last_why != 5) {
            log::info("[wield] the off-hand torch stays unlit: its weapon object's effect definition is not readable");
        } else if (w == 6 && last_why != 6) {
            log::info("[wield] the off-hand torch stays unlit: not available (anchors %d %d %d, follow site %d, HeldEffectsFollow %d, "
                      "the matrix hook %d, stood down %d)", anchors::addr(anchors::Id::IdleFxStart) ? 1 : 0,
                      anchors::addr(anchors::Id::IdleFxOffset) ? 1 : 0, anchors::addr(anchors::Id::IdleFxRot) ? 1 : 0, g_idlefx_ret ? 1 : 0,
                      g_fx_follow.load() ? 1 : 0, o_muzzle_fx ? 1 : 0, anchors::stand_down() ? 1 : 0);
        }
        last_why = w;
    };
    g_ohfx_W = 0;
    if (!offhand::torch_lit_enabled() || !offhand::enabled()) {
        why(1);
        return;
    }
    alignas(16) float P[16];
    if (!offhand::torch_pose(P)) {
        why(2);
        return;
    }
    const uintptr_t fn = anchors::addr(anchors::Id::IdleFxStart), off = anchors::addr(anchors::Id::IdleFxOffset),
                    rot = anchors::addr(anchors::Id::IdleFxRot);
    // (without the follow hook the flame would burn at the torch's holster: not asked)
    if (!fn || !off || !rot || !g_idlefx_ret || !o_muzzle_fx || !g_fx_follow.load(std::memory_order_relaxed) || anchors::stand_down()) {
        why(6);
        return;
    }
    for (float x : P)
        if (!std::isfinite(x)) {
            why(2);
            return;
        }
    const uintptr_t actor = player_actor();
    uintptr_t wmgr = 0, hand = 0, hand_W = 0;
    if (!actor || !rd(actor + 0x70, &wmgr) || !wmgr) {
        why(3);
        return;
    }
    if (rd(wmgr + 0x80, &hand) && hand) rd(hand + 0xa0, &hand_W);
    uintptr_t W = 0, info = 0;
    for (int s = 0; s < 8 && !W; ++s) {
        uintptr_t item = 0, w = 0, inf = 0;
        int16_t wt = -1;
        if (!rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) || !item || item == hand) continue;
        if (!rd(item + 0xa0, &w) || !w || w == hand_W || !rd(w + 0x28, &inf) || !inf || !rd(inf + 8, &wt) || wt != kTorchWeapon) continue;
        W = w;
        info = inf;
    }
    if (!W) {
        why(4);
        return;
    }
    // FUN_1406b8440's own reads: [info +0x3b8] and its +0x78 (both tested there, read here so a fault is ours, not the
    // game's), that def's vtable (its +0x20 called: the type name), the effect hash at def +0x50 (0: it returns)
    uintptr_t d0 = 0, def = 0, vt = 0, vf = 0;
    if (!rd(info + 0x3b8, &d0) || !d0 || !rd(d0 + 0x78, &def) || !def || !rd(def, &vt) || !vt || !rd(vt + 0x20, &vf) || !vf) {
        why(5);
        return;
    }
    std::memcpy(g_ohfx_P, P, sizeof(P));
    g_ohfx_W = W;
    if (g_ohfx_last_W.exchange(W, std::memory_order_relaxed) != W)
        log::info("[wield] the off-hand torch lit: its weapon object %p's flame asked for each tick, at the drawn torch", reinterpret_cast<void*>(W));
    // no __try (as gun_melee's ActorHit call): the effect manager takes its own lock (+0x3870), and a swallowed fault
    // could leave it held; everything it reads unchecked was checked above
    reinterpret_cast<IdleFxStart_t>(fn)(0, W, reinterpret_cast<const float*>(off), reinterpret_cast<const float*>(rot));
    g_ohfx_W = 0;
    g_ohfx_calls.fetch_add(1, std::memory_order_relaxed);
    why(0);
    const int path = g_ohfx_path.load(std::memory_order_relaxed);
    if (path && !last_path) {  // where its flame is placed, the take's first (later changes: g_ohfx_flips; which path runs is not measured)
        last_path = path;
        log::info("[wield] the off-hand torch's flame placed %s (%.2f m)", path == 1 ? "at the drawn torch's origin (W +0x80: no prop bone)"
                  : path == 2 ? "by the holstered torch's bone, carried onto the drawn torch"
                              : "at the drawn torch's origin (the bone over 1 m from W)", static_cast<double>(g_ohfx_bone_m.load(std::memory_order_relaxed)));
    }
}

// W one of the player's weapon objects: the item in hand's or an equip slot's (the game thread; guarded reads)
bool player_W(uintptr_t W) {
    const uintptr_t actor = player_actor();
    uintptr_t wmgr = 0, item = 0, w = 0;
    if (!W || !actor || !rd(actor + 0x70, &wmgr) || !wmgr) return false;
    if (rd(wmgr + 0x80, &item) && item && rd(item + 0xa0, &w) && w == W) return true;
    for (int s = 0; s < 8; ++s)
        if (rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item && rd(item + 0xa0, &w) && w == W) return true;
    return false;
}
// W's idle effect, from the table of the effects made keyed by a W (300 pairs of W and effect: FUN_1406b8440 fills a free
// one when it makes one, 0x1406b8655-0x1406b8696; its bounds read from those two lea's, the anchored function's own
// code), its own key (+0x140, set to W when made) W too: 0 none found
uintptr_t fx_entry(uintptr_t W) {
    const uintptr_t fn = anchors::addr(anchors::Id::IdleFxStart);
    uint8_t op[2][3] = {};
    int32_t disp[2] = {};
    if (!fn || !W || !raw(fn + 0x215, op[0], 3) || !raw(fn + 0x218, &disp[0], 4) || !raw(fn + 0x21c, op[1], 3) || !raw(fn + 0x21f, &disp[1], 4))
        return 0;
    if (op[0][0] != 0x48 || op[0][1] != 0x8d || op[0][2] != 0x05 || op[1][0] != 0x48 || op[1][1] != 0x8d || op[1][2] != 0x0d) return 0;
    const uintptr_t t0 = fn + 0x21c + static_cast<intptr_t>(disp[0]), t1 = fn + 0x223 + static_cast<intptr_t>(disp[1]);
    if (t1 <= t0 || t1 - t0 != 300 * 16) return 0;
    for (uintptr_t e = t0; e < t1; e += 16) {
        uintptr_t key = 0, fx = 0, own = 0;
        if (!rd(e, &key) || key != W) continue;
        if (rd(e + 8, &fx) && fx && rd(fx + 0x140, &own) && own == W) return fx;
    }
    return 0;
}
// ("dual fxwhere", read only) the matrix W's live idle effect follows (fx +0x170): 0 none found
uintptr_t fx_followed(uintptr_t W) {
    const uintptr_t fx = fx_entry(W);
    uintptr_t m = 0;
    return fx && rd(fx + 0x170, &m) ? m : 0;
}
std::atomic<uint64_t> g_idlefx_rebound{0};  // live effects found following another matrix, pointed at ours

void idle_fx(uintptr_t W, uintptr_t* mptr) {
    alignas(16) float P[16];
    // off: not placed (a flame already following a matrix of ours gets the game's own place, never frozen)
    // ([Hands] OffHandTorchLit) the off-hand torch's W, asked by offhand_torch_fx now: the drawn torch's pose (counted
    // apart: "calls" stays the game's own asks)
    const bool ohfx = W && W == g_ohfx_W;
    if (ohfx) {
        std::memcpy(P, g_ohfx_P, sizeof(P));
        g_ohfx_now.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_idlefx_calls.fetch_add(1, std::memory_order_relaxed);
    }
    const bool placed = g_fx_follow.load(std::memory_order_relaxed) && (ohfx || body::placed_item(W, P));
    FollowBuf* b = nullptr;
    for (FollowBuf& f : g_follow)
        if (f.W == W) b = &f;
    // (2026-11-03, round 78, the user: the molotov's and the torch's flame seen "moving along the holster animation") an item a
    // mod put-away let go (its draws hidden on the way to the holster, body.cpp): its flame follows the game's place 500 m
    // down, out of sight, until it is holstered
    if (!ohfx && b && body::leaving_W(W)) {
        // (cycle N46: the effect already holds our matrix as its source, so the game's own place is W +0x80's)
        alignas(16) float M[16];
        if (!raw(W + 0x80, M, sizeof(M))) return;
        M[13] -= 500.0f;
        bool ok = true;
        for (float x : M) ok = ok && std::isfinite(x);
        if (!ok) return;
        std::memcpy(b->m, M, sizeof(M));
        b->last = g_fx_tick;
        g_idlefx_leaving.fetch_add(1, std::memory_order_relaxed);
        wr<uintptr_t>(reinterpret_cast<uintptr_t>(mptr), reinterpret_cast<uintptr_t>(b->m));
        return;
    }
    // not the player's item placed at the hand: the game's own; but the player's own items from the first ask (above)
    const bool bind = !b && !placed && g_fx_follow.load(std::memory_order_relaxed) && g_fx_bind.load(std::memory_order_relaxed) && player_W(W);
    if (!b && !placed && !bind) return;
    uintptr_t src = 0;
    alignas(16) float M[16], G[16];
    if (!raw(reinterpret_cast<uintptr_t>(mptr), &src, sizeof(src)) || !src || !raw(src, M, sizeof(M))) return;
    if (!b) {  // a free slot, else the least recently called
        b = &g_follow[0];
        for (FollowBuf& f : g_follow)
            if (f.W == 0 ? b->W != 0 || f.last < b->last : b->W != 0 && f.last < b->last) b = &f;
        b->W = W;
        std::memcpy(b->m, M, sizeof(M));  // a fresh slot starts at the game's own place, never at another's
        b->take_ms = 0.0;  // (its first ask: a take's first, whatever the slot's last W asked)
        if (bind) g_idlefx_bound.fetch_add(1, std::memory_order_relaxed);
    }
    // (round 80) asked in the tick before too: its effect alive (kept so by the asks); else this take's first ask
    const bool live = b->last + 1 >= g_fx_tick && b->take_ms > 0.0;
    if (!live) {
        b->take_ms = log::now_ms();
        b->told = false;
    }
    b->src = src;
    b->last = g_fx_tick;
    b->ours = ohfx;
    if (placed && reinterpret_cast<uintptr_t>(b->m) != src && raw(W + 0x80, G, sizeof(G))) {
        // G^-1 for the rigid G (rows: its axes, then its position): R^T, and -t R^T (in double: two large terms)
        float Gi[16] = {G[0], G[4], G[8], 0, G[1], G[5], G[9], 0, G[2], G[6], G[10], 0, 0, 0, 0, 1};
        for (int c = 0; c < 3; ++c)
            Gi[12 + c] = static_cast<float>(-(static_cast<double>(G[12]) * Gi[c] + static_cast<double>(G[13]) * Gi[4 + c] +
                                              static_cast<double>(G[14]) * Gi[8 + c]));
        alignas(16) float out[16];
        if (src == W + 0x80) {
            std::memcpy(out, P, sizeof(out));  // M = G: the prop as placed
            if (ohfx) {
                g_ohfx_w80.fetch_add(1, std::memory_order_relaxed);
                if (const int was = g_ohfx_path.exchange(1, std::memory_order_relaxed); was && was != 1)
                    g_ohfx_flips.fetch_add(1, std::memory_order_relaxed);
                g_ohfx_bone_m.store(0.0f, std::memory_order_relaxed);
            }
        } else {
            mul44r(M, Gi, out);  // the bone in the hand's frame, then placed
            // (the off-hand torch: its W at the holster, its prop maybe elsewhere; a bone over 1 m from W is no bone of
            // this prop: the drawn torch itself)
            const float d2 = out[12] * out[12] + out[13] * out[13] + out[14] * out[14];
            if (ohfx) {
                (d2 <= 1.0f ? g_ohfx_bone : g_ohfx_far).fetch_add(1, std::memory_order_relaxed);
                const int now = d2 <= 1.0f ? 2 : 3;
                if (const int was = g_ohfx_path.exchange(now, std::memory_order_relaxed); was && was != now)
                    g_ohfx_flips.fetch_add(1, std::memory_order_relaxed);
                g_ohfx_bone_m.store(std::isfinite(d2) ? std::sqrt(d2) : -1.0f, std::memory_order_relaxed);
            }
            if (ohfx && !(d2 <= 1.0f)) std::memcpy(out, P, sizeof(out));
            else mul44r(out, P, out);
        }
        bool ok = true;
        for (float x : out) ok = ok && std::isfinite(x);
        if (ok && !ohfx) {  // ("dual fxwhere")
            auto dist = [](const float* a, const float* c) {
                const float dx = a[12] - c[12], dy = a[13] - c[13], dz = a[14] - c[14];
                return 1000.0f * std::sqrt(dx * dx + dy * dy + dz * dz);
            };
            g_fxw_ours_mm.store(dist(out, P), std::memory_order_relaxed);
            g_fxw_game_mm.store(dist(M, G), std::memory_order_relaxed);
            g_fxw_hand_mm.store(dist(P, G), std::memory_order_relaxed);
            g_fxw_src.store(src == W + 0x80 ? 1 : 2, std::memory_order_relaxed);
            g_fxw_W.store(W, std::memory_order_relaxed);
            uintptr_t info = 0;
            int16_t wt = -1;
            if (raw(W + 0x28, &info, sizeof(info)) && info && raw(info + 8, &wt, sizeof(wt))) g_fxw_weapon.store(wt, std::memory_order_relaxed);
        }
        std::memcpy(b->m, ok ? out : M, sizeof(out));  // (not finite: the game's own place this tick)
        (ok ? g_idlefx_moved : g_idlefx_bad).fetch_add(1, std::memory_order_relaxed);
    } else if (reinterpret_cast<uintptr_t>(b->m) != src) {
        std::memcpy(b->m, M, sizeof(M));  // not placed now (out of first person): the game's own place, followed
        g_idlefx_plain.fetch_add(1, std::memory_order_relaxed);
    }
    wr<uintptr_t>(reinterpret_cast<uintptr_t>(mptr), reinterpret_cast<uintptr_t>(b->m));  // the caller's local
    // (2026-11-04, round 80, the user: the torch's flame "always where it would be in johns normal animations", held too;
    // not seen in the simulator, whose first ask comes placed) the live effect of one held on (asked in the tick before
    // too, so not freed), made following another matrix (the game's own: made before it was ours), pointed at ours: its
    // own key W checked, the pointer one of ours (static: alive for the process). Once a take, 1.5 s in, a line: what
    // its effect follows
    if (!ohfx && live && g_fx_follow.load(std::memory_order_relaxed)) {
        const uintptr_t ours = reinterpret_cast<uintptr_t>(b->m);
        const uintptr_t fx = fx_entry(W);
        uintptr_t m = 0;
        bool was_other = false;
        if (fx && rd(fx + 0x170, &m) && m != ours) {
            was_other = true;
            if (wr<uintptr_t>(fx + 0x170, ours)) g_idlefx_rebound.fetch_add(1, std::memory_order_relaxed);
        }
        if (was_other || (!b->told && log::now_ms() - b->take_ms > 1500.0)) {
            b->told = true;
            uintptr_t info = 0;
            int16_t wt = -1;
            if (raw(W + 0x28, &info, sizeof(info)) && info) raw(info + 8, &wt, sizeof(wt));
            static uint64_t lines = 0;
            if (++lines <= 200)
                log::info("[wield] the held item's effect (weapon %d): %s (placed %s; moved %llu, plain %llu, rebound %llu)", static_cast<int>(wt),
                          !fx ? "its effect not found" : was_other ? (m == W + 0x80 ? "it followed the game's own matrix (W +0x80): pointed at ours"
                                                                                   : "it followed another matrix: pointed at ours")
                                                         : "its effect follows ours", placed ? "at the hand" : "not now",
                          static_cast<unsigned long long>(g_idlefx_moved.load()), static_cast<unsigned long long>(g_idlefx_plain.load()),
                          static_cast<unsigned long long>(g_idlefx_rebound.load()));
        }
    }
}

void idle_fx_tick() {  // the weapon tick, after the game's: held effects not asked for this tick follow the game's own place
    ++g_fx_tick;
    // ([Hands] OffHandTorchLit, the review) the off-hand torch's, no longer asked (let go, or the option off): its flame
    // fades where the drawn torch last was, not at the holster its src is
    for (FollowBuf& f : g_follow)
        if (f.W && f.src && !f.ours && f.last + 1 < g_fx_tick && !body::leaving_W(f.W)) {  // (round 78: a leaving item's kept out of sight)
            alignas(16) float m[16];
            bool ok = raw(f.src, m, sizeof(m));
            for (int i = 0; ok && i < 16; ++i) ok = std::isfinite(m[i]);
            if (ok) {
                std::memcpy(f.m, m, sizeof(m));
                g_idlefx_stale.fetch_add(1, std::memory_order_relaxed);
            }
        }
}

// (2026-11-04, GOAL-older-items item 28: the copy's damage as the gun it shows; research) [Debug] HitLog (off; installed
// at startup only with it on): every FUN_140ae1760(actor, DamageInfo*) call logged before the game's own, read only: its
// caller, the DamageInfo's weapon (+0xc4), damage (+0xcc), flags (+0xc8), attacker handle (+0xd4), its kind (+0) and
// +0xb8, the game time, and the copy's last shot then. Which caller is the bullet's and what it carries: not known yet
using ActorHit_t = void (*)(uintptr_t actor, uint8_t* info);
ActorHit_t o_actor_hit = nullptr;
std::atomic<bool> g_hitlog{false};
std::atomic<uint64_t> g_hitlog_n{0};
void hk_actor_hit(uintptr_t actor, uint8_t* info) {
    if (g_hitlog.load(std::memory_order_relaxed) && info) {
        int32_t kind = -1, wpn = -1, zone = -1;
        uint32_t flags = 0, att = 0;
        float dmg = -1.0f, gt = 0.0f, t90 = 0.0f;
        uintptr_t b8 = 0, b0 = 0;
        const uintptr_t ip = reinterpret_cast<uintptr_t>(info);
        rd(ip, &kind);
        rd(ip + 0x80, &zone);
        rd(ip + 0x90, &t90);
        rd(ip + 0xb0, &b0);
        rd(ip + 0xb8, &b8);
        rd(ip + 0xc4, &wpn);
        rd(ip + 0xc8, &flags);
        rd(ip + 0xcc, &dmg);
        rd(ip + 0xd4, &att);
        rd(anchors::addr(anchors::Id::GameTime), &gt);
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        g_hitlog_n.fetch_add(1, std::memory_order_relaxed);
        log::info("[hit] actor %p from RDR.exe+%#llx: kind %#x weapon %d damage %.3f flags %#x zone %d attacker 0x%x +0xb0 %p +0xb8 %p +0x90 %.3f | "
                  "game time %.3f, the copy's last shot %.3f", reinterpret_cast<void*>(actor),
                  static_cast<unsigned long long>(ret - anchors::base()), kind, wpn, static_cast<double>(dmg), flags, zone, att,
                  reinterpret_cast<void*>(b0), reinterpret_cast<void*>(b8), static_cast<double>(t90), static_cast<double>(gt),
                  static_cast<double>(g_copy_last));
    }
    o_actor_hit(actor, info);
}

// FUN_140301680(W, &matrix ptr, &local): the muzzle flash's followed matrix. For the copy's shot it is ours, kept on
// the copy (the prop's muzzle bone through T, every weapon tick while the flash lives)
void hk_muzzle_fx(uintptr_t W, uintptr_t* mptr, void* local) {
    o_muzzle_fx(W, mptr, local);
    if (g_idlefx_ret && W && mptr && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_idlefx_ret) {  // a held weapon's idle effect
        idle_fx(W, mptr);
        return;
    }
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
// (2026-10-27, DualWieldCrossReload's review) the copy's absolute spare writes, each only once the mod's queued spare
// changes have run and the game's own reload is quiet (then the inventory read now is the one the write replaces):
// end()'s give-back and begin()'s fill. After 3 s unsettled, made anyway (logged). Under g_mx
bool spare_ok() {
    RdrvrActorState st{};
    return api::actor_state(&st) && reload::spare_natives_done() && reload::reload_quiet(st);
}
void spare_writes() {
    bool any = g_fill_pending;
    for (float g : g_giveback) any = any || g > 0.0f;
    if (!any) return;
    if (g_fill_pending && g_want_slot.load() != kCopySlot) g_fill_pending = false;  // (the copy gone before its fill)
    const double now = log::now_ms();
    RdrvrActorState st{};
    const bool have = api::actor_state(&st) && st.valid && st.actor;
    // (the re-review) another player than the one queued for (a load, a new game): dropped; none valid for 10 s: dropped
    for (int t = 0; t < kGiveTypes; ++t) {
        if (g_giveback[t] <= 0.0f) continue;
        const bool other = have && (st.actor != g_giveback_actor[t] || st.object != g_giveback_obj[t]);
        if (other || (!have && now - g_giveback_ms[t] > 10000.0)) {
            log::warn("[wield] the copy's %.0f rounds (ammo type %d) not given back: %s", g_giveback[t], t, other ? "another player (a load)" : "no player for 10 s");
            g_giveback[t] = 0.0f;
        }
    }
    if (g_fill_pending && ((have && (st.actor != g_fill_actor || st.object != g_fill_obj)) || (!have && now - g_fill_ms > 10000.0))) {
        log::warn("[wield] the copy's fill dropped: %s", have ? "another player (a load)" : "no player for 10 s");
        g_fill_pending = false;
    }
    if (!have) return;
    // one absolute write a call: the next waits for it to have run (two from one reading: the second undid the first).
    // Always after the mod's queued spare changes have run; the game's reload quiet too, or 3 s waited for it (the
    // re-review: the 3 s force skips only that)
    if (!reload::spare_natives_done()) return;
    const bool quiet = reload::reload_quiet(st);
    for (int t = 0; t < kGiveTypes; ++t) {  // the old copy's rounds first, then a new copy's fill
        if (g_giveback[t] <= 0.0f) continue;
        if (!quiet && now - g_giveback_ms[t] <= 3000.0) return;
        const float sp = inv_spare(t);
        if (sp >= 0.0f) {
            if (sp < 9999.0f) set_spare(st.actor, t, sp + g_giveback[t]);
            log::info("[wield] the copy's %.0f round%s back to the spare (type %d, %.0f -> %.0f)%s", g_giveback[t], g_giveback[t] == 1.0f ? "" : "s", t, sp,
                      sp < 9999.0f ? sp + g_giveback[t] : sp, quiet ? "" : " (the game's reload not quiet after 3 s)");
            g_giveback[t] = 0.0f;
        } else if (now - g_giveback_ms[t] > 10000.0) {
            log::warn("[wield] the copy's %.0f rounds not given back: the inventory not read for 10 s", g_giveback[t]);
            g_giveback[t] = 0.0f;
        }
        return;
    }
    if (g_fill_pending && (quiet || now - g_fill_ms > 3000.0)) {
        const int type = sidearm_ammo(g_copy_weapon.load());
        const float sp = inv_spare(type), room = g_copy_max.load() - g_copy_clip.load();
        if (sp >= 0.0f && std::isfinite(room)) {
            const float n = std::floor(std::fmax(0.0f, std::fmin(sp, room)) + 0.01f);
            if (n > 0.0f && sp < 9999.0f) set_spare(st.actor, type, sp - n);
            g_copy_clip.fetch_add(n);
            log::info("[wield] the copy loaded: %.0f of %.0f from the spare rounds (%.0f left)%s", n, g_copy_max.load(), sp < 9999.0f ? sp - n : sp,
                      quiet ? "" : " (the game's reload not quiet after 3 s)");
            g_fill_pending = false;
        } else if (now - g_fill_ms > 10000.0) {
            log::warn("[wield] the copy not loaded: the inventory not read for 10 s");
            g_fill_pending = false;
        }
    }
}

void frame() {
    std::lock_guard lock(g_mx);
    static bool down = false;
    if (const float gv = g_carry_give.exchange(0.0f, std::memory_order_acquire); gv > 0.0f) {  // (R17) the carried gun's rounds
        const int t = g_carry_give_type.load(std::memory_order_relaxed);
        RdrvrActorState st{};
        if (t >= 0 && t < kGiveTypes && api::actor_state(&st) && st.valid && st.actor &&
            (g_giveback[t] <= 0.0f || (g_giveback_actor[t] == st.actor && g_giveback_obj[t] == st.object))) {
            g_giveback[t] += gv;
            g_giveback_ms[t] = log::now_ms();
            g_giveback_actor[t] = st.actor;
            g_giveback_obj[t] = st.object;
        } else if (t >= 0 && t < kGiveTypes) {
            g_carry_give.fetch_add(gv, std::memory_order_release);  // (the review: kept for a later frame, never dropped)
        }
    }
    spare_writes();
    const int ctrl = g_want_ctrl.load(std::memory_order_relaxed);
    if (ctrl < 0) {
        down = false;
        return;
    }
    const bool carry = g_carry.load(std::memory_order_relaxed);
    if (carry ? !g_carry_en.load(std::memory_order_relaxed)
              : g_want_slot.load(std::memory_order_relaxed) == kCopySlot ? !g_copy_en.load(std::memory_order_relaxed)
                                                                         : !g_enabled.load(std::memory_order_relaxed)) {
        end(carry ? "the carry turned off" : g_want_slot.load(std::memory_order_relaxed) == kCopySlot ? "the copy turned off" : "dual wield off");
        return;
    }
    const uint32_t gen = g_want_gen.load(std::memory_order_acquire);
    if (g_valid_gen.load(std::memory_order_acquire) == gen) {
        if (const int why = g_invalid.load(std::memory_order_relaxed)) {
            const bool returning = carry && g_carry_ret.load(std::memory_order_relaxed);
            // (the carry's first second: still in hand until the put-away is done; tick() keeps it, this too)
            if (carry && !returning && (why == 5 || why == 6) && log::now_ms() - g_carry_ms.load(std::memory_order_relaxed) < 1000.0) return;
            if (returning && (why == 5 || why == 6)) g_carry_takes.fetch_add(1, std::memory_order_relaxed);
            end(returning && (why == 5 || why == 6) ? "taken back by the gun hand" : why_text(why));
            return;
        }
    }
    if (carry && g_carry_ret.load(std::memory_order_relaxed) && log::now_ms() - g_carry_ret_ms.load(std::memory_order_relaxed) > 1500.0) {
        g_carry_ret.store(false, std::memory_order_relaxed);  // the draw did not happen: still carried
        log::info("[wield] the carried gun's take-back: not in hand after 1.5 s, still carried");
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
    if (carry) {  // carried only: its trigger stays the game's (controls.cpp), never a shot
        down = false;
        return;
    }
    const float t = whistle::suppressed(ctrl) ? 0.0f : hands::get(ctrl).trigger;  // run 8: the whistle's trigger fires nothing
    const bool was = down;
    down = t > 0.6f ? true : t < 0.4f ? false : was;
    const bool copy = g_want_slot.load(std::memory_order_relaxed) == kCopySlot;
    if (!down || was || menu::visible() || !(copy ? g_copy_W.load(std::memory_order_relaxed) : g_W.load(std::memory_order_relaxed))) return;
    // (2026-10-27, [Hands] DualWieldCrossReload) this hand holds rounds for the gun in hand (or still has the trigger it
    // held them with): no shot mid-reload; the pull is used up, so a fresh one fires after
    if (holster::cross_holds_fire(ctrl)) {
        log::info("[wield] the second gun's pull held back: its hand holds rounds for the other gun");
        return;
    }
    RdrvrActorState st{};
    if (api::actor_state(&st) && (st.weapon_flags & RDRVR_WEAPON_DEADEYE)) return;  // Dead Eye fires the gun in hand
    // (2026-10-28, [Hands] DualWieldOpen) opened by its stick for loading: the pull held (a buzz; the pull used up)
    if (actions::second_pull_held(ctrl)) {
        // (2026-10-31, r79 s1: the reason named "open" for a cock not made) open, or not cocked
        log::info("[wield] the %s's pull held: %s", copy ? "copy" : "second gun",
                  actions::second_open() ? "it is open (DualWieldOpen)" : "not cocked (a flick of its stick cocks it)");
        return;
    }
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

bool inv_entry(int type, float* count, float* cap) {
    // (2026-10-28, [Reload] PistolMagazines' review) as inv_spare reads it, with the entry's max (+4): a count over it, or
    // either not finite, refused. *count 9999 for an endless type
    if (type < 0 || type >= kAmmoTypes || !count || !cap) return false;
    const uintptr_t actor = player_actor();
    uintptr_t wmgr = 0, inv = 0;
    if (!actor || !rd(actor + 0x70, &wmgr) || !wmgr || !rd(wmgr + 0x418, &inv) || !inv) return false;
    float c = -1.0f, mx = -1.0f;
    uint8_t inf = 0;
    const uintptr_t e = inv + static_cast<uintptr_t>(type) * 12;
    if (!rd(e, &c) || !rd(e + 4, &mx) || !rd(e + 8, &inf) || !std::isfinite(c) || c < 0.0f) return false;
    if (inf) {
        *count = *cap = 9999.0f;
        return true;
    }
    if (!std::isfinite(mx) || mx < 0.0f || mx > 100000.0f || c > mx) return false;
    *count = c;
    *cap = mx;
    return true;
}

void init() {
    g_enabled = config::get_bool("Hands", "DualWield", false);
    log::info("[wield] dual wield %d: the free hand at another slot's holster takes that gun as a second", g_enabled.load() ? 1 : 0);
    g_copy_en = config::get_bool("Hands", "DualWieldCopy", false);
    g_own_model = config::get_bool("Hands", "DualWieldOwnModel", false);
    g_same_at_its = config::get_bool("Hands", "DualWieldSameAtItsHolster", true);
    g_copy_as_prop = config::get_bool("Hands", "CopyAsProp", true);
    g_copy_rate = config::get_bool("Hands", "CopyOwnRate", true);  // (on by default: the user, round 82)
    log::info("[wield] the copy's own fire rate %d: it waits the shown gun's ChamberTime after its own shots, the gun in hand its own",
              g_copy_rate.load() ? 1 : 0);
    log::info("[wield] the copy %d: the free hand at the other hip, a sidearm in hand, takes the same sidearm", g_copy_en.load() ? 1 : 0);
    g_carry_en = config::get_bool("Hands", "CarryByForegrip", true);
    log::info("[wield] carry by the foregrip %d: a long gun held two-handed, the gun hand's grip let go, stays in the front hand",
              g_carry_en.load() ? 1 : 0);
    g_cross = config::get_bool("Hands", "DualWieldCrossReload", false);
    log::info("[wield] cross reload %d: dual wielding, each hand takes rounds at the chest for the gun in the other hand", g_cross.load() ? 1 : 0);
    g_beside_torch = config::get_bool("Hands", "DualWieldBesideTorch", false);
    g_beside_items = config::get_bool("Hands", "DualWieldBesideItems", true);
    log::info("[wield] beside the torch %d: with DualWield, the torch (33) in hand takes a second gun as a gun in hand does", g_beside_torch.load() ? 1 : 0);
    d3d::add_frame_end_listener([](uint64_t) { frame(); });
}

bool install() {
    g_idlefx_ret = anchors::addr(anchors::Id::IdleFxMatrixRet);
    g_fx_follow = config::get_bool("Hands", "HeldEffectsFollow", true);
    log::info("[wield] the held weapons' idle effects (the fire bottle's, the torch's flame) follow the drawn item: %d (site %s)",
              g_fx_follow.load() ? 1 : 0, g_idlefx_ret ? "found" : "NOT FOUND: off");
    bool ok = hooks::install("RDR weapon tick (dual wield)", reinterpret_cast<void*>(anchors::addr(anchors::Id::WeaponTick)), hk_weapon_tick,
                             &o_weapon_tick);
    ok &= hooks::install("RDR muzzle flash matrix (the copy)", reinterpret_cast<void*>(anchors::addr(anchors::Id::MuzzleFxMatrix)), hk_muzzle_fx,
                         &o_muzzle_fx);
    if (config::get_bool("Debug", "HitLog", false) && anchors::addr(anchors::Id::ActorHit) && !anchors::stand_down()) {  // (research: not ok's)
        g_hitlog = hooks::install("RDR actor hit (the hit log)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ActorHit)), hk_actor_hit,
                                  &o_actor_hit);
        log::info("[wield] the hit log: %s", g_hitlog.load() ? "on (every actor hit logged)" : "NOT installed");
    }
    return ok;
}
bool copy_enabled() { return g_copy_en.load(); }
void quick_shot(int weapon) {
    g_quick_ms.store(log::now_ms(), std::memory_order_relaxed);
    g_quick_req.store(weapon, std::memory_order_release);
}
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
std::atomic<bool> g_copy_wt{false};  // ("dual copywt on|off": the test's)
std::atomic<uint64_t> g_copy_wt_n{0};
int copy_shot_weapon(int wtype) {
    const int m = copy_model();
    if (!g_copy_wt.load(std::memory_order_relaxed) || m < 0 || m > 7 || m == wtype) return wtype;
    g_copy_wt_n.fetch_add(1, std::memory_order_relaxed);
    return m;
}
bool copy_as_prop() { return g_copy_as_prop.load(std::memory_order_relaxed); }
void set_copy_as_prop(bool on, bool save) {
    if (g_copy_as_prop.exchange(on) != on) log::info("[wield] the copy of the same model shown as a prop of that model: %s", on ? "on" : "off");
    if (save) config::set("Hands", "CopyAsProp", on ? "1" : "0");
}
const char* sidearm_fragment(int weapon) {
    static const char* const kFrag[8] = {"pistol_volcanic01x", "pistol_semiauto01x", "pistol_highpower01x", "pistol_mauser01x",
                                         "revolver_cattleman01x", "revolver_schofield01x", "revolver_doubleaction01x", "revolver_lemat01x"};
    if (weapon == 15) return "shotgun_sawed01x";  // (P2: the Sawed-off's copy as its own prop; fragments.rpf)
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
    (void)weapon;  // (the copy's own weapon; 2026-10-27: the spare read from the inventory when settled, not the caller's)
    (void)spare;
    std::lock_guard lock(g_mx);
    if (g_want_slot.load() != kCopySlot || g_copy_clip.load() + 0.5f > g_copy_max.load()) return false;
    const int in = load_copy_rounds(1, actor);
    if (in > 0) log::info("[wield] the copy squeezed at the chest: one round (clip %.0f of %.0f)", g_copy_clip.load(), g_copy_max.load());
    return in > 0;  // (-1, the spare unsettled for a tick: the press refused, as a full gun's)
}

int load_copy_rounds(int n, int32_t actor) {
    std::lock_guard lock(g_mx);
    // (2026-10-27, DualWieldCrossReload) the copy's own count, n at once, from the gun in hand's ammo type's spare (the copy
    // is that gun: g_copy_weapon), ACTOR_SET_INV_AMMO to spare - m; the gun in hand's own clip (W +0x12c, shared with the
    // copy) untouched. The review: the spare is the game's inventory read now, and the write waits (-1) until the mod's
    // queued spare changes have run and the game's own reload is quiet, so it replaces exactly what it read
    if (n < 1 || !actor || g_want_slot.load() != kCopySlot || !g_copy_en.load() || !g_copy_W.load(std::memory_order_relaxed)) return 0;
    if (g_fill_pending) return -1;
    const int w = g_copy_weapon.load(), type = sidearm_ammo(w);
    if (type < 0) return 0;
    if (!spare_ok()) return -1;
    const float spare = inv_spare(type), room = g_copy_max.load() - g_copy_clip.load();
    if (spare < 0.0f || !std::isfinite(room)) return 0;
    const float m = std::floor(std::fmin(std::fmin(static_cast<float>(n), room), spare) + 0.01f);
    if (!(m >= 1.0f) || m > 50.0f) return 0;
    if (spare < 9999.0f) set_spare(actor, type, spare - m);
    g_copy_clip.fetch_add(m);
    g_copy_loads.fetch_add(1, std::memory_order_relaxed);
    g_cross_loads.fetch_add(1, std::memory_order_relaxed);
    g_cross_rounds.fetch_add(static_cast<uint64_t>(m), std::memory_order_relaxed);
    log::info("[wield] %.0f round%s into the copy (clip %.0f of %.0f, spare %.0f -> %.0f)", m, m == 1.0f ? "" : "s", g_copy_clip.load(), g_copy_max.load(),
              spare, spare < 9999.0f ? spare - m : spare);
    return static_cast<int>(m);
}

// (2026-11-01, GOAL-offhand-changes C7) the copy's magazine dropped: its count to the chambered round, the rest back to the
// gun in hand's ammo type's spare (as load_copy_rounds takes it, the other way); -1 the spare not settled (try again), else
// the rounds given back
int drop_copy_mag(int32_t actor) {
    std::lock_guard lock(g_mx);
    if (!actor || g_want_slot.load() != kCopySlot || !g_copy_en.load() || !g_copy_W.load(std::memory_order_relaxed)) return 0;
    if (g_fill_pending) return -1;
    const int w = g_copy_weapon.load(), type = sidearm_ammo(w);
    if (type < 0) return 0;
    if (!spare_ok()) return -1;
    const float c = std::floor(g_copy_clip.load() + 0.01f), keep = c >= 1.0f ? 1.0f : 0.0f, m = c - keep;
    const float spare = inv_spare(type);
    if (!(m >= 1.0f) || spare < 0.0f) return 0;
    if (spare < 9999.0f) set_spare(actor, type, spare + m);
    g_copy_clip.store(keep);
    log::info("[wield] the copy's magazine dropped: %.0f rounds back to the spare (%.0f -> %.0f), one chambered %d", m, spare,
              spare < 9999.0f ? spare + m : spare, keep >= 1.0f ? 1 : 0);
    return static_cast<int>(m);
}

bool cross_reload() { return g_cross.load(std::memory_order_relaxed); }
void set_cross_reload(bool on, bool save) {
    if (g_cross.exchange(on) != on) log::info("[wield] cross reload (each hand loads the other gun): %s", on ? "on" : "off");
    if (save) config::set("Hands", "DualWieldCrossReload", on ? "1" : "0");
}
bool beside_torch_enabled() { return g_beside_torch.load(std::memory_order_relaxed); }
void set_beside_torch(bool on, bool save) {
    if (g_beside_torch.exchange(on) != on) log::info("[wield] beside the torch (a second gun with the torch in hand): %s", on ? "on" : "off");
    if (save) config::set("Hands", "DualWieldBesideTorch", on ? "1" : "0");
    if (on) return;
    // turned off with the torch in hand and a second gun out (not the copy, not a carry): put back, as DualWield off does
    RdrvrActorState st{};
    std::lock_guard lock(g_mx);
    if (g_want_ctrl.load() >= 0 && g_want_slot.load() != kCopySlot && !g_carry.load() && api::actor_state(&st) && st.weapon == kTorchWeapon)
        end("beside the torch turned off");
}
bool beside_torch(int weapon) {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (weapon == kTorchWeapon) return g_beside_torch.load(std::memory_order_relaxed);
    // (step 6) the knife and the throwables (offhand's list: the fire bottle, dynamite, the throwing knife, the tomahawk,
    // the Undead Nightmare's holy water and baits)
    const bool item = weapon == 22 || weapon == 23 || weapon == 24 || weapon == 25 || weapon == 29 || weapon == 35 || weapon == 36 || weapon == 37;
    return item && g_beside_items.load(std::memory_order_relaxed);
}
bool second_beside(int weapon) { return reload::is_gun(weapon) || beside_torch(weapon); }
int load_second(int n, int32_t actor) {
    std::lock_guard lock(g_mx);
    // (2026-10-27, DualWieldCrossReload) the dual-wielded second gun only (not the copy: its own count; not a carry), as
    // the game thread last found it this generation
    // (2026-10-31, the off-hand build step 3, R17) and the carried gun (CarryByForegrip: not DualWield's option)
    const bool carried = g_carry.load();
    if (n < 1 || !actor || g_want_slot.load() == kCopySlot || g_want_slot.load() < 0 || (!carried && !g_enabled.load())) return 0;
    const State s = state();
    if (!s.ready || s.weapon < 0 || !std::isfinite(s.clip) || !std::isfinite(s.clip_max) || !std::isfinite(s.spare) || s.clip < 0.0f ||
        s.clip_max < 1.0f || s.spare < 0.0f)
        return 0;
    // (the review) the last load's rounds counted until the game thread's reading is newer than the tick it ran on (not a
    // fixed 250 ms: its clip shows them a tick later, and a short RoundInsertMs refused the last rounds)
    float p = 0.0f;
    if (g_sec_id) {
        if (!g_sec_ran) {
            RdrvrNativeResult r{};
            uint64_t t = 0;  // (its result, else the api's mark of the posted ones: reload's tracker may have read it)
            if (api::wait_native(g_sec_id, &r, 0)) g_sec_ran = r.tick ? r.tick : g_sec_q + 1;
            else if (api::result_posted(g_sec_id, &t)) g_sec_ran = t ? t : g_sec_q + 1;
            else if (api::script_ticks() > g_sec_q + 60) g_sec_ran = g_sec_q + 60;
        }
        if (g_sec_ran && g_clip_tick.load(std::memory_order_relaxed) > g_sec_ran) g_sec_id = 0, g_sec_m = 0.0f;
        else p = g_sec_m;
    }
    const float m = std::floor(std::fmin(std::fmin(static_cast<float>(n), s.clip_max - (s.clip + p)), s.spare - p) + 0.01f);
    if (!(m >= 1.0f) || m > 200.0f) return 0;
    // ACTOR_ADD_WEAPON_AMMO(actor, its eWeapon, m) (handler 0x1403be870: the item by the weapon's name, its W; then
    // FUN_140300f40 adds m from the inventory, clamped): the same native and arguments as its chest squeeze; the count a
    // float read with movss. Relative: never more than the inventory holds
    const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(actor)), static_cast<uint64_t>(static_cast<uint32_t>(s.weapon)), fbits(m)};
    g_sec_id = api::queue_native(0xCC69DCC1, args, 3, 0, nullptr);  // ACTOR_ADD_WEAPON_AMMO
    if (carried) {  // (R17: this rise is ours, no more than m, within 90 script ticks of its run)
        g_carry_adopt_n.store(g_carry_adopt_n.load(std::memory_order_relaxed) + m, std::memory_order_relaxed);
        g_carry_adopt_tick.store(api::script_ticks() + 90, std::memory_order_relaxed);
    }
    reload::note_spare_native(g_sec_id);  // (the re-review: a spare change as any other)
    g_sec_q = api::script_ticks();
    g_sec_ran = 0;
    g_sec_m = p + m;
    g_cross_loads.fetch_add(1, std::memory_order_relaxed);
    g_cross_rounds.fetch_add(static_cast<uint64_t>(m), std::memory_order_relaxed);
    log::info("[wield] %.0f round%s into the %s by the other hand (weapon %d, clip %.0f -> %.0f of %.0f, spare %.0f)", m, m == 1.0f ? "" : "s",
              carried ? "carried gun" : "second gun", s.weapon, s.clip + p, s.clip + p + m, s.clip_max, s.spare - p);
    return static_cast<int>(m);
}

bool enabled() { return g_enabled.load(); }
void set_enabled_session(bool on) {
    std::lock_guard lock(g_mx);
    if (g_enabled.exchange(on) != on) log::info("[wield] dual wield %d", on ? 1 : 0);
    if (!on && g_want_slot.load() != kCopySlot && !g_carry.load()) end("dual wield turned off");  // a copy and a carry keep their own switches
}
void set_enabled(bool on) {
    set_enabled_session(on);
    config::set("Hands", "DualWield", on ? "1" : "0");
}

void begin(int ctrl, int john, int slot, const char* zone) {
    std::lock_guard lock(g_mx);
    const bool copy = slot == kCopySlot;
    if (copy ? !g_copy_en.load() : !g_enabled.load()) return;
    if (g_carry.load()) return;  // the front hand carries a long gun
    if (ctrl < 0 || ctrl > 1 || john < 0 || john > 1 || (!copy && (slot < 0 || slot > 7))) return;
    if (copy) {  // its own clip, loaded from the spare rounds (as many as the gun in hand holds)
        RdrvrActorState st{};
        if (!api::actor_state(&st) || sidearm_ammo(st.weapon) < 0 || !std::isfinite(st.clip_max)) return;
        // (2026-10-27, DualWieldCrossReload's review) filled by spare_writes, from the inventory once the spare is settled
        // (at once when it is: below, after the copy is wanted)
        g_copy_clip.store(0.0f);
        g_copy_max.store(st.clip_max);
        g_copy_weapon.store(st.weapon);
        g_copy_T_ok = false;
        g_fill_pending = true;
        g_fill_ms = log::now_ms();
        g_fill_actor = st.actor;
        g_fill_obj = st.object;
    }
    g_sec_id = 0, g_sec_m = 0.0f;  // (the last second gun's pending rounds: not this one's)
    g_want_slot.store(slot, std::memory_order_relaxed);
    g_want_john.store(john, std::memory_order_relaxed);
    g_want_ctrl.store(ctrl, std::memory_order_relaxed);
    std::snprintf(g_zone, sizeof(g_zone), "%s", zone ? zone : "?");
    g_want_gen.fetch_add(1, std::memory_order_release);
    g_begins.fetch_add(1, std::memory_order_relaxed);
    if (copy) {
        log::info("[wield] %s hand at the %s: a copy of the sidearm in hand as the second (John's %s hand)", ctrl ? "right" : "left", g_zone,
                  john ? "right" : "left");
        spare_writes();  // its fill now if the spare is settled
    }
    else
        log::info("[wield] %s hand at the %s: the gun of slot %d as the second (John's %s hand)", ctrl ? "right" : "left", g_zone, slot,
                  john ? "right" : "left");
}

void end(const char* why) {
    std::lock_guard lock(g_mx);
    if (g_want_ctrl.load() < 0) return;
    const int ctrl = g_want_ctrl.load(), slot = g_want_slot.load();
    if (slot == kCopySlot) {  // the copy's rounds back to the spare
        // (2026-10-27, DualWieldCrossReload's review) by spare_writes, from the inventory once settled (at once when it is):
        // the actor state's spare was a tick old, and of whatever gun is in hand now (the copy ends as it is put away)
        const float left = g_copy_clip.exchange(0.0f);
        g_fill_pending = false;
        const int t = sidearm_ammo(g_copy_weapon.load());
        RdrvrActorState st{};
        if (left > 0.0f && std::isfinite(left) && t >= 0 && t < kGiveTypes) {
            if (!api::actor_state(&st) || !st.valid || !st.actor) {
                log::warn("[wield] the copy's %.0f rounds not given back: no player state (a load)", left);
            } else {
                if (g_giveback[t] > 0.0f && (g_giveback_actor[t] != st.actor || g_giveback_obj[t] != st.object)) g_giveback[t] = 0.0f;
                g_giveback[t] += left;
                g_giveback_ms[t] = log::now_ms();
                g_giveback_actor[t] = st.actor;
                g_giveback_obj[t] = st.object;
                spare_writes();  // at once if the spare is settled
            }
        }
    }
    // (the review: the carry's W cleared first, then the controller, then the carry itself, so the game thread's place_mode
    // never sees the carried W as a dual-wielded second gun in between and snaps it to the front hand's grip)
    const bool carry = g_carry.load();
    if (carry) {  // the carried gun at its holster from the next update (not placed by its grip in the meantime)
        g_W.store(0, std::memory_order_relaxed);
        g_item.store(0, std::memory_order_relaxed);
        g_john.store(-1, std::memory_order_relaxed);
    }
    g_want_ctrl.store(-1, std::memory_order_relaxed);
    g_want_slot.store(-1, std::memory_order_relaxed);
    g_want_john.store(-1, std::memory_order_relaxed);
    g_carry.store(false, std::memory_order_release);
    g_carry_ret.store(false);
    g_want_gen.fetch_add(1, std::memory_order_release);
    g_ends.fetch_add(1, std::memory_order_relaxed);
    if (const int fs = g_forced.exchange(-1); fs >= 0) {  // the slot's force flag cleared again (the gun stays shown on its holster)
        RdrvrActorState st{};
        if (api::actor_state(&st) && st.actor) {
            const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(fs), 0ull};
            api::queue_native(0x1511D111, args, 3, 0, nullptr);
        }
    }
    log::info("[wield] the %s (slot %d, the %s hand) put back: %s", carry ? "carried gun" : "second gun", slot, ctrl ? "right" : "left", why ? why : "?");
}

bool carry_enabled() { return g_carry_en.load(); }
void set_carry_enabled(bool on, bool save) {
    std::lock_guard lock(g_mx);
    if (g_carry_en.exchange(on) != on) log::info("[wield] carry by the foregrip %d", on ? 1 : 0);
    if (!on && g_carry.load()) end("the carry turned off");
    if (save) config::set("Hands", "CarryByForegrip", on ? "1" : "0");
}

void begin_carry(int ctrl, int john, int slot, uintptr_t item, uintptr_t W, const float P[16], const float hand0[3], const float rot0[9]) {
    std::lock_guard lock(g_mx);
    if (!g_carry_en.load() || g_want_ctrl.load() >= 0) return;  // (a second gun or a copy out: none)
    if (ctrl < 0 || ctrl > 1 || john < 0 || john > 1 || slot < 4 || slot > 6 || !item || !W || !P || !hand0 || !rot0) return;
    g_sec_id = 0, g_sec_m = 0.0f;  // (R17, the review: the last gun's pending cross rounds are not this one's)
    // (r289: every hand-over of a session refused: the rows' fourth column is the game's own data, never used here and
    // not finite in some sessions, as capture_two_hand found in cycle r197) the rows' xyz only
    for (int i = 0; i < 16; ++i)
        if ((i & 3) != 3 && !std::isfinite(P[i])) return;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(hand0[k])) return;
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(rot0[i])) return;
    uintptr_t info = 0;
    int16_t wt = -1;
    if (!rd(W + 0x28, &info) || !info || !rd(info + 8, &wt)) wt = -1;
    g_carry_T_made.store(false, std::memory_order_release);  // (no carry is out: nothing reads T now)
    g_carry_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    std::memcpy(g_carry_P, P, sizeof(g_carry_P));
    std::memcpy(g_carry_hand0, hand0, sizeof(g_carry_hand0));
    std::memcpy(g_carry_rot0, rot0, sizeof(g_carry_rot0));
    g_carry_seq.fetch_add(1, std::memory_order_release);
    g_carry_weapon.store(wt);
    g_carry_ms.store(log::now_ms());
    g_carry_ret.store(false);
    {  // (R17, the review) the count it keeps: its clip at the hand-over; no load of a former carry adopted
        float c0 = -1.0f;
        g_carry_clip0.store(rd(W + 0x12c, &c0) && std::isfinite(c0) && c0 >= 0.0f ? c0 : -1.0f, std::memory_order_relaxed);
        g_carry_adopt_n.store(0.0f, std::memory_order_relaxed);
        g_carry_adopt_tick.store(0, std::memory_order_relaxed);
    }
    g_carries.fetch_add(1, std::memory_order_release);  // (before the carry: the game thread's carry_serial sees a new carry's number with it)
    g_carry.store(true, std::memory_order_release);
    g_want_slot.store(slot, std::memory_order_relaxed);
    g_want_john.store(john, std::memory_order_relaxed);
    g_want_ctrl.store(ctrl, std::memory_order_relaxed);
    std::snprintf(g_zone, sizeof(g_zone), "foregrip");
    // the game thread's view at once: the gun's first holstered update is placed at the hand (tick() confirms it then)
    g_item.store(item, std::memory_order_relaxed);
    g_W.store(W, std::memory_order_relaxed);
    g_john.store(john, std::memory_order_relaxed);
    g_weapon.store(wt, std::memory_order_relaxed);
    g_want_gen.fetch_add(1, std::memory_order_release);
    g_begins.fetch_add(1, std::memory_order_relaxed);
    log::info("[wield] %s hand carries the long gun of slot %d (weapon %d) by its foregrip (John's %s hand): drawn at (%.3f %.3f %.3f)",
              ctrl ? "right" : "left", slot, wt, john ? "right" : "left", P[12], P[13], P[14]);
}

bool carry_T(float T[16]) {
    if (!g_carry.load(std::memory_order_acquire) || !g_carry_T_made.load(std::memory_order_acquire)) return false;
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_carry_tseq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        std::memcpy(T, g_carry_T, sizeof(g_carry_T));
        if (g_carry_tseq.load(std::memory_order_acquire) == s0) return true;
    }
    return false;
}

bool carry_start(float P[16], float hand0[3], float rot0[9]) {
    if (!g_carry.load(std::memory_order_acquire)) return false;
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_carry_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        std::memcpy(P, g_carry_P, sizeof(g_carry_P));
        std::memcpy(hand0, g_carry_hand0, sizeof(g_carry_hand0));
        std::memcpy(rot0, g_carry_rot0, sizeof(g_carry_rot0));
        if (g_carry_seq.load(std::memory_order_acquire) == s0) return true;
    }
    return false;
}
uint64_t carry_serial() { return g_carries.load(std::memory_order_acquire); }

void carry_T_set(const float T[16]) {
    if (!g_carry.load(std::memory_order_acquire) || g_carry_T_made.load(std::memory_order_acquire)) return;
    g_carry_tseq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing (the game thread its only writer)
    std::memcpy(g_carry_T, T, sizeof(g_carry_T));
    g_carry_tseq.fetch_add(1, std::memory_order_release);
    g_carry_T_made.store(true, std::memory_order_release);
    log::info("[wield] the carried gun's place on the hand made, its front bone settled: T (%.3f %.3f %.3f)", T[12], T[13], T[14]);
}

void carry_return() {
    std::lock_guard lock(g_mx);
    if (!g_carry.load() || g_carry_ret.load()) return;
    g_carry_ret_ms.store(log::now_ms());
    g_carry_ret.store(true);
    log::info("[wield] the carried gun taken back by the gun hand: drawn, carried until it is in hand");
}
bool carry_returning() { return g_carry.load() && g_carry_ret.load(); }

State state() {
    State s;
    s.ctrl = g_want_ctrl.load(std::memory_order_relaxed);
    s.on = s.ctrl >= 0;
    if (!s.on) return s;
    s.john = g_want_john.load(std::memory_order_relaxed);
    s.slot = g_want_slot.load(std::memory_order_relaxed);
    s.copy = s.slot == kCopySlot;
    s.carry = !s.copy && g_carry.load(std::memory_order_acquire);
    if (s.carry) {
        s.carry_weapon = g_carry_weapon.load(std::memory_order_relaxed);
        s.carry_T_ok = g_carry_T_made.load(std::memory_order_acquire);
        s.carry_ms = g_carry_ms.load(std::memory_order_relaxed);
    }
    s.ready = (s.copy ? g_copy_W.load(std::memory_order_relaxed) : g_W.load(std::memory_order_relaxed)) != 0 &&
              g_valid_gen.load(std::memory_order_acquire) == g_want_gen.load(std::memory_order_acquire);
    if (s.copy) {
        s.weapon = g_copy_weapon.load(std::memory_order_relaxed);
        s.clip = g_copy_clip.load(std::memory_order_relaxed);
        s.copy_max = g_copy_max.load(std::memory_order_relaxed);
        s.clip_max = s.copy_max;  // (2026-10-27, DualWieldCrossReload; its spare is the gun in hand's: the actor state's)
    } else if (s.ready) {
        s.weapon = g_weapon.load(std::memory_order_relaxed);
        s.clip = g_clip.load(std::memory_order_relaxed);
        s.clip_max = g_clip_max.load(std::memory_order_relaxed);  // (2026-10-27, DualWieldCrossReload)
        s.spare = g_spare.load(std::memory_order_relaxed);
    }
    return s;
}

int copy_john() { return g_want_john.load(std::memory_order_relaxed); }
uint64_t shots_fired(int* weapon) {
    const uint64_t n = g_shots.load(std::memory_order_acquire) + g_copy_shots.load(std::memory_order_acquire);
    if (weapon) *weapon = g_shot_weapon.load(std::memory_order_relaxed);
    return n;
}

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
// (a carried gun is never fired as the second: in its first second g_W is still the gun in hand, whose shots are its own)
bool is_secondary_W(uintptr_t W) { return W && !g_carry.load(std::memory_order_relaxed) && W == g_W.load(std::memory_order_relaxed); }
int place_mode(float T[16]) {
    if (g_carry.load(std::memory_order_acquire)) {
        if (g_carry_T_made.load(std::memory_order_acquire)) return carry_T(T) ? 2 : 3;
        return 4;  // T to make (carry_start)
    }
    return g_want_ctrl.load(std::memory_order_relaxed) >= 0 ? 1 : 0;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    bool copy_status = false;
    while (in >> w) {
        if (w == "on" || w == "off") {
            set_enabled_session(w == "on");
        } else if (w == "carry") {  // dual carry on|off: [Hands] CarryByForegrip for the session
            std::string v;
            in >> v;
            if (v == "on" || v == "off") set_carry_enabled(v == "on", false);
        } else if (w == "begin") {  // dual begin <slot> [left|right]: the free hand (the other than the gun's) takes that slot's gun
            int slot = -1;
            std::string side;
            in >> slot >> side;
            const int ctrl = side == "left" ? 0 : side == "right" ? 1 : 0;
            begin(ctrl, ctrl, slot, "test");
        } else if (w == "quick") {  // dual quick: the hand-cocked shots let through (their counters, the last ChamberTime)
            char b[200];
            std::snprintf(b, sizeof(b), "quick: done %llu, not needed %llu, refused %llu; chamber %.3f s, clip phase %.2f",
                          static_cast<unsigned long long>(g_quick_done.load()), static_cast<unsigned long long>(g_quick_noneed.load()),
                          static_cast<unsigned long long>(g_quick_refused.load()), g_quick_chamber.load(), g_quick_phase.load());
            return b;
        } else if (w == "idlefx") {  // dual idlefx [on|off]: [Hands] HeldEffectsFollow for the session; its counters
            std::string v;
            if (in >> v) {
                std::string x;
                if (v == "bind") {
                    if (in >> x) g_fx_bind = x == "on";  // dual idlefx bind on|off (the test's; alone: the counters)
                } else if (v == "forget") {  // (the test's, its effect gone) the last placed W's buffer freed: its next ask a first
                    const uintptr_t fw = g_fxw_W.load();
                    for (FollowBuf& f : g_follow)
                        if (fw && f.W == fw) {
                            f.src = 0;
                            f.W = 0;
                        }
                } else {
                    g_fx_follow = v == "on";
                }
            }
            // ("calls": the game's own asks; "asked": offhand_torch_fx's; "followed": those idle_fx placed, by W +0x80, a
            // bone within 1 m or over it (the last's distance); "last": g_ohfx_why's code; "avail": its anchors, the
            // follow site and the matrix hook all there. Under 512 characters: the test channel's answer is cut near it)
            char b[512];
            std::snprintf(b, sizeof(b), "idlefx %d: calls %llu, moved %llu, plain %llu, stale %llu, bad %llu, leaving %llu, site %s | off-hand torch lit %d avail %d: "
                          "asked %llu, followed %llu (W+0x80 %llu, bone %llu, far %llu, %.2f m, flips %llu), no weapon object %llu, last %d, W %p",
                          g_fx_follow.load() ? 1 : 0, static_cast<unsigned long long>(g_idlefx_calls.load()),
                          static_cast<unsigned long long>(g_idlefx_moved.load()), static_cast<unsigned long long>(g_idlefx_plain.load()),
                          static_cast<unsigned long long>(g_idlefx_stale.load()), static_cast<unsigned long long>(g_idlefx_bad.load()),
                          static_cast<unsigned long long>(g_idlefx_leaving.load()), g_idlefx_ret ? "found" : "not found", offhand::torch_lit_enabled() ? 1 : 0,
                          anchors::addr(anchors::Id::IdleFxStart) && anchors::addr(anchors::Id::IdleFxOffset) && anchors::addr(anchors::Id::IdleFxRot) &&
                                  g_idlefx_ret && o_muzzle_fx
                              ? 1
                              : 0,
                          static_cast<unsigned long long>(g_ohfx_calls.load()), static_cast<unsigned long long>(g_ohfx_now.load()),
                          static_cast<unsigned long long>(g_ohfx_w80.load()), static_cast<unsigned long long>(g_ohfx_bone.load()),
                          static_cast<unsigned long long>(g_ohfx_far.load()), static_cast<double>(g_ohfx_bone_m.load()), static_cast<unsigned long long>(g_ohfx_flips.load()),
                          static_cast<unsigned long long>(g_ohfx_nobj.load()), g_ohfx_why.load(), reinterpret_cast<void*>(g_ohfx_last_W.load()));
            return b;
        } else if (w == "fxwhere") {  // dual fxwhere: the last held effect placed (g_fxw_*), the matrix its live effect follows
            const uintptr_t fw = g_fxw_W.load();
            const uintptr_t m = fx_followed(fw);
            const char* fol = "none found";
            if (m && fw && m == fw + 0x80) {
                fol = "the game's (W+0x80)";
            } else if (m) {
                fol = "another";
                for (const FollowBuf& f : g_follow)
                    if (m == reinterpret_cast<uintptr_t>(f.m)) fol = f.W == fw ? "ours" : "ours, another W's";
            }
            char b[300];
            std::snprintf(b, sizeof(b), "fxwhere: weapon %d, its effect %.0f mm from the drawn item's origin, the game's %.0f mm from W+0x80's, "
                          "the drawn item %.0f mm from W+0x80, followed %s; the live effect follows %s; bound %llu, rebound %llu", static_cast<int>(g_fxw_weapon.load()),
                          static_cast<double>(g_fxw_ours_mm.load()), static_cast<double>(g_fxw_game_mm.load()),
                          static_cast<double>(g_fxw_hand_mm.load()), g_fxw_src.load() == 1 ? "W+0x80" : g_fxw_src.load() == 2 ? "a bone" : "none",
                          fol, static_cast<unsigned long long>(g_idlefx_bound.load()), static_cast<unsigned long long>(g_idlefx_rebound.load()));
            return b;
        } else if (w == "copywt") {  // dual copywt [on|off]: the copy's projectile launched as the shown gun's weapon type (research)
            std::string v;
            if (in >> v) g_copy_wt = v == "on";
            char b[100];
            std::snprintf(b, sizeof(b), "copywt %d: launches %llu", g_copy_wt.load() ? 1 : 0, static_cast<unsigned long long>(g_copy_wt_n.load()));
            return b;
        } else if (w == "hitlog") {  // dual hitlog [on|off]: [Debug] HitLog's lines (its hook installed at startup only)
            std::string v;
            if (in >> v && o_actor_hit) g_hitlog = v == "on";
            char b[120];
            std::snprintf(b, sizeof(b), "hitlog %d (installed %d): hits %llu", g_hitlog.load() ? 1 : 0, o_actor_hit ? 1 : 0,
                          static_cast<unsigned long long>(g_hitlog_n.load()));
            return b;
        } else if (w == "copyrate") {  // dual copyrate [on|off]: [Hands] CopyOwnRate for the session; its counters
            std::string v;
            if (in >> v) g_copy_rate = v == "on";
            char b[260];
            std::snprintf(b, sizeof(b), "copyrate %d: shots %llu, held by its own interval %llu; its interval %.3f s (weapon %d's), the gun in hand's %.3f s; "
                          "copy shots %llu, refused %llu; held a tick %llu", g_copy_rate.load() ? 1 : 0, static_cast<unsigned long long>(g_copy_own_shots.load()),
                          static_cast<unsigned long long>(g_copy_waits.load()), static_cast<double>(g_copy_iv.load()), g_copy_iv_of.load(),
                          static_cast<double>(g_copy_hand_iv.load()), static_cast<unsigned long long>(g_copy_shots.load()),
                          static_cast<unsigned long long>(g_refused.load()), static_cast<unsigned long long>(g_copy_same_tick.load()));
            return b;
        } else if (w == "copyen") {  // dual copyen on|off: [Hands] DualWieldCopy for the session (a test aid)
            std::string v;
            in >> v;
            g_copy_en = v == "on";
        } else if (w == "copy") {  // dual copy [left|right]: that hand takes a copy of the sidearm in hand
            std::string side;
            in >> side;
            const int ctrl = side == "right" ? 1 : 0;
            begin(ctrl, ctrl, kCopySlot, "test");
        } else if (w == "copyst") {
            copy_status = true;
        } else if (w == "cross") {  // dual cross [on|off]: [Hands] DualWieldCrossReload for the session; the second gun's ammo
            std::string v;
            if (in >> v && (v == "on" || v == "off")) set_cross_reload(v == "on", false);
            const State cs = state();
            char cb[300];
            std::snprintf(cb, sizeof(cb), "cross %d | second: %s, ready %d, copy %d, carry %d, ctrl %d, John's hand %d, weapon %d, clip %.0f/%.0f, spare %.0f | "
                          "loads %llu, rounds %llu",
                          g_cross.load() ? 1 : 0, cs.on ? "out" : "none", cs.ready ? 1 : 0, cs.copy ? 1 : 0, cs.carry ? 1 : 0, cs.ctrl, cs.john, cs.weapon,
                          cs.clip, cs.clip_max, cs.spare, static_cast<unsigned long long>(g_cross_loads.load()),
                          static_cast<unsigned long long>(g_cross_rounds.load()));
            return cb;
        } else if (w == "torch") {  // dual torch [on|off]: [Hands] DualWieldBesideTorch for the session
            std::string v;
            if (in >> v && (v == "on" || v == "off")) set_beside_torch(v == "on", false);
            char tb[220];
            std::snprintf(tb, sizeof(tb), "torch %d (dual %d): the torch (%d) in hand takes a second gun %d | triggers refused for the listener chain %llu (last %d)",
                          g_beside_torch.load() ? 1 : 0, g_enabled.load() ? 1 : 0, kTorchWeapon, beside_torch(kTorchWeapon) ? 1 : 0,
                          static_cast<unsigned long long>(g_notify_refused.load()), g_notify_why.load());
            return tb;
        } else if (w == "beside") {  // dual beside <eWeapon>: (a test's) the gates' answer for that weapon in hand, as the code asks it
            int bw = -1;
            in >> bw;
            char bb[200];
            std::snprintf(bb, sizeof(bb), "beside %d: gun %d, torch %d, second %d | dual %d, torch option %d", bw, reload::is_gun(bw) ? 1 : 0,
                          beside_torch(bw) ? 1 : 0, second_beside(bw) ? 1 : 0, g_enabled.load() ? 1 : 0, g_beside_torch.load() ? 1 : 0);
            return bb;
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
    char b[500];
    std::snprintf(b, sizeof(b),
                  "dual %d | second: %s, ready %d, ctrl %d, John's hand %d, slot %d, weapon %d, clip %.0f, W %p | shots %llu, refused %llu "
                  "(last %d), faults %llu, pulls %llu (unplaced %llu), clicks %llu, begins %llu, ends %llu, ticks %llu, why %d | carry %d (en %d), "
                  "carry weapon %d, carry ready %d, T %d, returning %d, carries %llu, takes %llu",
                  g_enabled.load() ? 1 : 0, s.on ? "out" : "none", s.ready ? 1 : 0, s.ctrl, s.john, s.slot, s.weapon, s.clip,
                  reinterpret_cast<void*>(g_W.load()), static_cast<unsigned long long>(g_shots.load()),
                  static_cast<unsigned long long>(g_refused.load()), g_last_ret.load(), static_cast<unsigned long long>(g_faults.load()),
                  static_cast<unsigned long long>(g_pulls.load()), static_cast<unsigned long long>(g_unplaced.load()),
                  static_cast<unsigned long long>(g_clicks.load()),
                  static_cast<unsigned long long>(g_begins.load()), static_cast<unsigned long long>(g_ends.load()),
                  static_cast<unsigned long long>(g_ticks.load()), g_invalid.load(), s.carry ? 1 : 0, g_carry_en.load() ? 1 : 0, s.carry_weapon,
                  s.carry && s.ready ? 1 : 0, s.carry_T_ok ? 1 : 0, s.carry && g_carry_ret.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_carries.load()), static_cast<unsigned long long>(g_carry_takes.load()));
    return b;
}

}  // namespace rdrvr::dual
