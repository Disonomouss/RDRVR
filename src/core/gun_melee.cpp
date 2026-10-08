#include "core/gun_melee.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/anchors.h"
#include "core/api.h"
#include "core/config.h"
#include "core/log.h"

namespace rdrvr::gun_melee {
namespace {

std::atomic<bool> g_on{false}, g_dry{false}, g_lethal{false}, g_by_peak{true};
std::atomic<float> g_speed{3.0f}, g_arm{1.5f}, g_damage{10.0f}, g_heavy{6.0f}, g_force{1.0f}, g_stock{0.40f};

// the hit side's counters (the script tick writes, the test channel reads)
constexpr int kCodes = RDRVR_MELEE_HIT_COOLDOWN + 1;
std::atomic<uint64_t> g_calls{0}, g_code[kCodes] = {};
std::atomic<float> g_force_seen{-1.0f};
std::atomic<uint64_t> g_force_reads{0};
std::mutex g_last_mutex;
char g_last[260] = "none";

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
void put(uint8_t* info, size_t off, T v) {
    std::memcpy(info + off, &v, sizeof(T));
}
template <class T>
T get(const uint8_t* info, size_t off) {
    T v;
    std::memcpy(&v, info + off, sizeof(T));
    return v;
}
void put3(uint8_t* info, size_t off, const float* v, float k = 1.0f) {
    const float w[3] = {v[0] * k, v[1] * k, v[2] * k};
    std::memcpy(info + off, w, sizeof(w));
}
bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// an actor-pool handle (an actor's +8, idx | generation << 16) -> the actor, the slot's generation checked
uintptr_t actor_of_pool(uint32_t ah) {
    uintptr_t apool = 0, actor = 0;
    uint16_t sgen = 0;
    if (!ah || ah == 0xffffffffu || !rd(anchors::addr(anchors::Id::ActorPool), &apool) || !apool) return 0;
    const uintptr_t slot = apool + static_cast<uintptr_t>(ah & 0xffff) * 0x10;
    if (!rd(slot + 8, &sgen) || sgen != static_cast<uint16_t>(ah >> 16) || !rd(slot, &actor)) return 0;
    return actor;
}
// a script handle (an ObjectsPool GUID: GET_PLAYER_ACTOR's, the iterator's) -> its actor: the slot's generation, an
// actor object (type 15 at +0x2c, as DESTROY's handler reads it), its actor-pool handle (+0xb0) and that slot's
// generation (aim.cpp's player_actor; the natives' own checks, e.g. IS_ACTOR_HUMAN's 0x140281bf0)
uintptr_t actor_of(uint32_t h) {
    uintptr_t pool = 0, obj = 0;
    uint16_t gen = 0;
    uint8_t type = 0;
    uint32_t ah = 0;
    if (!h || h == 0xffffffffu || !rd(anchors::addr(anchors::Id::ObjectsPool), &pool) || !pool) return 0;
    const uintptr_t slot = pool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16) || !rd(slot, &obj) || !obj) return 0;
    if (!rd(obj + 0x2c, &type) || type != 0x0f || !rd(obj + 0xb0, &ah)) return 0;
    return actor_of_pool(ah);
}

int answer(int code) {
    if (code >= 0 && code < kCodes) g_code[code].fetch_add(1, std::memory_order_relaxed);
    return code;
}

// one hit per victim per 500 ms (the script tick only)
struct Recent {
    uint32_t h;
    double ms;
};
Recent g_recent[8] = {};
int g_recent_next = 0;

}  // namespace

