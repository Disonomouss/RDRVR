#include "core/offhand.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/aim.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/body.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/dual.h"
#include "core/gestures.h"
#include "core/gun_melee.h"
#include "core/held_prop.h"
#include "core/holster.h"
#include "core/log.h"

namespace rdrvr::offhand {
namespace {

constexpr int kWeapons = 38;
constexpr int kKnifeW = 22;
// GET_WEAPON_FRAGMENT_NAME(eWeapon) 0xE8739A48 (handler 0x1402d8e50, read: the weapon-info tree DAT_142b1d398 by the
// eWeapon, each pointer checked; null for an unknown weapon, else weaponInfo +0x1fc when its +0x1fa is set, else a static
// empty string): the item's own model, the name as a string pointer into the game's weapon info
constexpr uint32_t kFragmentName = 0xE8739A48;
// (stage B) the throwable's count, each with the arguments the mod already passes it (main.cpp's actor state,
// reload.cpp's lose_round): GET_AMMOENUM_FOR_WEAPONENUM(w), ACTOR_GET_WEAPON_AMMO(actor, w) -> float,
// ACTOR_SET_WEAPON_AMMO(actor, w, float), ACTOR_SET_INV_AMMO(actor, type, float, 0)
constexpr uint32_t kAmmoEnumFor = 0x17883570;
constexpr uint32_t kGetWeaponAmmo = 0x43DEDFAE;
constexpr uint32_t kSetNextWeapon = 0xBFD6D55F;  // ACTOR_SET_NEXT_WEAPON(actor, eWeapon): the weapon its slot holds (F2)
constexpr uint32_t kSetWeaponAmmo = 0x8266C617;
constexpr uint32_t kSetInvAmmo = 0x4372593E;
// the knife's damage (health, flags 4) at GunMeleeSpeed, doubled by GunMeleeHeavy as the gun melee's: the game's knife
// attack's damage is the action tree's MeleeAnimationTrack m_Damage (in the game's data, not the exe: research run28
// offhand_items.md 3.1) [I]; a constant here, for the headset round to judge
constexpr float kKnifeDamage = 50.0f;
constexpr int kTorchW = 33;
// (stage C) the torch's fire on the person struck (research 3.3; each handler read): FIRE_CREATE_HANDLE() -> a free fire
// handle (0: none); FIRE_CREATE_ON_ACTOR(fire handle, object handle): both generation-checked, refused when the actor
// already burns or no fire is free; FIRE_IS_ACTOR_ON_FIRE(object handle) -> bool (a read); FIRE_RELEASE_HANDLE(fire
// handle, 0): the handle object freed. A handle of 0 is never passed (pool slot 0's generation could match it)
constexpr uint32_t kFireCreateHandle = 0xBBAE9CBD;
constexpr uint32_t kFireOnActor = 0x9679CF84;
constexpr uint32_t kFireIsOnFire = 0x30C4CA99;
constexpr uint32_t kFireRelease = 0xB14B936A;
constexpr int kFires = 4;                 // the torch's fires held at once (the game has 64 handles, shared with its scripts)
constexpr double kFireHoldMs = 15000.0;   // a fire's handle released this long after the fire was made
constexpr double kFireRepeatMs = 3000.0;  // one fire per person within this
constexpr double kThrowWindowMs = 300.0;  // the let-go's throw: the hand's fastest step within this before it (2026-10-31: 300,
                                          // the let-go's 100 ms debounce, the 200 ms before the let-go still within)
constexpr double kPendingMs = 500.0;      // a throw not launched by a script tick within this is dropped (paused, loading)

std::atomic<bool> g_on{false}, g_dry{false};
std::atomic<bool> g_torch_lit{false};  // [Hands] OffHandTorchLit (off)

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
uint64_t fbits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
float as_f32(uint64_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

bool thrown_w(int w) { return w == 23 || w == 24 || w == 25 || w == 29 || w == 35 || w == 36 || w == 37; }
const char* label(int w) {
    switch (w) {
        case 22: return "knife";
        case 23: return "fire bottle";
        case 24: return "dynamite";
        case 25: return "throwing knife";
        case 29: return "tomahawk";
        case 35: return "holy water";
        case 36: return "zombie bait";
        case 37: return "boom bait";
        case 33: return "torch";
        default: return "item";
    }
}

// a name at p, read byte by byte with SEH up to its NUL (never past it: the string's own bounds), at most 63 characters
// of [A-Za-z0-9_] (the game's fragment and weapon names: "p_gen_cannonBall01x" has capitals); out holds 64 bytes
bool read_name(uintptr_t p, char* out) {
    if (!p) return false;
    for (int i = 0; i < 64; ++i) {
        char c = 0;
        if (!raw(p + static_cast<uintptr_t>(i), &c, 1)) return false;
        if (!c) {
            if (out) out[i] = 0;
            return i > 0;
        }
        if (i == 63 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
        if (out) out[i] = c;
    }
    return false;
}

// a queued native's answer awaited by the frame end: given up after kReqMs AND kReqTicks script ticks since it was asked
// (cancelled if no tick took it yet; an answer lost to the results map's clear past 4096 unread is never waited for again)
// and asked again, kReqTries times. Without ticks (paused, loading) it is only waited for: no try is spent on wall-clock
// time alone (the review)
constexpr double kReqMs = 2000.0;
constexpr uint64_t kReqTicks = 8;
constexpr int kReqTries = 5;
struct Req {
    uint64_t id = 0;
    double ms = 0.0;
    uint64_t tick0 = 0;
    int tries = 0;
};
void ask(Req& q, uint32_t hash, const uint64_t* args, uint32_t argc) {
    q.id = api::queue_native(hash, args, argc, 0, nullptr);
    q.ms = log::now_ms();
    q.tick0 = api::script_ticks();
    ++q.tries;
}
// true: the answer in *r; false: none yet (or given up: q.id 0 again, to be asked again while tries remain)
bool answered(Req& q, RdrvrNativeResult* r) {
    if (!q.id) return false;
    if (api::wait_native(q.id, r, 0)) {
        q.id = 0;
        return true;
    }
    if (log::now_ms() - q.ms > kReqMs && api::script_ticks() - q.tick0 >= kReqTicks) {
        api::cancel_op(q.id);
        log::info("[offhand] a native's answer not had in %.0f ms and %llu ticks (try %d of %d)", kReqMs,
                  static_cast<unsigned long long>(kReqTicks), q.tries, kReqTries);
        q.id = 0;
    }
    return false;
}

// each weapon's model name: asked once of the game (a queued GET_WEAPON_FRAGMENT_NAME), copied here (the slot keeps
// the pointer: written once, before the first want, never again); the fallback the research's table (run10)
std::mutex g_mu;  // everything below (the frame end, the test channel, the plugin's tick read it)
char g_frag[kWeapons][64] = {};
Req g_frag_req[kWeapons];
bool g_frag_done[kWeapons] = {};
const char* fallback_fragment(int w) {
    switch (w) {
        case 22: return "melee_knife01x";
        case 23: return "throw_molotov01x";
        case 24: return "throw_dynamite01x";
        // (no throwing knife fragment in fragments.rpf: research/run10's table; a knife's model [I])
        case 25: return "melee_knife01x";
        case 29: return "melee_tomahawk01x";
        case 35: return "throw_holywater01x";
        case 36: return "throw_zombiebait01x";
        case 37: return "throw_zombiebait02x";  // (the bait's second model, the larger: the boom bait's [I])
        case 33: return "melee_torch01x";
        default: return nullptr;
    }
}
const char* fragment_of(int w) {  // under g_mu: the name once known, else nullptr (asked)
    if (w < 0 || w >= kWeapons) return nullptr;
    if (g_frag_done[w]) return g_frag[w][0] ? g_frag[w] : nullptr;
    RdrvrNativeResult r{};
    bool have = answered(g_frag_req[w], &r);
    if (!have && !g_frag_req[w].id) {
        if (g_frag_req[w].tries < kReqTries) {
            const uint64_t args[1] = {static_cast<uint64_t>(static_cast<uint32_t>(w))};
            ask(g_frag_req[w], kFragmentName, args, 1);
            return nullptr;
        }
        r.value = 0;  // (no answer after every try: the fallback)
        have = true;
    }
    if (!have) return nullptr;
    char s[64] = {};
    const bool ok = r.value != 0 && read_name(static_cast<uintptr_t>(r.value), s);
    const char* fb = fallback_fragment(w);
    if (ok && s[0]) std::snprintf(g_frag[w], sizeof(g_frag[w]), "%s", s);
    else if (fb) std::snprintf(g_frag[w], sizeof(g_frag[w]), "%s", fb);
    g_frag_done[w] = true;
    log::info("[offhand] weapon %d's model: %s (%s)", w, g_frag[w][0] ? g_frag[w] : "none", ok && s[0] ? "the game's GET_WEAPON_FRAGMENT_NAME" : "the fallback");
    return g_frag[w][0] ? g_frag[w] : nullptr;
}
// (stage B) each throwable's ammo type, asked once (GET_AMMOENUM_FOR_WEAPONENUM): known, then -1 none or the type
int g_type[kWeapons] = {};
bool g_type_known[kWeapons] = {};
Req g_type_req[kWeapons];
int type_of(int w) { return w >= 0 && w < kWeapons ? (g_type_known[w] ? g_type[w] : -2) : -1; }  // -2: not known yet

struct Hand {
    int w = -1;          // the eWeapon held, -1 none
    int jh = -1;         // John's hand on the controller
    int zone = -1;       // the zone it was taken at
    char zone_key[24] = {};
    double since = 0.0;  // the take
    bool pose_ok = false;
    float pose[12] = {};  // the drawn copy's pose as wanted (the axes X, Y, Z, then the position; world)
    float grab[3] = {};   // the hand's grab point then
    // (stage B) a throwable's count: its clip (ACTOR_GET_WEAPON_AMMO, asked at the take) and the inventory's spare of
    // its type (read from the game's inventory each frame); the actor it was asked for
    Req clip_req;
    float clip = -1.0f, spare = -1.0f;
    uint32_t actor = 0;
};
Hand g_hand[2];
// (stage B) the throw waiting for the plugin's tick: its weapon, type, clip, velocity and place (world), when
struct Pending {
    bool on = false;
    bool main = false;  // (the off-hand build step 2) the game's item in hand thrown by the grip's let-go: not OffHandItems'
    int w = -1, type = -1;
    float clip = 0.0f, vel[3] = {}, org[3] = {}, speed = 0.0f;
    uint32_t actor = 0;
    double ms = 0.0;
};
Pending g_pend;
// counters (the status)
uint64_t g_takes = 0, g_backs = 0, g_backs_zone = 0, g_refused = 0, g_strikes = 0, g_strikes_dry = 0;
uint64_t g_throws = 0, g_launched = 0, g_unlaunched = 0, g_throw_refused = 0, g_slow = 0, g_counted = 0;
// (stage C) the torch's fires: 0 free, 1 its handle asked, 2 set on the person (the handle held until its release).
// Run by the plugin's tick (slot 1): the person struck checked again in the tick that queues the fire on them
// states: 1 the handle asked; 3 the handle had, the fire to be queued (this tick); 4 the fire queued at the front (to be
// seen run by the next tick); 2 set (the handle held until its release)
struct Fire {
    int state = 0;
    uint32_t victim = 0, handle = 0;
    uint64_t req = 0, on_req = 0, check_req = 0;
    uint64_t tick0 = 0;  // the tick the handle was asked in
    double ms = 0.0;
};
Fire g_fire[kFires];
uint64_t g_fires_asked = 0, g_fires_set = 0, g_fires_lit = 0, g_fires_unlit = 0, g_fires_none = 0, g_fires_busy = 0, g_fires_released = 0;
char g_last_fire[160] = "none";
char g_last[200] = "none", g_last_throw[240] = "none";

void put_back(int h, const char* why) {  // under g_mu
    Hand& s = g_hand[h];
    if (s.w < 0) return;
    log::info("[offhand] %s hand: the %s back at the %s (%s)", h ? "right" : "left", label(s.w), s.zone_key[0] ? s.zone_key : "?", why);
    s = Hand{};
}

// ---- the throw (the game thread, inside the plugin's tick; research 3.2): FIRE_PROJECTILE's core with the item's own
// weapon name, every pointer it reads unchecked checked first, the launch's velocity and place the hand's
struct Launch {
    bool on = false, seen = false;
    float vel[3] = {}, org[3] = {};
    int wtype = -1;  // (F4's test) the weapon type passed to the launch instead of the core's 0 (-1: the core's)
    bool thrown = false;  // (F4's test) the launch's 7th and 8th arguments 1, as the game's own throw passes them (FUN_140302a50)
};
thread_local Launch t_launch;
// (2026-11-01, GOAL-offhand-fixes F4, a test: Undead Nightmare's throwables burst at the hand) "offhand throwtest <target m>
// <its own type 0|1>": the core's target that far along the hand's velocity (10 m), the launch given the item's own weapon
// type (the game's own throw passes it; the core passes 0). The session's only
std::atomic<float> g_tt_dist{10.0f};
std::atomic<bool> g_tt_type{false};
// (F4's test) the launch point this far ahead of the hand along its velocity (m). (2026-11-01, GOAL-offhand-changes C13;
// round 64, the user: every off-hand throwable bursts at the hand or drops) a lead by default: launched from the hand the
// projectile stopped at once (John's own collision: cycle H1, 0.07 m in its first 0.1 s, then straight down to the ground);
// 0.6 m ahead it flew (cycle H2, 4.2-4.8 m from a 10 m/s throw); the game throw's flags and the item's own type: no change.
// 0.15 m (cycle H3: 0.15, 0.3, 0.45, 0.6 m all flew 4.4-6.5 m: the hand's own collision), the least that clears (the
// review: a longer lead could start a throw beyond a wall close ahead)
constexpr float kLaunchAhead = 0.15f;
std::atomic<float> g_tt_ahead{kLaunchAhead};
// (F4's test) the game's own throw (FUN_140302a50) launches a thrown item with its 7th argument 1 (the projectile's +0x284 bit
// 8, which its hit handling FUN_140323cf0 reads) and its 8th 1 (+0x2f8 bit 0x20); the core passes 0 for both
std::atomic<bool> g_tt_thrown{false};
using FireCore_t = void (*)(uint32_t actor_handle, const char* name, float spread, const float* origin, const float* target);

// an actor-pool handle (an actor's +8 form, idx | generation << 16) -> the actor, the slot's generation checked
uintptr_t actor_of_pool(uint32_t ah) {
    uintptr_t apool = 0, actor = 0;
    uint16_t sgen = 0;
    if (!ah || ah == 0xffffffffu || !rd(anchors::addr(anchors::Id::ActorPool), &apool) || !apool) return 0;
    const uintptr_t slot = apool + static_cast<uintptr_t>(ah & 0xffff) * 0x10;
    if (!rd(slot + 8, &sgen) || sgen != static_cast<uint16_t>(ah >> 16) || !rd(slot, &actor)) return 0;
    return actor;
}
// the player's script handle (GET_PLAYER_ACTOR's) -> its actor-pool handle, as FIRE_PROJECTILE's handler takes it (the
// object's +0xb0), checked as gun_melee's actor_of: the slot's generation, an actor object (type 15 at +0x2c), its
// actor resolving, the local player (+0x118 & 3)
uint32_t player_pool_handle(uint32_t h) {
    uintptr_t pool = 0, obj = 0;
    uint16_t gen = 0;
    uint8_t type = 0, pf = 0;
    uint32_t ah = 0;
    if (!h || h == 0xffffffffu || !rd(anchors::addr(anchors::Id::ObjectsPool), &pool) || !pool) return 0;
    const uintptr_t slot = pool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16) || !rd(slot, &obj) || !obj) return 0;
    if (!rd(obj + 0x2c, &type) || type != 0x0f || !rd(obj + 0xb0, &ah)) return 0;
    const uintptr_t actor = actor_of_pool(ah);
    if (!actor || !rd(actor + 0x118, &pf) || (pf & 3) != 3) return 0;
    return ah;
}
// a script object handle (the plugin's iterator's victim) -> its actor, checked now: the slot's generation, the object
// and its vtable there (FIRE_CREATE_ON_ACTOR's handler calls its +0xd0 unchecked), an actor object (type 15 at +0x2c)
// whose actor-pool handle resolves, not a player
uintptr_t victim_actor(uint32_t h) {
    uintptr_t pool = 0, obj = 0, vt = 0;
    uint16_t gen = 0;
    uint8_t type = 0, vf = 0;
    uint32_t ah = 0;
    if (!h || h == 0xffffffffu || !rd(anchors::addr(anchors::Id::ObjectsPool), &pool) || !pool) return 0;
    const uintptr_t slot = pool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16) || !rd(slot, &obj) || !obj || !rd(obj, &vt) || !vt) return 0;
    if (!rd(obj + 0x2c, &type) || type != 0x0f || !rd(obj + 0xb0, &ah)) return 0;
    const uintptr_t actor = actor_of_pool(ah);
    if (!actor || !rd(actor + 0x118, &vf) || (vf & 1)) return 0;
    return actor;
}
// the weapon's info and internal name from the weapon-info tree (as GET_WEAPON_INTERNAL_NAME's handler walks it, every
// pointer checked here): info +0x1ba when the word at +0x1b8 is set
bool weapon_info(int w, uintptr_t* info, const char** name) {
    uintptr_t node = 0;
    if (!rd(anchors::addr(anchors::Id::WeaponInfoTree), &node)) return false;
    for (int i = 0; node && i < 64; ++i) {
        int32_t key = 0;
        if (!rd(node + 8, &key)) return false;
        if (w == key) break;
        if (!rd(node + (w < key ? 0x18 : 0x20), &node)) return false;
    }
    uintptr_t entry = 0, inf = 0;
    uint16_t has = 0;
    if (!node || !rd(node, &entry) || !entry || !rd(entry + 0x20, &inf) || !inf || !rd(inf + 0x1b8, &has) || !has || !read_name(inf + 0x1ba, nullptr))
        return false;
    *info = inf;
    *name = reinterpret_cast<const char*>(inf + 0x1ba);
    return true;
}
// the info FUN_1403ba970 will find for `name` (FUN_140590540 on [[WeaponNameMapOwner] +0x10]: the same hash, bucket and
// compare, read here with SEH), 0 none
uintptr_t map_info(const char* name) {
    char want[64] = {};
    if (!read_name(reinterpret_cast<uintptr_t>(name), want)) return 0;
    uintptr_t owner = 0, map = 0, buckets = 0;
    uint32_t count = 0;
    if (!rd(anchors::addr(anchors::Id::WeaponNameMapOwner), &owner) || !owner || !rd(owner + 0x10, &map) || !map || !rd(map + 0x18, &buckets) ||
        !buckets || !rd(map + 0x10, &count) || count == 0 || count > (1u << 20))
        return 0;
    // (the map's own bounds: the bucket index below count, each chain's nodes read field by field, each key to its NUL)
    uint32_t hsh = 0;  // FUN_1405905c0's (0x140590644): h = (h << 4) + (signed char) c; the top nibble folded in
    for (const char* c = want; *c; ++c) {
        hsh = (hsh << 4) + static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(*c)));
        const uint32_t g = hsh & 0xf0000000u;
        if (g) hsh ^= g ^ (g >> 24);
    }
    uintptr_t node = 0;
    if (!rd(buckets + static_cast<uintptr_t>(hsh % count) * 8, &node)) return 0;
    for (int i = 0; node && i < 256; ++i) {
        uintptr_t key = 0, val = 0;
        char k[64] = {};
        if (!rd(node, &key) || !rd(node + 8, &val)) return 0;
        if (key && read_name(key, k) && std::strcmp(k, want) == 0) return val;
        if (!rd(node + 0x10, &node)) return 0;
    }
    return 0;
}
// the chain FUN_1403ba970 dereferences unchecked after its own null test of info +0x30 (0x1403baa12..0x1403baa2a): the
// fire list +0x60, its first entry, that entry's projectile +0x70 (null would make it do nothing; checked too)
bool fire_chain_ok(uintptr_t info, uintptr_t* proj) {
    uintptr_t base = 0, list = 0, first = 0, pd = 0;
    if (!rd(info + 0x30, &base) || !base || !rd(base + 0x60, &list) || !list || !rd(list, &first) || !first || !rd(first + 0x70, &pd) || !pd) return false;
    *proj = pd;
    return true;
}

