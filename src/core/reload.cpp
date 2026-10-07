#include "core/reload.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "core/anchors.h"
#include "core/api.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/pose.h"

namespace rdrvr::reload {
namespace {

std::atomic<bool> g_hand{true};        // [Reload] Hand
std::atomic<int> g_squeeze{1};  // [Reload] ChestSqueeze: 0 off, 1 one round, 2 a full clip
std::atomic<bool> g_auto{false};       // [Reload] Automatic
std::atomic<bool> g_button{false};     // [Reload] Button
std::atomic<bool> g_two{true};         // [Reload] TwoHanded
std::atomic<bool> g_switch{false};     // [Reload] OutOfAmmoSwitch
std::atomic<bool> g_empty_pending{false};  // an empty trigger asked for a reload (let through: automatic reloads on)
enum Source { kAfterShot, kOnDraw, kCommand, kOther, kSources };
const char* const kSourceNames[kSources] = {"after the last round", "on the draw", "button or empty trigger", "other"};
std::atomic<uint64_t> g_held[kSources], g_passed{0}, g_trigger_held{0}, g_switch_held{0}, g_inserted{0};

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

// The player's actor has bits 0 and 1 of +0x118 set (the test the game's own reload code makes).
bool is_player_actor(uintptr_t actor) {
    uint8_t f = 0;
    return actor && rd(actor + 0x118, &f) && (f & 3) == 3;
}

// FUN_140d1dde0 (gun controller G): the one reload request (research\gc-weapons.md 3.3). Told apart by its return
// address: the automatic reload after the last round, an empty gun drawn with spare rounds, and the weapon command's
// reload bit (the reload button, and the empty trigger, which after_fire_trigger already filtered). Others (an
// action-tree op's one-round load, a network restore) pass.
using Request_t = uint64_t (*)(uintptr_t g);
Request_t o_request = nullptr;

uint64_t hk_request(uintptr_t g) {
    if (g_hand.load(std::memory_order_relaxed) && pose::anchor_active()) {
        uintptr_t ped = 0, actor = 0;
        if (rd(g + 8, &ped) && rd(ped + 0x10, &actor) && is_player_actor(actor)) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const Source s = ret == anchors::addr(anchors::Id::ReloadAfterShotRet) ? kAfterShot
                             : ret == anchors::addr(anchors::Id::ReloadOnDrawRet)  ? kOnDraw
                             : ret == anchors::addr(anchors::Id::ReloadCommandRet) ? kCommand
                                                                                    : kOther;
            bool allow = true;
            if (s == kAfterShot || s == kOnDraw) allow = g_auto.load(std::memory_order_relaxed);
            if (s == kCommand)
                allow = g_button.load(std::memory_order_relaxed) ||
                        (g_auto.load(std::memory_order_relaxed) && g_empty_pending.exchange(false, std::memory_order_relaxed));
            if (!allow) {
                g_held[s].fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        }
    }
    const uint64_t r = o_request(g);
    if (r & 0xff) g_passed.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// FUN_140340480 (weapon manager): when the gun in hand runs dry, another weapon with ammo is equipped (and John says
// he is out). Held back for the player's guns (slots 1, 4, 5, 6) in first person: the empty gun stays in the hand.
using Switch_t = uint64_t (*)(uintptr_t wmgr);
Switch_t o_switch = nullptr;

uint64_t hk_switch(uintptr_t wmgr) {
    if (g_hand.load(std::memory_order_relaxed) && !g_switch.load(std::memory_order_relaxed) && pose::anchor_active()) {
        uintptr_t actor = 0;
        int32_t slot = -1;
        if (rd(wmgr + 0x10, &actor) && is_player_actor(actor) && rd(wmgr + 0x448, &slot) &&
            (slot == 1 || slot == 4 || slot == 5 || slot == 6)) {
            g_switch_held.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }
    return o_switch(wmgr);
}

void save(const char* key, bool on) { config::set("Reload", key, on ? "1" : "0"); }

}  // namespace

void init() {
    g_hand = config::get_bool("Reload", "Hand", true);
    {
        const std::string cs = config::get_string("Reload", "ChestSqueeze", "one");
        g_squeeze = cs == "off" ? 0 : cs == "full" ? 2 : 1;
    }
    g_auto = config::get_bool("Reload", "Automatic", false);
    g_button = config::get_bool("Reload", "Button", false);
    g_two = config::get_bool("Reload", "TwoHanded", true);
    g_switch = config::get_bool("Reload", "OutOfAmmoSwitch", false);
    log::info("[reload] by hand %d, the game's automatic %d, button %d, out-of-ammo switch %d; two-handed long guns %d",
              g_hand.load() ? 1 : 0, g_auto.load() ? 1 : 0, g_button.load() ? 1 : 0, g_switch.load() ? 1 : 0, g_two.load() ? 1 : 0);
}

bool install() {
    bool ok = hooks::install("RDR reload request (reload)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ReloadRequest)), hk_request,
                             &o_request);
    ok &= hooks::install("RDR out-of-ammo switch (reload)", reinterpret_cast<void*>(anchors::addr(anchors::Id::OutOfAmmoSwitch)), hk_switch,
                         &o_switch);
    return ok;
}

bool hand_reload() { return g_hand.load(); }
void set_hand_reload(bool on) {
    if (g_hand.exchange(on) != on) log::info("[reload] by hand: %s", on ? "on" : "off");
    save("Hand", on);
}
bool automatic() { return g_auto.load(); }
void set_automatic(bool on) {
    if (g_auto.exchange(on) != on) log::info("[reload] the game's automatic reloads: %s", on ? "on" : "off");
    save("Automatic", on);
}
bool button() { return g_button.load(); }
void set_button(bool on) {
    if (g_button.exchange(on) != on) log::info("[reload] the game's reload button: %s", on ? "on" : "off");
    save("Button", on);
}
bool two_handed() { return g_two.load(); }
void set_two_handed(bool on) {
    if (g_two.exchange(on) != on) log::info("[reload] two-handed long guns: %s", on ? "on" : "off");
    save("TwoHanded", on);
}

bool is_gun(int32_t w) { return (w >= 0 && w <= 20) || w == 31 || w == 34; }
bool is_long_gun(int32_t w) { return (w >= 8 && w <= 20) || w == 31 || w == 34; }

void after_fire_trigger(uintptr_t info, uint8_t before) {
    if (!g_hand.load(std::memory_order_relaxed) || !pose::anchor_active()) return;
    uint8_t now = 0;
    if (before || !rd(info + 0x248, &now) || !now) return;
    // the fire trigger found the clip empty with spare rounds and asked for a reload (0x1403483b8)
    if (g_auto.load(std::memory_order_relaxed)) {
        g_empty_pending = true;
    } else if (wr<uint8_t>(info + 0x248, 0)) {
        g_trigger_held.fetch_add(1, std::memory_order_relaxed);
    }
}

bool fill_clip(const RdrvrActorState& st) {
    if (!g_hand.load(std::memory_order_relaxed) || !st.valid || !st.actor || !is_gun(st.weapon)) return false;
    if (st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING)) return false;
    float n = st.clip_max - st.clip;
    if (n > st.spare) n = st.spare;
    n = std::floor(n + 0.01f);
    if (!(n >= 1.0f) || n > 200.0f) return false;
    uint32_t bits = 0;
    std::memcpy(&bits, &n, sizeof(bits));  // a float read with movss
    const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(static_cast<uint32_t>(st.weapon)), bits};
    api::queue_native(0xCC69DCC1, args, 3, 0, nullptr);  // ACTOR_ADD_WEAPON_AMMO
    g_inserted.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return true;
}

int chest_squeeze() { return g_squeeze.load(); }
void set_chest_squeeze(int mode, bool save) {
    g_squeeze = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    if (save) config::set("Reload", "ChestSqueeze", g_squeeze.load() == 0 ? "off" : g_squeeze.load() == 2 ? "full" : "one");
}

bool insert_round(const RdrvrActorState& st) {
    if (!g_hand.load(std::memory_order_relaxed) || !st.valid || !st.actor || !is_gun(st.weapon)) return false;
    if (st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING)) return false;
    if (st.clip + 0.5f > st.clip_max || st.spare < 1.0f) return false;
    // the count is a float read with movss (an integer 1 would be a denormal): 1.0f's bits
    const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(static_cast<uint32_t>(st.weapon)),
                              0x3F800000ull};
    api::queue_native(0xCC69DCC1, args, 3, 0, nullptr);  // ACTOR_ADD_WEAPON_AMMO
    g_inserted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w, v;
    in >> c;
    while (in >> w >> v) {
        const bool on = v == "on";
        if (w == "hand") set_hand_reload(on);
        if (w == "auto") set_automatic(on);
        if (w == "button") set_button(on);
        if (w == "two") set_two_handed(on);
        if (w == "switch") g_switch = on;  // this session only
    }
    RdrvrActorState st{};
    const bool have = api::actor_state(&st);
    char b[480];
    std::snprintf(b, sizeof(b),
                  "hand %d auto %d button %d two %d switch %d | held: after shot %llu, on draw %llu, command %llu, empty trigger %llu, "
                  "switch %llu; passed %llu; inserted %llu | weapon %d clip %.0f/%.0f spare %.0f flags %#x",
                  g_hand.load() ? 1 : 0, g_auto.load() ? 1 : 0, g_button.load() ? 1 : 0, g_two.load() ? 1 : 0, g_switch.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_held[kAfterShot].load()), static_cast<unsigned long long>(g_held[kOnDraw].load()),
                  static_cast<unsigned long long>(g_held[kCommand].load()), static_cast<unsigned long long>(g_trigger_held.load()),
                  static_cast<unsigned long long>(g_switch_held.load()), static_cast<unsigned long long>(g_passed.load()),
                  static_cast<unsigned long long>(g_inserted.load()), have ? st.weapon : -1, have ? st.clip : 0.0f, have ? st.clip_max : 0.0f,
                  have ? st.spare : 0.0f, have ? st.weapon_flags : 0u);
    (void)kSourceNames;
    return b;
}

}  // namespace rdrvr::reload
