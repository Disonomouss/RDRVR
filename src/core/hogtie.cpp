#include "core/hogtie.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

#include "common/rdrvr_api.h"
#include "core/api.h"
#include "core/config.h"
#include "core/held_prop.h"
#include "core/log.h"

namespace rdrvr::hogtie {
namespace {

constexpr uint32_t kGetLassoTarget = 0xAA364907u;  // GET_LASSO_TARGET(user) -> the roped actor
constexpr uint32_t kWaitMs = 500;                  // a native's result: the next script tick or two
constexpr int kEmptyTicks = 4;                     // the rope empty this many checks (250 ms each) before the props show

std::atomic<bool> g_on{true};
std::atomic<bool> g_clear{false};  // the props hidden now
std::atomic<uint64_t> g_ropes{0};  // times someone was found on the lasso (the props hidden)
std::atomic<int32_t> g_target{0};

// the one on the player's lasso, or 0 (none, or no answer: paused, between levels)
bool lasso_target(int32_t player, int32_t* out) {
    uint64_t args[1] = {static_cast<uint64_t>(static_cast<uint32_t>(player))};
    const uint64_t id = api::queue_native(kGetLassoTarget, args, 1);
    RdrvrNativeResult r{};
    if (!api::wait_native(id, &r, kWaitMs)) {
        api::cancel_op(id);
        return false;
    }
    *out = static_cast<int32_t>(static_cast<uint32_t>(r.value));
    return true;
}

void watcher() {
    int empty = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const bool on = g_on.load(std::memory_order_relaxed);
        if (!on) {
            if (g_clear.exchange(false)) held_prop::set_clear(false);
            continue;
        }
        // (no holster prop wanted and none hidden: nothing to clear, no native queued: the review)
        if (!g_clear.load(std::memory_order_relaxed) && !held_prop::holster_wanted()) {
            empty = 0;
            continue;
        }
        RdrvrActorState st{};
        if (!api::actor_state(&st) || !st.valid) continue;
        int32_t t = 0;
        if (!lasso_target(st.actor, &t)) continue;  // no script tick (paused): kept as it is
        const bool roped = t != 0 && t != -1 && t != st.actor && !(st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING));
        empty = roped ? 0 : empty + 1;
        const bool clear = g_clear.load(std::memory_order_relaxed);
        const bool want = roped || (clear && empty < kEmptyTicks);
        if (want != clear) {
            g_clear = want;
            held_prop::set_clear(want);
            if (want) {
                g_ropes.fetch_add(1, std::memory_order_relaxed);
                g_target = t;
                log::info("[hogtie] %d on the lasso: the mod's props hidden (the game's hogtie check sweeps between you)", t);
            } else {
                log::info("[hogtie] the lasso empty: the mod's props shown again");
            }
        }
    }
}

}  // namespace

void init() {
    g_on = config::get_bool("Gestures", "HogtieClearProps", true);
    std::thread(watcher).detach();
    log::info("[hogtie] the mod's props hidden while someone is on the lasso (HogtieClearProps) %d", g_on.load() ? 1 : 0);
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v;
    in >> c >> v;
    if (v == "on" || v == "off") g_on = v == "on";
    char b[200];
    std::snprintf(b, sizeof(b), "hogtie: props hidden on the lasso %s, now %s (the one on it %d), times %llu", g_on.load() ? "on" : "off",
                  g_clear.load() ? "hidden" : "shown", g_target.load(), static_cast<unsigned long long>(g_ropes.load()));
    return b;
}

}  // namespace rdrvr::hogtie