// (stage C) the torch's fires, each plugin tick (under g_mu): the handle's answer; the person struck checked again in this
// same tick, then the fire queued on them (it runs later in this tick, the game thread not yielding meanwhile); the
// handle released kFireHoldMs later, or at once when the person no longer resolves; a lost answer given up
// The fire on the person and its check are queued at the FRONT of the plugin's queue (api::queue_native_front) right after
// the person is checked, in this tick hook: the plugin runs them in this same tick, before anything queued earlier (its 64
// a tick: only the watches go first), with no ScriptWait between (a backlog of 56 or more at the back cannot push them to
// a later tick: the review). The next tick sees FIRE_CREATE_ON_ACTOR's result; if it did not run (still queued: taken back
// by cancel_op), the person is checked again and both queued again.
void fires_tick() {
    const double now = log::now_ms();
    for (Fire& f : g_fire) {
        RdrvrNativeResult r{};
        if (f.state == 4) {  // queued last tick: run (its result posted), or slipped (taken back: checked and queued again)
            if (api::wait_native(f.on_req, &r, 0)) {
                f.on_req = 0;
                f.state = 2;
                f.ms = now;
                ++g_fires_set;
            } else if (api::cancel_op(f.on_req)) {
                f.on_req = 0;
                if (f.check_req) api::cancel_op(f.check_req);
                f.check_req = 0;
                f.state = 3;
                log::info("[offhand] the torch's fire on 0x%x did not run in its tick: the person checked again", f.victim);
            } else {  // (taken by the plugin, its result not seen: as run)
                f.on_req = 0;
                f.state = 2;
                f.ms = now;
                ++g_fires_set;
            }
        }
        if (f.state == 1) {
            if (api::wait_native(f.req, &r, 0)) {
                f.req = 0;
                f.handle = static_cast<uint32_t>(r.value);
                if (!f.handle || f.handle == 0xffffffffu) {
                    ++g_fires_none;
                    std::snprintf(g_last_fire, sizeof(g_last_fire), "0x%x: no fire handle free", f.victim);
                    f = Fire{};
                    continue;
                }
                f.state = 3;  // (this same tick, below: the person checked and the fire queued)
            } else if (now - f.ms > kReqMs && api::script_ticks() - f.tick0 >= kReqTicks) {
                // no answer: not yet taken by a tick (cancelled: it never runs) or its answer lost (it ran: the handle it
                // made, unknown here, stays the game's)
                const bool never = api::cancel_op(f.req);
                ++g_fires_none;
                std::snprintf(g_last_fire, sizeof(g_last_fire), "0x%x: the fire handle's answer not had (%s)", f.victim,
                              never ? "cancelled before it ran" : "it ran: that handle is lost");
                log::warn("[offhand] the torch's fire: %s", g_last_fire);
                f = Fire{};
            }
        }
        if (f.state == 3) {  // the handle had: the person checked now, the fire queued at the front (it runs in this tick)
            if (!victim_actor(f.victim)) {  // gone (dead and removed, the slot reused): the handle back at once
                const uint64_t a2[2] = {static_cast<uint64_t>(f.handle), 0ull};
                api::queue_native(kFireRelease, a2, 2, 0, nullptr);
                ++g_fires_released;
                std::snprintf(g_last_fire, sizeof(g_last_fire), "0x%x: gone before its fire (the handle released)", f.victim);
                f = Fire{};
                continue;
            }
            const uint64_t a1[1] = {static_cast<uint64_t>(f.victim)};
            f.check_req = api::queue_native_front(kFireIsOnFire, a1, 1);  // (first: the fire goes in front of it)
            const uint64_t a2[2] = {static_cast<uint64_t>(f.handle), static_cast<uint64_t>(f.victim)};
            f.on_req = api::queue_native_front(kFireOnActor, a2, 2);
            f.state = 4;
            continue;
        }
        if (f.state == 2) {
            if (f.check_req && api::wait_native(f.check_req, &r, 0)) {
                f.check_req = 0;
                const bool lit = (r.value & 0xff) != 0;
                (lit ? g_fires_lit : g_fires_unlit) += 1;
                std::snprintf(g_last_fire, sizeof(g_last_fire), "0x%x %s (fire handle 0x%x)", f.victim, lit ? "on fire" : "NOT on fire", f.handle);
                log::info("[offhand] the torch's fire: %s", g_last_fire);
            }
            if (now - f.ms > kFireHoldMs) {
                const uint64_t a2[2] = {static_cast<uint64_t>(f.handle), 0ull};
                api::queue_native(kFireRelease, a2, 2, 0, nullptr);
                ++g_fires_released;
                f = Fire{};
            }
        }
    }
}