void init() {
    g_on = config::get_bool("Gestures", "GunMelee", false);
    g_dry = config::get_bool("Gestures", "GunMeleeDryRun", false);
    g_lethal = config::get_bool("Gestures", "GunMeleeLethal", false);
    auto clampf = [](float v, float lo, float hi, float def) { return !(v >= lo) ? (std::isfinite(v) ? lo : def) : v > hi ? hi : v; };
    // run 8 item 1: a hit from 1.8 m/s at the swing's peak (was 3.0 at contact: the user's swings armed at 1.5-2.1)
    const float speed = clampf(config::get_float("Gestures", "GunMeleeSpeed", 1.8f), 1.0f, 15.0f, 1.8f);
    g_speed = speed;
    g_arm = clampf(config::get_float("Gestures", "GunMeleeArm", 1.2f), 0.9f, speed, 1.2f);
    g_by_peak = config::get_bool("Gestures", "GunMeleeByPeak", true);
    g_damage = clampf(config::get_float("Gestures", "GunMeleeDamage", 10.0f), 0.1f, 1000.0f, 10.0f);
    g_heavy = clampf(config::get_float("Gestures", "GunMeleeHeavy", 6.0f), speed + 0.1f, 30.0f, 6.0f);
    g_force = clampf(config::get_float("Gestures", "GunMeleeForce", 1.0f), 0.0f, 50.0f, 1.0f);
    g_stock = clampf(config::get_float("Gestures", "GunMeleeStockLen", 0.40f), 0.10f, 1.0f, 0.40f);
    log::info("[gunmelee] the gun-butt melee %d (dry run %d): a hit from %.1f m/s %s (scans from %.1f), damage %.1f doubling by %.1f m/s, %s, "
              "force %.2f, the stock %.2f m",
              g_on.load() ? 1 : 0, g_dry.load() ? 1 : 0, g_speed.load(), g_by_peak.load() ? "at the swing's peak" : "at contact", g_arm.load(),
              g_damage.load(), g_heavy.load(),
              g_lethal.load() ? "lethal (health)" : "knock-out points", g_force.load(), g_stock.load());
}

Config config() {
    return {g_on.load(std::memory_order_relaxed), g_speed.load(std::memory_order_relaxed), g_arm.load(std::memory_order_relaxed),
            g_damage.load(std::memory_order_relaxed), g_heavy.load(std::memory_order_relaxed), g_lethal.load(std::memory_order_relaxed),
            g_force.load(std::memory_order_relaxed), g_dry.load(std::memory_order_relaxed), g_stock.load(std::memory_order_relaxed),
            g_by_peak.load(std::memory_order_relaxed)};
}
void set_by_peak(bool on) {
    if (g_by_peak.exchange(on) != on) log::info("[gunmelee] a hit judged %s (the session)", on ? "at the swing's peak" : "at contact");
}
void set_speed(float mps, bool save) {
    mps = mps < 1.0f ? 1.0f : mps > 6.0f ? 6.0f : mps;
    const float old = g_speed.exchange(mps);
    if (g_arm.load() > mps) g_arm = mps;
    if (g_heavy.load() < mps + 0.1f) g_heavy = mps + 0.1f;
    if (old != mps) log::info("[gunmelee] the hit speed %.2f m/s (%s; scans from %.2f, damage doubled by %.1f)", mps, save ? "saved" : "the session",
                              g_arm.load(), g_heavy.load());
    if (save) {
        char v[16];
        std::snprintf(v, sizeof(v), "%.2f", mps);
        config::set("Gestures", "GunMeleeSpeed", v);
    }
}
bool enabled() { return g_on.load(std::memory_order_relaxed); }
void set_enabled(bool on, bool save) {
    if (g_on.exchange(on) != on) log::info("[gunmelee] the gun-butt melee %s (%s)", on ? "on" : "off", save ? "saved" : "the session");
    if (save) config::set("Gestures", "GunMelee", on ? "1" : "0");
}
bool dry_run() { return g_dry.load(std::memory_order_relaxed); }
void set_dry(bool on, bool save) {
    if (g_dry.exchange(on) != on) log::info("[gunmelee] dry run %s (%s)", on ? "on" : "off", save ? "saved" : "the session");
    if (save) config::set("Gestures", "GunMeleeDryRun", on ? "1" : "0");
}

int zone_of(const char* b) {
    if (!b || !*b) return 1;  // the game's -1: the melee keeps its default, 1
    auto pre = [b](const char* s) { return std::strncmp(b, s, std::strlen(s)) == 0; };
    const size_t n = std::strlen(b);
    const bool left = b[n - 1] == 'l';
    if (pre("spine") || pre("root") || pre("shoulder") || pre("clavicle")) {
        if (pre("spine03") || pre("spine02")) return 1;
        return pre("spine00") ? 3 : 4;
    }
    if (std::strcmp(b, "head") == 0 || pre("neck") || pre("Facial")) return 0;
    if (pre("arm") || pre("elbow") || pre("wrist")) return left ? 5 : 6;
    if (pre("pelvis") || pre("hip") || pre("knee") || pre("ankle")) return left ? 7 : 8;
    return 1;
}

