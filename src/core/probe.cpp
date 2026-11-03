#include "core/probe.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "core/aim.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/log.h"

namespace rdrvr::probe {
namespace {

// research/RDR.exe (the analysed build) only: the RVAs and each function's first 16 bytes there (read 2026-10-31 with
// pefile; Ghidra: push rbx; sub rsp,0x20; ...)
constexpr uint32_t kSnapRva = 0xd19670;  // FUN_140d19670(G): if G +0x3bc: ped +0x978 = 0, G +0x5d5 &= ~0x40, then for
                                         // wmgr = [[G +8] +0x10] +0x70 (null-checked): FUN_140340080, FUN_140342ba0,
                                         // FUN_140d196d0(G) (G state 0, G +0x3f8 released)
constexpr uint8_t kSnapBytes[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x83, 0xb9, 0xbc, 0x03, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9};
constexpr uint32_t kFuseRva = 0xd1bde0;  // FUN_140d1bde0(G, lit): G +0x5ea bit 0x40; lit 0 -> 1: W +0x13c = info +0x32c
                                         // (when the fire list has a projectile with +200 set); unlit: W +0x13c = -1
constexpr uint8_t kFuseBytes[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x0f, 0xb6, 0x89, 0xea, 0x05, 0x00, 0x00};
constexpr uint32_t kDlcRva = 0x2ad1c98;  // DAT_142ad1c98: a pointer, +0xc set while Undead Nightmare is on (FUN_140d1bde0's
                                         // unlit path reads it unchecked, 0x140d1bf1f)
constexpr uint32_t kWeaponInfoTreeRva = 0x2b1d398;  // (anchors.txt WeaponInfoTree; read through the anchor below)

using Snap_t = void (*)(uintptr_t G);
using Fuse_t = void (*)(uintptr_t G, uint8_t lit);

bool raw(uintptr_t a, void* out, size_t n) {
    if (a < 0x10000) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename T>
bool rd(uintptr_t a, T* v) {
    return raw(a, v, sizeof(T));
}
template <typename T>
T get(uintptr_t a, T fallback) {
    T v{};
    return rd(a, &v) ? v : fallback;
}

enum Op : int { kNone = 0, kSnap = 1, kFuseOn = 2, kFuseOff = 3 };

std::mutex g_mu;
std::condition_variable g_cv;
std::atomic<int> g_pending{kNone};  // set by the channel's thread, taken by the tick
std::atomic<bool> g_hooked{false};
uint64_t g_seq = 0, g_done = 0;     // under g_mu
bool g_running = false;             // under g_mu: taken by the tick, its reply not yet posted
std::string g_reply;                // under g_mu

// why the game function at `rva` may not be called now (nullptr: it may)
const char* refuse(uint32_t rva, const uint8_t* bytes) {
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated()) return "not the analysed RDR.exe build (nothing called)";
    if (!api::in_script_tick()) return "not inside the plugin's script tick (nothing called)";
    uint8_t b[16] = {};
    if (!raw(anchors::base() + rva, b, sizeof(b)) || std::memcmp(b, bytes, sizeof(b)) != 0)
        return "the function's first bytes differ from research/RDR.exe's (nothing called)";
    return nullptr;
}

// the player's G and the chain the game functions read unchecked: ped = [G +8], actor = [ped +0x10]; wmgr = [actor +0x70]
// (may be 0: both functions test it)
struct Gc {
    uintptr_t G = 0, ped = 0, actor = 0, wmgr = 0;
};
const char* player_g(Gc* g) {
    g->G = aim::player_gun_ctl();
    if (!g->G) return "the player's G not found";
    if (!rd(g->G + 8, &g->ped) || !g->ped) return "G +8 (the ped) null";
    if (!rd(g->ped + 0x10, &g->actor) || !g->actor) return "the ped's +0x10 (the actor) null";
    if (!rd(g->actor + 0x70, &g->wmgr)) return "the actor's +0x70 unreadable";
    return nullptr;
}

std::string snap_state(const Gc& g) {
    const uintptr_t item = g.wmgr ? get<uintptr_t>(g.wmgr + 0x80, 0) : 0;
    char b[256];
    std::snprintf(b, sizeof(b), "G state %d G+0x5d5 0x%02x G+0x3f8 %p ped+0x978 %d wmgr+0x80 %p item state %d", get<int32_t>(g.G + 0x3bc, -1),
                  get<uint8_t>(g.G + 0x5d5, 0), reinterpret_cast<void*>(get<uintptr_t>(g.G + 0x3f8, 0)), get<int32_t>(g.ped + 0x978, -1),
                  reinterpret_cast<void*>(item), item ? get<int32_t>(item + 0x24, -1) : -1);
    return b;
}

std::string run_snap() {
    if (const char* why = refuse(kSnapRva, kSnapBytes)) return std::string("ERROR probe snap: ") + why;
    Gc g;
    if (const char* why = player_g(&g)) return std::string("ERROR probe snap: ") + why + " (nothing called)";
    const std::string before = snap_state(g);
    const int32_t st = get<int32_t>(g.G + 0x3bc, -1);
    // no __try (as gun_melee's ActorHit call): the game's own code, which may take its locks; every pointer it reads
    // unchecked (G +8, the ped's +0x10) was checked above
    reinterpret_cast<Snap_t>(anchors::base() + kSnapRva)(g.G);
    const std::string after = snap_state(g);
    char b[700];
    std::snprintf(b, sizeof(b), "OK probe snap G %p%s: before {%s} after {%s}", reinterpret_cast<void*>(g.G),
                  st == 0 ? " (G state was 0: the snap does nothing)" : "", before.c_str(), after.c_str());
    log::info("[probe] %s", b);
    return b;
}

// the item in hand's W as FUN_140d1bde0 finds it (wmgr +0x80, its vtable +0xd8, +0xa0), through aim's checked getter
std::string fuse_state(uintptr_t G, uintptr_t W) {
    char b[200];
    std::snprintf(b, sizeof(b), "G+0x5ea 0x%02x (lit %d) W+0x13c %.3f W+0x918 %.4f", get<uint8_t>(G + 0x5ea, 0), (get<uint8_t>(G + 0x5ea, 0) >> 6) & 1,
                  W ? get<float>(W + 0x13c, -999.0f) : -999.0f, W ? get<float>(W + 0x918, -999.0f) : -999.0f);
    return b;
}

std::string run_fuse(bool lit) {
    const char* tag = lit ? "ERROR probe fuse on: " : "ERROR probe fuse off: ";
    if (const char* why = refuse(kFuseRva, kFuseBytes)) return std::string(tag) + why;
    Gc g;
    if (const char* why = player_g(&g)) return std::string(tag) + why + " (nothing called)";
    float m[16];
    bool left = false;
    uintptr_t W = 0, wmgr = 0;
    if (!aim::held_item_matrix(m, &left, &W, &wmgr) || !W) return std::string(tag) + "no item in hand (nothing called)";
    uintptr_t info = 0;
    if (!rd(W + 0x28, &info) || !info) return std::string(tag) + "W +0x28 (the weapon info) null (nothing called)";
    const int16_t wtype = get<int16_t>(info + 8, -1);
    const float fuse = get<float>(info + 0x32c, 0.0f);
    char chain[160] = "";
    if (lit) {
        // the lit path's walk, read unchecked by the game: [info +0x30 + W+0x30 * 8] (or [info +0x30]) +0x60, each node
        // {entry, next}, entry +0x70 (the projectile) +200; checked here up to the first entry it accepts
        const int32_t idx = get<int32_t>(W + 0x30, 0);
        if (fuse > 0.0f) {
            uintptr_t fire = 0, node = 0;
            if (!rd(info + 0x30 + static_cast<intptr_t>(idx) * 8, &fire)) return std::string(tag) + "the fire entry's slot unreadable (nothing called)";
            if (!fire && (!rd(info + 0x30, &fire) || !fire)) fire = 0;
            if (fire && rd(fire + 0x60, &node)) {
                int n = 0;
                for (; node && n < 64; ++n) {
                    uintptr_t entry = 0, proj = 0, timeout = 0;
                    if (!rd(node, &entry) || !entry || !rd(entry + 0x70, &proj) || !proj || !rd(proj + 200, &timeout))
                        return std::string(tag) + "a fire list entry or its projectile null (nothing called)";
                    if (timeout) {
                        std::snprintf(chain, sizeof(chain), ", the fire list's entry %d has the timeout definition %p", n, reinterpret_cast<void*>(timeout));
                        break;
                    }
                    if (!rd(node + 8, &node)) return std::string(tag) + "a fire list node unreadable (nothing called)";
                }
                if (n >= 64) return std::string(tag) + "the fire list longer than 64 (nothing called)";
                if (!chain[0]) std::snprintf(chain, sizeof(chain), ", no fire entry with a timeout definition (the fuse will not be set)");
            } else {
                std::snprintf(chain, sizeof(chain), ", no fire list (the fuse will not be set)");
            }
        } else {
            std::snprintf(chain, sizeof(chain), ", info +0x32c is %.3f (the fuse will not be set)", fuse);
        }
    } else {
        uintptr_t dlc = 0;
        uint8_t un = 0;
        if (!rd(anchors::base() + kDlcRva, &dlc) || !dlc || !rd(dlc + 0xc, &un)) return std::string(tag) + "DAT_142ad1c98 null (nothing called)";
        std::snprintf(chain, sizeof(chain), ", Undead Nightmare flag %d", un);
    }
    const std::string before = fuse_state(g.G, W);
    reinterpret_cast<Fuse_t>(anchors::base() + kFuseRva)(g.G, lit ? 1 : 0);  // no __try: as run_snap
    const std::string after = fuse_state(g.G, W);
    char b[700];
    std::snprintf(b, sizeof(b), "OK probe fuse %s G %p W %p weapon %d (%s hand) info+0x32c %.3f%s: before {%s} after {%s}", lit ? "on" : "off",
                  reinterpret_cast<void*>(g.G), reinterpret_cast<void*>(W), wtype, left ? "left" : "right", fuse, chain, before.c_str(), after.c_str());
    log::info("[probe] %s", b);
    return b;
}

std::string run(int op) {
    switch (op) {
        case kSnap: return run_snap();
        case kFuseOn: return run_fuse(true);
        case kFuseOff: return run_fuse(false);
        default: return "ERROR probe: unknown op";
    }
}

// the core's tick hook slot 2 (set by the first probe command): the game thread, inside the plugin's script tick
void probe_tick() {
    if (g_pending.load(std::memory_order_acquire) == kNone) return;
    uint64_t seq = 0;
    int op = kNone;
    {
        std::lock_guard lock(g_mu);
        op = g_pending.exchange(kNone);
        if (op == kNone) return;
        seq = g_seq;
        g_running = true;
    }
    std::string r = run(op);
    {
        std::lock_guard lock(g_mu);
        g_reply = std::move(r);
        g_done = seq;
        g_running = false;
    }
    g_cv.notify_all();
}

std::string on_game_thread(int op) {
    if (!g_hooked.exchange(true)) {
        api::set_tick_hook(probe_tick, 2);
        log::info("[probe] the test-only probe commands' tick hook set (slot 2)");
    }
    std::unique_lock lock(g_mu);
    if (g_running || g_pending.load() != kNone) return "ERROR probe: an earlier probe is still running";
    const uint64_t seq = ++g_seq;
    g_pending.store(op, std::memory_order_release);
    if (!g_cv.wait_for(lock, std::chrono::milliseconds(3000), [&] { return g_done == seq; })) {
        if (g_pending.exchange(kNone) != kNone) return "ERROR probe: no script tick in 3 s (not run)";
        return "ERROR probe: taken by the script tick but not answered in 3 s";
    }
    return g_reply;
}

// ---- probe fuseinfo: read only, on the channel's thread (the weapon-info tree is the game's static data)
bool weapon_info(int w, uintptr_t* info, char name[64]) {
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
    if (!node || !rd(node, &entry) || !entry || !rd(entry + 0x20, &inf) || !inf || !rd(inf + 0x1b8, &has) || !has) return false;
    if (!raw(inf + 0x1ba, name, 63)) return false;
    name[63] = 0;
    *info = inf;
    return true;
}

std::string fuseinfo(const std::vector<int>& ws) {
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated() || anchors::rva(anchors::Id::WeaponInfoTree) != kWeaponInfoTreeRva)
        return "ERROR probe fuseinfo: not the analysed RDR.exe build (nothing read)";
    std::string out = "OK probe fuseinfo";
    for (int w : ws) {
        uintptr_t info = 0;
        char name[64] = {};
        char b[400];
        if (!weapon_info(w, &info, name)) {
            std::snprintf(b, sizeof(b), " | %d: no weapon info", w);
            out += b;
            continue;
        }
        // the fire list as offhand.cpp's fire_chain_ok reads it (info +0x30, its +0x60, the first entry, +0x70), each entry's
        // projectile definition: +0x20 (the life FUN_140320180 gives a throw without a fuse) and +200 (a timeout definition)
        uintptr_t base = 0, node = 0;
        std::string projs;
        int n = 0;
        if (rd(info + 0x30, &base) && base && rd(base + 0x60, &node))
            for (; node && n < 8; ++n) {
                uintptr_t entry = 0, pd = 0;
                char e[120];
                if (!rd(node, &entry) || !entry || !rd(entry + 0x70, &pd) || !pd) {
                    std::snprintf(e, sizeof(e), " [%d: null]", n);
                    projs += e;
                    break;
                }
                std::snprintf(e, sizeof(e), " [%d: def %p +0x20 %.3f +200 %p]", n, reinterpret_cast<void*>(pd), get<float>(pd + 0x20, -999.0f),
                              reinterpret_cast<void*>(get<uintptr_t>(pd + 200, 0)));
                projs += e;
                if (!rd(node + 8, &node)) break;
            }
        std::snprintf(b, sizeof(b), " | %d %s: info %p +0x32c %.3f type %d fire%s", w, name, reinterpret_cast<void*>(info), get<float>(info + 0x32c, -999.0f),
                      get<int16_t>(info + 8, -1), projs.empty() ? " none" : projs.c_str());
        out += b;
    }
    log::info("[probe] %s", out.c_str());
    return out;
}

}  // namespace

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v, a;
    in >> c >> v >> a;  // "probe" <verb> [arg]
    if (v == "snap") return on_game_thread(kSnap);
    if (v == "fuse" && (a == "on" || a == "off")) return on_game_thread(a == "on" ? kFuseOn : kFuseOff);
    if (v == "fuseinfo") {
        std::vector<int> ws;
        if (!a.empty()) {
            char* end = nullptr;
            const long w = std::strtol(a.c_str(), &end, 10);
            if (!end || *end || w < 0 || w > 1000) return "ERROR probe fuseinfo [weapon]: a weapon number";
            ws.push_back(static_cast<int>(w));
        } else {
            ws = {24, 25, 37};
        }
        return fuseinfo(ws);
    }
    return "ERROR usage: probe snap | probe fuse on|off | probe fuseinfo [weapon] (test only)";
}

}  // namespace rdrvr::probe