void throw_tick() {
    Pending p;
    {
        std::lock_guard lock(g_mu);
        fires_tick();
        if (!g_pend.on) return;
        p = g_pend;
        g_pend.on = false;
    }
    char why[120] = "";
    uintptr_t info = 0, info2 = 0, proj = 0;
    const char* name = nullptr;
    uint32_t ah = 0;
    [&] {  // the first refusal's reason, each check in order
        if (log::now_ms() - p.ms > kPendingMs) {
            std::snprintf(why, sizeof(why), "not launched within %.0f ms (no script tick)", kPendingMs);
            return;
        }
        if (!g_on.load(std::memory_order_relaxed) && !p.main) {
            std::snprintf(why, sizeof(why), "OffHandItems off");
            return;
        }
        if (anchors::stand_down() || !anchors::addr(anchors::Id::FireProjectileCore) || !api::in_script_tick()) {
            std::snprintf(why, sizeof(why), "not now (the anchors stood down, or not inside the plugin's tick)");
            return;
        }
        if (!aim::launch_hooked()) {  // (its velocity is set there: without it the core's 650 m/s would leave the hand)
            std::snprintf(why, sizeof(why), "the projectile launch hook is not installed");
            return;
        }
        ah = player_pool_handle(p.actor);
        if (!ah) {
            std::snprintf(why, sizeof(why), "the player's handle did not resolve");
            return;
        }
        if (!weapon_info(p.w, &info, &name)) {
            std::snprintf(why, sizeof(why), "weapon %d's info or name not found", p.w);
            return;
        }
        info2 = map_info(name);
        if (info2 != info) {
            std::snprintf(why, sizeof(why), "the name map gives another info (%p, the tree's %p)", reinterpret_cast<void*>(info2), reinterpret_cast<void*>(info));
            return;
        }
        if (!fire_chain_ok(info, &proj)) std::snprintf(why, sizeof(why), "weapon %d has no projectile (its fire list)", p.w);
    }();
    if (why[0]) {
        std::lock_guard lock(g_mu);
        ++g_throw_refused;
        std::snprintf(g_last_throw, sizeof(g_last_throw), "weapon %d refused: %s", p.w, why);
        log::info("[offhand] the %s's throw refused: %s", label(p.w), why);
        return;
    }
    // the target 10 m along the hand's velocity (the core's own direction; its 650 m/s replaced in the launch hook)
    const float sp = std::sqrt(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]);
    const float td = g_tt_dist.load(std::memory_order_relaxed);
    if (const float ah = g_tt_ahead.load(std::memory_order_relaxed); ah > 0.0f && sp > 1e-3f)  // (F4's test)
        for (int k = 0; k < 3; ++k) p.org[k] += p.vel[k] / sp * ah;
    alignas(16) float org[4] = {p.org[0], p.org[1], p.org[2], 0.0f}, tgt[4] = {p.org[0], p.org[1] - td, p.org[2], 0.0f};
    if (sp > 1e-3f)
        for (int k = 0; k < 3; ++k) tgt[k] = p.org[k] + p.vel[k] / sp * td;
    t_launch.wtype = g_tt_type.load(std::memory_order_relaxed) ? p.w : -1;
    t_launch.thrown = g_tt_thrown.load(std::memory_order_relaxed);
    t_launch.on = true;
    t_launch.seen = false;
    std::memcpy(t_launch.vel, p.vel, sizeof(t_launch.vel));
    std::memcpy(t_launch.org, p.org, sizeof(t_launch.org));
    // no __try (as gun_melee's ActorHit call): its AI event takes the game's own locks, and a swallowed fault could leave
    // one held; everything it reads unchecked was checked above
    reinterpret_cast<FireCore_t>(anchors::addr(anchors::Id::FireProjectileCore))(ah, name, 0.0f, org, tgt);
    const bool seen = t_launch.seen;
    t_launch.on = false;
    // the count: one fewer, as reload.cpp's lose_round lowers one (the spare as it is now from the game's inventory)
    char cnt[96] = "the count not lowered";
    bool counted = false;
    if (seen && p.type >= 0) {
        float spare = -1.0f, cap = 0.0f;
        const bool sp_ok = dual::inv_entry(p.type, &spare, &cap);
        const uint64_t act = static_cast<uint64_t>(p.actor);
        if (sp_ok && spare >= 9999.0f) {
            std::snprintf(cnt, sizeof(cnt), "an endless count");
        } else if (sp_ok && spare >= 1.0f) {
            const uint64_t a2[4] = {act, static_cast<uint64_t>(p.type), fbits(std::floor(spare + 0.01f) - 1.0f), 0ull};
            api::queue_native(kSetInvAmmo, a2, 4, 0, nullptr);
            std::snprintf(cnt, sizeof(cnt), "the spare %.0f -> %.0f", spare, std::floor(spare + 0.01f) - 1.0f);
            counted = true;
        } else if (sp_ok && p.clip >= 1.0f) {
            const float c1 = std::floor(p.clip + 0.01f) - 1.0f;
            const uint64_t a1[3] = {act, static_cast<uint64_t>(static_cast<uint32_t>(p.w)), fbits(c1 < 0.0f ? 0.0f : c1)};
            api::queue_native(kSetWeaponAmmo, a1, 3, 0, nullptr);  // (its round back to the spare: then the spare as it was)
            const uint64_t a2[4] = {act, static_cast<uint64_t>(p.type), fbits(spare), 0ull};
            api::queue_native(kSetInvAmmo, a2, 4, 0, nullptr);
            std::snprintf(cnt, sizeof(cnt), "the clip %.0f -> %.0f (the spare kept at %.0f)", p.clip, c1, spare);
            counted = true;
        }
    }
    std::lock_guard lock(g_mu);
    (seen ? g_launched : g_unlaunched) += 1;
    if (counted) ++g_counted;
    std::snprintf(g_last_throw, sizeof(g_last_throw), "weapon %d %s from (%.2f %.2f %.2f) at (%.2f %.2f %.2f) m/s (the hand %.2f m/s), %s", p.w,
                  seen ? "launched" : "NOT launched (the projectile pool empty?)", p.org[0], p.org[1], p.org[2], p.vel[0], p.vel[1], p.vel[2], p.speed, cnt);
    log::info("[offhand] the %s thrown: %s", label(p.w), g_last_throw);
}

}  // namespace