int hit(const RdrvrMeleeHit* h) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    // only inside the plugin's tick (the game thread, as KILL_ACTOR_WITH_KILLER's handler calls the same hit), with
    // the anchors verified (ActorHit placed)
    if (!g_on.load(std::memory_order_relaxed) || anchors::stand_down() || !api::in_script_tick() || !anchors::addr(anchors::Id::ActorHit))
        return answer(RDRVR_MELEE_HIT_NOT_NOW);
    if (!h || !finite3(h->pos) || !finite3(h->dir) || !std::isfinite(h->speed) || !(h->speed > 0.0f) || !h->bone[0])
        return answer(RDRVR_MELEE_HIT_BAD_INPUT);
    float dir[3] = {h->dir[0], h->dir[1], h->dir[2]};
    const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(dl > 0.5f && dl < 1.5f)) return answer(RDRVR_MELEE_HIT_BAD_INPUT);
    for (float& x : dir) x /= dl;
    char bone[16] = {};
    std::memcpy(bone, h->bone, sizeof(bone) - 1);
    // both handles through ObjectsPool and ActorPool, each generation checked; the attacker the local player
    // (+0x118 & 3), the victim no player; each actor's own pool handle (+8) back to it (the hit's +0xd4 is resolved so)
    const uintptr_t v = actor_of(h->victim), p = actor_of(h->attacker);
    uint8_t vf = 0, pf = 0;
    uint32_t vh = 0, ph = 0;
    if (!v || !p || v == p || !rd(p + 0x118, &pf) || (pf & 3) != 3 || !rd(v + 0x118, &vf) || (vf & 1) || !rd(p + 8, &ph) ||
        actor_of_pool(ph) != p || !rd(v + 8, &vh) || actor_of_pool(vh) != v)
        return answer(RDRVR_MELEE_HIT_HANDLE);
    // the victim's parts the hit's path reads without a check: FUN_140ae1760 (the AI +0x28 and its actor +0x10, the
    // physics +0xb0), FUN_140ad1110 (the physics' listener list +0x8e8 with flags 2 or 4), FUN_140ae74a0 (the health
    // component +0x60, the ped +0x38), FUN_140cedc90 (the ped's +0xaa8 and that component's +0x20, +0x78, +0x88), the
    // block test's melee controller (+0x80), the facing (the physics' matrix +0x18)
    uintptr_t H = 0, brain = 0, brain_actor = 0, ped = 0, ped_actor = 0, comp = 0, c20 = 0, c78 = 0, c88 = 0, M = 0, vphys = 0, vm = 0, vlist = 0;
    uintptr_t b38 = 0, b38phys = 0;
    uint32_t b38h = 0;
    float hp = 0.0f, ko = 0.0f;
    // FUN_140ae1760 also reads [[AI +0x38] +0xb0] +0x8e0 unchecked on every hit's path (0x140ae1830..0x140ae183d; run 7
    // item 6's review)
    if (!rd(v + 0x28, &brain) || !brain || !rd(brain + 0x38, &b38) || !b38 || !rd(b38 + 0xb0, &b38phys) || !b38phys || !rd(b38phys + 0x8e0, &b38h))
        return answer(RDRVR_MELEE_HIT_VICTIM);
    if (!rd(v + 0x60, &H) || !H || !rd(H + 0x20, &hp) || !(hp > 0.0f) || !rd(H + 0x2c, &ko) || !rd(v + 0x28, &brain) || !brain ||
        !rd(brain + 0x10, &brain_actor) || !brain_actor || !rd(v + 0x38, &ped) || !ped || !rd(ped + 0x10, &ped_actor) || ped_actor != v ||
        !rd(ped + 0xaa8, &comp) || !comp || !rd(comp + 0x20, &c20) || !c20 || !rd(comp + 0x78, &c78) || !c78 || !rd(comp + 0x88, &c88) || !c88 ||
        !rd(comp + 0x80, &M) || !M || !rd(v + 0xb0, &vphys) || !vphys || !rd(vphys + 0x18, &vm) || !vm || !rd(vphys + 0x8e8, &vlist) || !vlist)
        return answer(RDRVR_MELEE_HIT_VICTIM);
    // blocking: its melee controller's state 2 or its flag 0x80 at +0x554 (the fist melee's own test, 0x140d015d1)
    int32_t mst = 0;
    uint8_t mfl = 0;
    if (!rd(M + 0xc0, &mst) || !rd(M + 0x554, &mfl)) return answer(RDRVR_MELEE_HIT_VICTIM);
    if (mst == 2 || (mfl & 0x80)) return answer(RDRVR_MELEE_HIT_BLOCKED);
    // mounted: the physics' mount handle (+0x894) resolves (FUN_140ae5a40 tests it the same way)
    uint32_t mount = 0;
    if (!rd(vphys + 0x894, &mount)) return answer(RDRVR_MELEE_HIT_VICTIM);
    if (actor_of_pool(mount)) return answer(RDRVR_MELEE_HIT_MOUNTED);
    // the attacker: its physics (FUN_140ae1760 reads its +0x8e0), its position (a copy the hit points at) and facing
    uintptr_t pphys = 0, pm = 0;
    float ppos[4] = {}, prow[3] = {}, vrow[3] = {};
    if (!rd(p + 0xb0, &pphys) || !pphys || !rd(pphys + 0x18, &pm) || !pm || !raw(pm + 0x30, ppos, 12) || !raw(pm + 0x20, prow, 12) ||
        !raw(vm + 0x20, vrow, 12) || !finite3(ppos))
        return answer(RDRVR_MELEE_HIT_HANDLE);
    const double now = log::now_ms();
    for (const Recent& r : g_recent)
        if (r.h == h->victim && now - r.ms < 500.0) return answer(RDRVR_MELEE_HIT_COOLDOWN);
    // the damage: GunMeleeDamage at GunMeleeSpeed, doubled by GunMeleeHeavy (linear)
    const Config c = config();
    const float span = c.heavy - c.speed > 0.1f ? c.heavy - c.speed : 0.1f;
    float k = (h->speed - c.speed) / span;
    k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
    const float damage = c.damage * (1.0f + k);
    float game_t = 0.0f;
    rd(anchors::addr(anchors::Id::GameTime), &game_t);
    const int zone = zone_of(bone);
    const uint32_t flags = c.lethal ? 4u : 2u;
    const int away = vrow[0] * prow[0] + vrow[1] * prow[1] + vrow[2] * prow[2] >= 0.0f ? 1 : 0;
    // the DamageInfo, as FUN_140d007f0 builds it (0x140d017aa .. 0x140d01ad2; research 3.1)
    alignas(16) uint8_t info[0x100] = {};
    put<int32_t>(info, 0x00, 0x24);
    const float back[3] = {-dir[0], -dir[1], -dir[2]};
    put3(info, 0x10, h->pos);  // the bone-local record: the game's fallback is the world values (0x140d01a62)
    put3(info, 0x20, back);
    put3(info, 0x30, h->pos);  // the hit point
    put3(info, 0x40, back);    // the surface's normal: back along the strike
    std::memcpy(info + 0x70, bone, 16);
    put<int32_t>(info, 0x80, zone);
    put<int32_t>(info, 0x84, 0);
    put<int32_t>(info, 0x88, away);  // the victim faces away (its facing row against the attacker's, >= 0)
    put<int32_t>(info, 0x8c, -1);    // the bone index: none
    put<float>(info, 0x90, game_t);
    put<uint8_t>(info, 0x96, 1);     // as the fist melee sets it (0x140d01915)
    put<uint8_t>(info, 0x98, 1);     // the record copied into the health component
    put3(info, 0xa0, dir, c.force);  // the reaction's direction and push
    // the attacker's position: the game's own row, as the melee points at it (0x140d0179a..0x140d0186c: [[attacker
    // +0xb0] +0x18] +0x30), never a copy on this stack (the info may be kept past the call)
    put<uintptr_t>(info, 0xb0, pm + 0x30);
    put<uintptr_t>(info, 0xb8, 0);
    put<int32_t>(info, 0xc0, 0);
    put<int32_t>(info, 0xc4, h->weapon);
    put<uint32_t>(info, 0xc8, flags);
    put<float>(info, 0xcc, damage);
    put<uint32_t>(info, 0xd4, ph);   // the attacker's actor-pool handle
    put<uint8_t>(info, 0xd8, 1);
    put<uint8_t>(info, 0xd9, 1);
    put<float>(info, 0xe4, -1.0f);   // outputs: the health after, its ratio
    put<float>(info, 0xe8, -1.0f);
    char line[260];
    if (c.dry) {
        std::snprintf(line, sizeof(line), "dry hit 0x%x %s zone %d at %.1f m/s: damage %.1f, flags %u, faces away %d, at (%.2f %.2f %.2f) along (%.2f %.2f %.2f); health %.1f, KO %.1f",
                      h->victim, bone, zone, h->speed, damage, flags, away, h->pos[0], h->pos[1], h->pos[2], dir[0], dir[1], dir[2], hp, ko);
        log::info("[gunmelee] %s", line);
        {
            std::lock_guard lock(g_last_mutex);
            std::snprintf(g_last, sizeof(g_last), "%s", line);
        }
        g_recent[g_recent_next++ % 8] = {h->victim, now};
        return answer(RDRVR_MELEE_HIT_DRY);
    }
    log::info("[gunmelee] hit 0x%x %s zone %d at %.1f m/s: damage %.1f, flags %u, faces away %d, force %.2f: the game's hit (ActorHit)", h->victim, bone,
              zone, h->speed, damage, flags, away, c.force);
    using ActorHit_t = void (*)(uintptr_t actor, void* info);
    // no __try: FUN_140ad1110 enters a critical section, and a swallowed fault could leave it held (validated above)
    reinterpret_cast<ActorHit_t>(anchors::addr(anchors::Id::ActorHit))(v, info);
    g_recent[g_recent_next++ % 8] = {h->victim, now};
    float hp2 = -1.0f, ko2 = -1.0f;
    rd(H + 0x20, &hp2);
    rd(H + 0x2c, &ko2);
    std::snprintf(line, sizeof(line), "hit 0x%x %s zone %d at %.1f m/s: damage %.1f, flags %u -> health %.1f -> %.1f (info %.1f, ratio %.2f), KO %.1f -> %.1f",
                  h->victim, bone, zone, h->speed, get<float>(info, 0xcc), get<uint32_t>(info, 0xc8), hp, hp2, get<float>(info, 0xe4),
                  get<float>(info, 0xe8), ko, ko2);
    log::info("[gunmelee] %s", line);
    {
        std::lock_guard lock(g_last_mutex);
        std::snprintf(g_last, sizeof(g_last), "%s", line);
    }
    return answer(RDRVR_MELEE_HIT_OK);
}