int kind_of(int w) { return w == kKnifeW ? kKnife : w == kTorchW ? kTorch : thrown_w(w) ? kThrown : kNone; }

void init() {
    g_on = config::get_bool("Hands", "OffHandItems", false);
    g_torch_lit = config::get_bool("Hands", "OffHandTorchLit", false);
    api::set_tick_hook(throw_tick, 1);
    log::info("[offhand] off-hand items %d: with a gun in one hand, the other hand draws the knife or the torch at the knife's sheath or a "
              "throwable at the satchel (our drawn copy; the gun stays in hand); a fast stab or slash hits as the game's knife (lethal, weapon "
              "22), a fast swing of the torch knocks and sets alight (weapon 33; lit %d: the game's flame asked for its own weapon object); a "
              "throwable let go in a throw leaves the hand with its velocity",
              g_on.load() ? 1 : 0, g_torch_lit.load() ? 1 : 0);
}
bool torch_lit_enabled() { return g_torch_lit.load(std::memory_order_relaxed); }
uintptr_t info_of(int weapon) {
    uintptr_t info = 0;
    const char* name = nullptr;
    return weapon_info(weapon, &info, &name) ? info : 0;
}
void set_torch_lit(bool on, bool save) {
    if (g_torch_lit.exchange(on) != on) log::info("[offhand] the off-hand torch lit: %s (%s)", on ? "on" : "off", save ? "saved" : "the session");
    if (save) config::set("Hands", "OffHandTorchLit", on ? "1" : "0");
}
bool torch_pose(float m[16]) {
    if (!m || !g_on.load(std::memory_order_relaxed) || !g_torch_lit.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_mu);
    for (const Hand& s : g_hand) {
        // (the review) only while the torch's model is known, so drawn: no bare flame in the hand on the first take
        if (s.w != kTorchW || !s.pose_ok || !fragment_of(kTorchW)) continue;
        for (int r = 0; r < 4; ++r) {
            for (int k = 0; k < 3; ++k) m[r * 4 + k] = s.pose[r * 3 + k];
            m[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
        }
        return true;
    }
    return false;
}
bool enabled() { return g_on.load(std::memory_order_relaxed); }
void set_enabled(bool on, bool save) {
    if (g_on.exchange(on) != on) log::info("[offhand] off-hand items %s (%s)", on ? "on" : "off", save ? "saved" : "the session");
    if (save) config::set("Hands", "OffHandItems", on ? "1" : "0");
}

bool take(int h, int jh, int weapon, int zone, const char* zone_key, const RdrvrActorState& st) {
    if (h < 0 || h > 1 || jh < 0 || jh > 1) return false;
    std::lock_guard lock(g_mu);
    char why[80] = "";
    if (!g_on.load(std::memory_order_relaxed)) std::snprintf(why, sizeof(why), "off");
    else if (kind_of(weapon) == kNone) std::snprintf(why, sizeof(why), "weapon %d is no off-hand item", weapon);
    else if (g_hand[1 - h].w >= 0) std::snprintf(why, sizeof(why), "the other hand holds one");
    else if (g_hand[h].w >= 0) std::snprintf(why, sizeof(why), "this hand holds one");
    else if (!st.actor) std::snprintf(why, sizeof(why), "no player actor");
    else if (kind_of(weapon) == kThrown && st.weapon >= 0 && st.weapon < kWeapons) {
        // (F2, the review) the item in hand in this throwable's slot: its count unreadable (the game answers only the slot's
        // own item), and ACTOR_SET_NEXT_WEAPON would swap the item in hand: refused, as holster's one gun a slot
        holster::Arsenal ar{};
        const bool same = holster::arsenal(&ar) && ar.valid && weapon < kWeapons ? ar.equip_slot[weapon] >= 0 && ar.equip_slot[weapon] == ar.equip_slot[st.weapon]
                                                                                 : kind_of(st.weapon) == kThrown;
        if (same) std::snprintf(why, sizeof(why), "it shares the slot of the item in hand (weapon %d)", st.weapon);
    }
    if (why[0]) {
        ++g_refused;
        log::info("[offhand] %s hand at the %s: no off-hand item (%s)", h ? "right" : "left", zone_key ? zone_key : "?", why);
        return false;
    }
    Hand& s = g_hand[h];
    s = Hand{};
    s.w = weapon;
    s.jh = jh;
    s.zone = zone;
    std::snprintf(s.zone_key, sizeof(s.zone_key), "%s", zone_key ? zone_key : "?");
    s.since = log::now_ms();
    s.actor = static_cast<uint32_t>(st.actor);
    ++g_takes;
    (void)fragment_of(weapon);  // (its model asked of the game now, if not yet)
    if (kind_of(weapon) == kThrown) {  // its count: the type once, the clip now (the spare read each frame)
        if (!g_type_known[weapon] && !g_type_req[weapon].id) {
            const uint64_t a1[1] = {static_cast<uint64_t>(static_cast<uint32_t>(weapon))};
            g_type_req[weapon].tries = 0;
            ask(g_type_req[weapon], kAmmoEnumFor, a1, 1);
        }
        // (2026-11-01, GOAL-offhand-fixes F2; round 63 check 6: "none left" whenever the throwable was not its slot's own
        // item, the game answering its count 0) made its slot's item first (ACTOR_SET_NEXT_WEAPON, as dual's second gun:
        // the gun in hand kept), its count asked 300 ms on, once the slot holds it (the frame's loop asks it)
        const uint64_t a2[2] = {static_cast<uint64_t>(s.actor), static_cast<uint64_t>(static_cast<uint32_t>(weapon))};
        api::queue_native(kSetNextWeapon, a2, 2, 0, nullptr);
    }
    controllers::pulse(h, 0.5f, 30);
    log::info("[offhand] %s hand at the %s: the %s (weapon %d) drawn into this hand; the gun in hand (weapon %d) stays", h ? "right" : "left", s.zone_key,
              label(weapon), weapon, st.weapon);
    std::snprintf(g_last, sizeof(g_last), "took weapon %d at the %s (%s hand)", weapon, s.zone_key, h ? "right" : "left");
    return true;
}
void prime_type(int w) {
    if (kind_of(w) != kThrown) return;
    std::lock_guard lock(g_mu);
    if (!g_type_known[w] && !g_type_req[w].id) {
        const uint64_t a1[1] = {static_cast<uint64_t>(static_cast<uint32_t>(w))};
        g_type_req[w].tries = 0;
        ask(g_type_req[w], kAmmoEnumFor, a1, 1);
    }
}
bool throw_in_hand(int h, int w, float clip, uint32_t actor, float* speed_out, double window_ms) {
    if (h < 0 || h > 1 || kind_of(w) != kThrown) return false;
    float vel[3] = {}, org[3] = {}, speed = 0.0f;
    const bool got = gestures::hand_throw(h, window_ms, vel, org, &speed);
    if (speed_out) *speed_out = got ? speed : -1.0f;
    const bool can = anchors::addr(anchors::Id::FireProjectileCore) && anchors::addr(anchors::Id::WeaponInfoTree) &&
                     anchors::addr(anchors::Id::WeaponNameMapOwner) && aim::launch_hooked();
    std::lock_guard lock(g_mu);
    const int type = type_of(w);
    if (!got || speed < gestures::swing_speed() || !can || type < 0 || g_pend.on) {
        if (got && speed >= gestures::swing_speed()) {
            ++g_throw_refused;
            std::snprintf(g_last_throw, sizeof(g_last_throw), "weapon %d (in hand) not thrown: %s", w,
                          !can ? "the throw is not available on this build" : type < 0 ? "its ammo type not known yet" : "a throw still waiting");
        }
        return false;
    }
    Pending& p = g_pend;
    p = Pending{};
    p.on = true;
    p.main = true;
    p.w = w;
    p.type = type;
    p.clip = clip;
    p.actor = actor;
    p.speed = speed;
    p.ms = log::now_ms();
    std::memcpy(p.vel, vel, sizeof(p.vel));
    std::memcpy(p.org, org, sizeof(p.org));
    ++g_throws;
    controllers::pulse(h, 0.6f, 30);
    log::info("[offhand] %s hand let go of the %s in hand in a throw (unlit): %.2f m/s, its velocity (%.2f %.2f %.2f) m/s; clip %.0f",
              h ? "right" : "left", label(w), speed, vel[0], vel[1], vel[2], clip);
    std::snprintf(g_last, sizeof(g_last), "threw weapon %d (in hand) at %.2f m/s", w, speed);
    return true;
}
bool held(int h) {
    if (h < 0 || h > 1) return false;
    std::lock_guard lock(g_mu);
    return g_hand[h].w >= 0;
}
int held_weapon(int h) {
    if (h < 0 || h > 1) return -1;
    std::lock_guard lock(g_mu);
    return g_hand[h].w;
}
int held_zone(int h) {
    if (h < 0 || h > 1) return -1;
    std::lock_guard lock(g_mu);
    return g_hand[h].zone;
}

void release(int h, bool at_zone) {
    if (h < 0 || h > 1) return;
    std::lock_guard lock(g_mu);
    Hand& s = g_hand[h];
    if (s.w < 0) return;
    if (!at_zone && kind_of(s.w) == kThrown) {  // a throw, if the hand moved fast enough just before the let-go
        float vel[3] = {}, org[3] = {}, speed = 0.0f;
        const bool got = gestures::hand_throw(h, kThrowWindowMs, vel, org, &speed);
        const float need = gestures::swing_speed();
        const int type = type_of(s.w);
        const bool counts = type >= 0 && s.clip >= 0.0f && s.spare >= 0.0f;
        // (the throw's anchors: optional, absent on another build of the game: then no throw, the item back)
        const bool can = anchors::addr(anchors::Id::FireProjectileCore) && anchors::addr(anchors::Id::WeaponInfoTree) &&
                         anchors::addr(anchors::Id::WeaponNameMapOwner) && aim::launch_hooked();
        if (got && speed >= need && counts && can && s.clip + s.spare >= 1.0f && !g_pend.on) {
            Pending& p = g_pend;
            p = Pending{};
            p.on = true;
            p.w = s.w;
            p.type = type;
            p.clip = s.clip;
            p.actor = s.actor;
            p.speed = speed;
            p.ms = log::now_ms();
            std::memcpy(p.vel, vel, sizeof(p.vel));
            if (s.pose_ok) std::memcpy(p.org, s.pose + 9, sizeof(p.org));  // from the item as drawn (the hand's grab point)
            else std::memcpy(p.org, org, sizeof(p.org));
            ++g_throws;
            controllers::pulse(h, 0.6f, 30);
            log::info("[offhand] %s hand let go of the %s in a throw: %.2f m/s (needs %.2f), its velocity (%.2f %.2f %.2f) m/s; clip %.0f spare %.0f",
                      h ? "right" : "left", label(s.w), speed, need, vel[0], vel[1], vel[2], s.clip, s.spare);
            std::snprintf(g_last, sizeof(g_last), "threw weapon %d at %.2f m/s", s.w, speed);
            s = Hand{};  // the item left the hand (its count lowered once launched)
            return;
        }
        if (got && speed >= need) {
            ++g_throw_refused;
            std::snprintf(g_last_throw, sizeof(g_last_throw), "weapon %d not thrown: %s", s.w,
                          !can               ? "the throw is not available on this build (its anchors, or the launch hook)"
                          : !counts          ? "its count not known yet"
                          : s.clip + s.spare < 1.0f ? "none left"
                                             : "a throw still waiting");
        } else {
            ++g_slow;
            std::snprintf(g_last_throw, sizeof(g_last_throw), "weapon %d not thrown: let go at %.2f m/s (a throw needs %.2f)", s.w, got ? speed : -1.0f, need);
        }
    }
    ++g_backs;
    if (at_zone) ++g_backs_zone;
    std::snprintf(g_last, sizeof(g_last), "weapon %d back (%s)", s.w, at_zone ? "let go at its zone" : "let go elsewhere: back to its zone");
    controllers::pulse(h, 0.4f, 25);
    put_back(h, at_zone ? "let go at its zone" : "let go elsewhere: back to its zone");
}

void frame(const body::BodyPoints* bp, const RdrvrActorState* st, const float (*grab)[3], int gun_h, bool gun) {
    std::lock_guard lock(g_mu);
    const bool on = g_on.load(std::memory_order_relaxed);
    for (int w = 0; w < kWeapons; ++w) {  // (stage B) the ammo types asked
        RdrvrNativeResult r{};
        if (answered(g_type_req[w], &r)) {
            const int32_t t = static_cast<int32_t>(static_cast<uint32_t>(r.value));
            g_type[w] = t >= 0 && t < 17 ? t : -1;
            g_type_known[w] = true;
            log::info("[offhand] weapon %d's ammo type %d", w, g_type[w]);
        } else if (!g_type_known[w] && !g_type_req[w].id && g_type_req[w].tries > 0) {
            if (g_type_req[w].tries < kReqTries) {  // (given up: asked again)
                const uint64_t a1[1] = {static_cast<uint64_t>(static_cast<uint32_t>(w))};
                ask(g_type_req[w], kAmmoEnumFor, a1, 1);
            } else {
                g_type[w] = -1;  // no answer after every try: no type, so no throw
                g_type_known[w] = true;
            }
        }
    }
    int shown = -1;
    for (int h = 0; h < 2; ++h) {
        Hand& s = g_hand[h];
        if (s.w < 0) continue;
        if (!bp || !st || !grab) {
            put_back(h, "out of first person");
            continue;
        }
        if (!on) {
            put_back(h, "OffHandItems off");
            continue;
        }
        // (the review) the gun must still be in the other hand: a wheel switch to the game's own knife (or any put-away)
        // would leave two of it drawn, so the copy goes back
        if (!gun || h == gun_h) {
            put_back(h, !gun ? "no gun (or item) in the other hand any more" : "this hand holds the gun now");
            continue;
        }
        if (st->weapon == s.w) {  // (step 5, O4) the same item in hand now (a wheel switch): never two of it drawn
            put_back(h, "the same item in the other hand now");
            continue;
        }
        if (kind_of(s.w) == kThrown) {  // its count: the clip asked at the take, the spare from the inventory
            RdrvrNativeResult r{};
            if (answered(s.clip_req, &r)) {
                const float c = as_f32(r.value);
                s.clip = std::isfinite(c) && c >= 0.0f && c < 10000.0f ? c : 0.0f;
            } else if (s.clip < 0.0f && !s.clip_req.id && s.clip_req.tries < kReqTries) {
                if (log::now_ms() - s.since >= 300.0) {  // (F2: its slot's item by then; the review: waited for, not put back)
                    const uint64_t a2[2] = {static_cast<uint64_t>(s.actor), static_cast<uint64_t>(static_cast<uint32_t>(s.w))};
                    ask(s.clip_req, kGetWeaponAmmo, a2, 2);
                }
            } else if (s.clip < 0.0f && !s.clip_req.id) {
                put_back(h, "its count never answered");
                continue;
            }
            float sp = -1.0f, cap = 0.0f;
            const int type = type_of(s.w);
            if (type >= 0 && dual::inv_entry(type, &sp, &cap)) s.spare = sp;
            if (type == -1 || (s.clip >= 0.0f && s.spare >= 0.0f && s.clip + s.spare < 1.0f)) {
                put_back(h, type == -1 ? "it has no ammo type" : "none left");
                continue;
            }
        }
        const int jh = s.jh;
        if (jh < 0 || jh > 1 || !bp->hand_ok[jh]) {
            s.pose_ok = false;
            continue;
        }
        // the item along the controller's pointing (the IK target's -z tipped 10 degrees up: the guns' barrel in the
        // target, its measured constant), its right the target's x: the model's own axes as the guns' (forward -z, up
        // +y) [I: a non-gun model's axes are not measured]; at the hand's grab point
        const float* R = bp->target_rot[jh];  // columns: the target's axes (world)
        const float bt[3] = {0.0f, 0.174f, -0.985f};
        float u[3], x[3] = {R[0], R[3], R[6]}, Z[3], X[3], Y[3];
        for (int k = 0; k < 3; ++k) u[k] = R[k * 3] * bt[0] + R[k * 3 + 1] * bt[1] + R[k * 3 + 2] * bt[2];
        float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (!(ul > 1e-4f)) continue;
        for (int k = 0; k < 3; ++k) Z[k] = -u[k] / ul;
        const float xz = x[0] * Z[0] + x[1] * Z[1] + x[2] * Z[2];
        for (int k = 0; k < 3; ++k) X[k] = x[k] - xz * Z[k];
        const float xl = std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
        if (!(xl > 1e-4f)) continue;
        for (float& c : X) c /= xl;
        Y[0] = Z[1] * X[2] - Z[2] * X[1];
        Y[1] = Z[2] * X[0] - Z[0] * X[2];
        Y[2] = Z[0] * X[1] - Z[1] * X[0];
        for (int k = 0; k < 3; ++k) {
            s.pose[k] = X[k];
            s.pose[3 + k] = Y[k];
            s.pose[6 + k] = Z[k];
            s.pose[9 + k] = grab[jh][k];
            s.grab[k] = grab[jh][k];
        }
        s.pose_ok = true;
        if (shown < 0) shown = h;
    }
    // one slot: the off hand's item (both hands never hold one: take refuses the second)
    const char* frag = shown >= 0 ? fragment_of(g_hand[shown].w) : nullptr;
    held_prop::want(held_prop::kOffhandSlot, frag, frag ? g_hand[shown].pose : nullptr);
}

bool strike_hand(int* h, int* jh, int* weapon) {
    if (!g_on.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_mu);
    for (int i = 0; i < 2; ++i) {
        const Hand& s = g_hand[i];
        if (s.w >= 0 && (kind_of(s.w) == kKnife || kind_of(s.w) == kTorch) && s.pose_ok) {
            if (h) *h = i;
            if (jh) *jh = s.jh;
            if (weapon) *weapon = s.w;
            return true;
        }
    }
    return false;
}
bool strike_weapon(int weapon) {
    if (!g_on.load(std::memory_order_relaxed) || (kind_of(weapon) != kKnife && kind_of(weapon) != kTorch)) return false;
    std::lock_guard lock(g_mu);
    return g_hand[0].w == weapon || g_hand[1].w == weapon;
}
void hit_params(int weapon, float speed, float* damage, uint32_t* flags, bool* dry) {
    const gun_melee::Config c = gun_melee::config();
    const float span = c.heavy - c.speed > 0.1f ? c.heavy - c.speed : 0.1f;
    float k = (speed - c.speed) / span;
    k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
    const bool torch = weapon == kTorchW;
    // the knife: lethal melee, the health (the fist melee's lethal bit, (M +0x554 & 1) * 2 + 2); the torch a club: a
    // knock-out melee with the gun melee's damage (its fire is its own, on_hit)
    if (damage) *damage = (torch ? c.damage : kKnifeDamage) * (1.0f + k);
    if (flags) *flags = torch ? 2u : 4u;
    if (dry) *dry = g_dry.load(std::memory_order_relaxed);
}
void on_hit(uint32_t victim, int weapon, bool made) {
    std::lock_guard lock(g_mu);
    if (made) ++g_strikes;
    else ++g_strikes_dry;
    std::snprintf(g_last, sizeof(g_last), "%s 0x%x with weapon %d", made ? "hit" : "dry hit", victim, weapon);
    if (!made || weapon != kTorchW || !victim) return;
    // (stage C) the torch sets the person struck alight: a fire handle asked now (the plugin's tick), the fire made on
    // them in the tick that reads the answer (fires_tick: the person checked again there)
    const double now = log::now_ms();
    for (const Fire& f : g_fire)
        if (f.state && f.victim == victim && now - f.ms < kFireRepeatMs) return;
    for (Fire& f : g_fire) {
        if (f.state) continue;
        f = Fire{};
        f.state = 1;
        f.victim = victim;
        f.ms = now;
        f.tick0 = api::script_ticks();
        f.req = api::queue_native(kFireCreateHandle, nullptr, 0, 0, nullptr);
        ++g_fires_asked;
        return;
    }
    ++g_fires_busy;
    std::snprintf(g_last_fire, sizeof(g_last_fire), "0x%x: %d fires held already", victim, kFires);
}

bool launch_override(float vel[3], float org[3], int* wtype, bool* thrown) {
    if (!t_launch.on || t_launch.seen) return false;
    t_launch.seen = true;
    if (wtype) *wtype = t_launch.wtype;
    if (thrown) *thrown = t_launch.thrown;
    std::memcpy(vel, t_launch.vel, sizeof(t_launch.vel));
    std::memcpy(org, t_launch.org, sizeof(t_launch.org));
    return true;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w, v;
    in >> c;
    if (line.find(" stab") != std::string::npos) return gestures::offhand_strike_status();  // "offhand stab"
    if (line.find(" throwtest") != std::string::npos) {  // (F4's test) offhand throwtest <target m> <its own type 0|1>
        float d = 10.0f, ah = 0.0f;
        int t = 0, th = 0;
        const size_t at = line.find(" throwtest");
        const int got = std::sscanf(line.c_str() + at + 10, "%f %d %f %d", &d, &t, &ah, &th);
        g_tt_thrown = got >= 4 && th != 0;
        if (got >= 1) g_tt_dist = !(d >= 1.0f) ? 1.0f : d > 2000.0f ? 2000.0f : d;  // (the review: a NaN to 1 m)
        if (got >= 2) g_tt_type = t != 0;  // (the review: the distance alone leaves the type as it was)
        g_tt_ahead = got >= 3 && ah >= 0.0f && ah <= 1.0f ? ah : kLaunchAhead;  // (C13: not given, the default lead)
        char b[128];
        std::snprintf(b, sizeof(b), "throwtest: target %.0f m, its own type %d, launched %.2f m ahead, thrown flags %d", g_tt_dist.load(),
                      g_tt_type.load() ? 1 : 0, g_tt_ahead.load(), g_tt_thrown.load() ? 1 : 0);
        return b;
    }
    if (line.find(" inject") != std::string::npos) {  // "offhand inject <weapon> <x> <y> <z> [speed]" (a test's; the channel's thread)
        std::string kw;
        int iw = -1;
        float p[3] = {}, speed = 4.0f;
        in >> kw >> iw >> p[0] >> p[1] >> p[2];
        if (!in) return "inject: offhand inject <weapon> <x> <y> <z> [speed]";
        // (the review) only the off-hand items' weapons: gun_melee::hit makes another weapon's hit as the gun melee's
        // when GunMelee is on
        if (iw != 22 && iw != 33) return "inject: the weapon must be 22 (the knife) or 33 (the torch)";
        if (!(in >> speed)) speed = 4.0f;
        RdrvrActorState st{};
        if (!api::actor_state(&st) || !st.actor) return "inject: no player actor";
        if (!api::plugin_attached() || anchors::stand_down()) return "inject: the plugin is not attached, or the anchors stood down";
        RdrvrGunMeleeArgs a{};
        for (int k = 0; k < 3; ++k) {
            a.seg[0][0][k] = p[k] - (k == 0 ? 0.4f : 0.0f);
            a.seg[0][1][k] = p[k] + (k == 0 ? 0.4f : 0.0f);
            a.vel[0][k] = k == 0 ? speed : 0.0f;
        }
        a.guard = static_cast<uint64_t>(anchors::addr(anchors::Id::ScriptThreadCurrent));
        a.actor = static_cast<uint32_t>(st.actor);
        a.weapon = iw;
        a.flags = iw == 22 ? RDRVR_GUN_MELEE_NONHUMAN : 0u;  // (step 5) the knife's as the off-hand stab's: zombies too
        a.radius_cm = 200;
        a.speed = 1.0f;
        uint64_t args[12];
        std::memcpy(args, &a, sizeof(args));
        const uint64_t id = api::queue_op(RDRVR_NATIVE_GUN_MELEE, args, 12, nullptr);
        RdrvrNativeResult r{};
        if (!api::wait_native(id, &r, 2000)) {
            api::cancel_op(id);
            return "inject: no answer in 2 s";
        }
        char b[200];
        std::snprintf(b, sizeof(b), "inject weapon %d: outcome %u code %u victim 0x%x at (%.2f %.2f %.2f)", iw,
                      static_cast<unsigned>((r.value >> 32) & 0xff), static_cast<unsigned>((r.value >> 40) & 0xff),
                      static_cast<uint32_t>(r.value), r.vec[0], r.vec[1], r.vec[2]);
        log::info("[offhand] %s", b);
        return b;
    }
    if (line.find(" fire") != std::string::npos) {  // "offhand fire"
        std::lock_guard lock(g_mu);
        int held = 0;
        for (const Fire& f : g_fire) held += f.state ? 1 : 0;
        char b[400];
        std::snprintf(b, sizeof(b), "fires asked %llu, set %llu, lit %llu, not lit %llu, no handle %llu, busy %llu, released %llu, held %d | last: %s",
                      static_cast<unsigned long long>(g_fires_asked), static_cast<unsigned long long>(g_fires_set),
                      static_cast<unsigned long long>(g_fires_lit), static_cast<unsigned long long>(g_fires_unlit),
                      static_cast<unsigned long long>(g_fires_none), static_cast<unsigned long long>(g_fires_busy),
                      static_cast<unsigned long long>(g_fires_released), held, g_last_fire);
        return b;
    }
    if (line.find(" throw") != std::string::npos) {  // "offhand throw"
        std::lock_guard lock(g_mu);
        char b[480];
        std::snprintf(b, sizeof(b), "throws %llu, launched %llu, not launched %llu, refused %llu, slow %llu, counted %llu, pending %d | last: %s",
                      static_cast<unsigned long long>(g_throws), static_cast<unsigned long long>(g_launched),
                      static_cast<unsigned long long>(g_unlaunched), static_cast<unsigned long long>(g_throw_refused),
                      static_cast<unsigned long long>(g_slow), static_cast<unsigned long long>(g_counted), g_pend.on ? 1 : 0, g_last_throw);
        return std::string(b) + " | flight: " + aim::throw_flight();  // (C13)
    }
    while (in >> w) {
        if (w == "on" || w == "off") set_enabled(w == "on", false);  // the session only
        if (w == "dry" && in >> v) g_dry = v == "on";
        if (w == "lit" && in >> v) set_torch_lit(v == "on", false);  // the session only
        if (w == "reset") {
            std::lock_guard lock(g_mu);
            g_takes = g_backs = g_backs_zone = g_refused = g_strikes = g_strikes_dry = 0;
            g_throws = g_launched = g_unlaunched = g_throw_refused = g_slow = g_counted = 0;
            g_fires_asked = g_fires_set = g_fires_lit = g_fires_unlit = g_fires_none = g_fires_busy = g_fires_released = 0;
        }
    }
    std::lock_guard lock(g_mu);
    char b[900];
    // (the review: r52 reads "offhand %d dry %d | takes": OffHandTorchLit's "lit" after the counters, short: the answer
    // is cut near 512 characters)
    int n = std::snprintf(b, sizeof(b), "offhand %d dry %d | takes %llu, back %llu (at the zone %llu), refused %llu, hits %llu, dry hits %llu, lit %d | last: %s",
                          g_on.load() ? 1 : 0, g_dry.load() ? 1 : 0, static_cast<unsigned long long>(g_takes), static_cast<unsigned long long>(g_backs),
                          static_cast<unsigned long long>(g_backs_zone), static_cast<unsigned long long>(g_refused),
                          static_cast<unsigned long long>(g_strikes), static_cast<unsigned long long>(g_strikes_dry), g_torch_lit.load() ? 1 : 0, g_last);
    for (int h = 0; h < 2 && n > 0 && n < static_cast<int>(sizeof(b)); ++h) {
        const Hand& s = g_hand[h];
        float dp[3] = {};
        double dms = 0.0;
        const bool drawn = s.w >= 0 && held_prop::last_drawn(held_prop::kOffhandSlot, dp, &dms);
        const float ex = dp[0] - s.grab[0], ey = dp[1] - s.grab[1], ez = dp[2] - s.grab[2];
        // (compact: the test channel's answer is cut near 512 characters) L / R: the eWeapon, its zone, its model, the pose
        // as wanted (its place), held_prop's last drawn place, how long ago, its distance from the hand's grab point; a
        // throwable's clip and spare
        n += std::snprintf(b + n, sizeof(b) - n, " | %s: w %d z %d m %s pose %d (%.3f %.3f %.3f) drawn %d (%.3f %.3f %.3f) %.0f ms, %.3f m n %.0f+%.0f",
                           h ? "R" : "L", s.w, s.zone, s.w >= 0 && g_frag_done[s.w] && g_frag[s.w][0] ? g_frag[s.w] : "-", s.pose_ok ? 1 : 0, s.pose[9],
                           s.pose[10], s.pose[11], drawn ? 1 : 0, dp[0], dp[1], dp[2], drawn ? log::now_ms() - dms : -1.0,
                           drawn ? std::sqrt(ex * ex + ey * ey + ez * ez) : -1.0f, s.clip, s.spare);
    }
    return b;
}

}  // namespace rdrvr::offhand