void note_punch(uintptr_t m) {
    if (!g_on.load(std::memory_order_relaxed) || !m) return;
    uintptr_t tune = 0;
    float f = 0.0f;
    if (!rd(m + 0x10, &tune) || !tune || !rd(tune + 0xd4, &f) || !std::isfinite(f)) return;
    const float was = g_force_seen.exchange(f, std::memory_order_relaxed);
    const uint64_t n = g_force_reads.fetch_add(1, std::memory_order_relaxed);
    if (n < 3 || was != f)
        log::info("[gunmelee] calibration: the game's melee force scale at this punch ([[M+0x10]+0xd4]) %.3f; GunMeleeForce %.3f", f,
                  g_force.load(std::memory_order_relaxed));
}

std::string status() {
    char last[260];
    {
        std::lock_guard lock(g_last_mutex);
        std::snprintf(last, sizeof(last), "%s", g_last);
    }
    auto n = [](int i) { return static_cast<unsigned long long>(g_code[i].load(std::memory_order_relaxed)); };
    // the anchor: its RVA (this build's, relocated or not) and whether verify() let the hooks and calls run
    const bool placed = !anchors::stand_down() && anchors::addr(anchors::Id::ActorHit) != 0;
    char b[720];
    std::snprintf(b, sizeof(b),
                  "the hit: ActorHit RDR.exe+%#x %s | calls %llu, made %llu, dry %llu | refused: not now %llu, bad input %llu, handle %llu, victim %llu, "
                  "blocked %llu, mounted %llu, cooldown %llu | the game's punch force %.3f (reads %llu) | last: %s",
                  anchors::rva(anchors::Id::ActorHit), placed ? (anchors::relocated() ? "verified (relocated)" : "verified") : "NOT placed (stood down)",
                  static_cast<unsigned long long>(g_calls.load()), n(RDRVR_MELEE_HIT_OK), n(RDRVR_MELEE_HIT_DRY), n(RDRVR_MELEE_HIT_NOT_NOW),
                  n(RDRVR_MELEE_HIT_BAD_INPUT), n(RDRVR_MELEE_HIT_HANDLE), n(RDRVR_MELEE_HIT_VICTIM), n(RDRVR_MELEE_HIT_BLOCKED),
                  n(RDRVR_MELEE_HIT_MOUNTED), n(RDRVR_MELEE_HIT_COOLDOWN), g_force_seen.load(), static_cast<unsigned long long>(g_force_reads.load()), last);
    return b;
}

}  // namespace rdrvr::gun_melee
