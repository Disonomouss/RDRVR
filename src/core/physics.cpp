#include "core/physics.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include "core/anchors.h"
#include "core/api.h"
#include "core/body.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/dual.h"
#include "core/controllers.h"
#include "core/hands.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/pose.h"

namespace rdrvr::physics {
namespace {

std::atomic<bool> g_on{false};  // [Physics] HandCollision
float g_gain = 1.2f, g_max = 5.0f, g_min_speed = 0.5f, g_radius = 0.35f, g_cooldown_ms = 120.0f;
int g_max_touched = 96, g_pool_floor = 256;
// [Physics] Grab (run 5 item 5, research\run3\handphysics.md 6.6): a loose prop held in an empty hand, thrown on release
std::atomic<bool> g_grab_on{false};
float g_grab_radius = 0.15f, g_throw_gain = 1.0f, g_throw_max = 12.0f;

uint64_t f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
bool raw(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the frame end's state (one thread); the test channel reads under the mutex
std::mutex g_mutex;
struct HandTrack {
    float p[3] = {};
    double ms = 0;
    bool have = false;
    uint64_t req = 0;     // the HAND_PUSH in flight
    double req_ms = 0;
};
HandTrack g_hand[2];
struct Grab {
    int st = 0;            // 0 none, 1 asked, 2 held, 3 let go, 4 its collisions with the player asked back (0.5 s after)
    uint32_t h = 0;
    float off[3] = {};     // the prop from the palm, in the hand's target axes
    uint64_t req = 0;      // the request in flight (one a hand)
    double req_ms = 0, end_ms = 0;
    bool grip = false;     // the grip as last seen (its edges)
};
Grab g_grab[2];
uint64_t g_grabs = 0, g_throws = 0, g_lost = 0, g_grab_none = 0;
std::atomic<bool> g_grab_ready[2] = {false, false};  // Grab on and the hand empty (the last frame end): controls holds a press back
std::atomic<int> g_grab_st[2] = {0, 0};
std::atomic<uint32_t> g_grab_miss[2] = {0, 0};      // asks that found no prop (or no answer)
struct Touched {
    uint32_t h;
    double ms;  // the last push (the cooldown)
};
std::vector<Touched> g_touched;  // the distinct props touched this session (each pins a script reference node)
uint64_t g_count[8] = {};        // by outcome (RdrvrPush)
int g_pool_free = -1;
double g_pool_ms = 0;
bool g_stopped = false;          // MaxTouched reached, or the reference pool low: no more LOCATE
char g_last[200] = "";
uint64_t g_probe = 0;            // "physics probe": a request whose result goes to the status line

void grab_frame(const RdrvrActorState& st, const body::BodyPoints& bp, double now);

void frame() {
    const double now = log::now_ms();
    std::lock_guard lock(g_mutex);
    // results first (a request can be answered on the next script tick)
    for (int c = 0; c < 2; ++c) {
        HandTrack& t = g_hand[c];
        RdrvrNativeResult r{};
        if (t.req && api::wait_native(t.req, &r, 0)) {
            t.req = 0;
            const uint32_t h = static_cast<uint32_t>(r.value), code = static_cast<uint32_t>(r.value >> 32);
            ++g_count[code < 8 ? code : 0];
            if (h) {
                bool seen = false;
                for (Touched& x : g_touched)
                    if (x.h == h) {
                        seen = true;
                        if (code == RDRVR_PUSH_OK) x.ms = now;
                    }
                if (!seen) g_touched.push_back({h, code == RDRVR_PUSH_OK ? now : 0.0});
            }
            if (code == RDRVR_PUSH_OK) {
                std::snprintf(g_last, sizeof(g_last), "the %s hand pushed prop 0x%x: (%.2f %.2f %.2f) m/s", c ? "right" : "left", h, r.vec[0], r.vec[1],
                              r.vec[2]);
                log::info("[physics] %s", g_last);
            }
        } else if (t.req && now - t.req_ms > 1000.0) {
            t.req = 0;  // no script tick answered (paused, loading)
        }
    }
    // [Physics] Grab: the answers (a grab, a move, a release, the collisions back)
    for (int c = 0; c < 2; ++c) {
        Grab& g = g_grab[c];
        RdrvrNativeResult r{};
        if (!g.req) continue;
        if (!api::wait_native(g.req, &r, 0)) {
            if (now - g.req_ms > 1000.0) {  // no tick answered (paused, loading)
                g.req = 0;
                if (g.st == 4) g = Grab{};
                if (g.st == 1) {
                    g.st = 0;
                    g_grab_miss[c].fetch_add(1, std::memory_order_relaxed);
                }
            }
            continue;
        }
        g.req = 0;
        if (g.st == 1) {
            const uint32_t h = static_cast<uint32_t>(r.value), code = static_cast<uint32_t>(r.value >> 32);
            if (h) {
                bool seen = false;
                for (const Touched& x : g_touched) seen = seen || x.h == h;
                if (!seen) g_touched.push_back({h, now});  // a LOCATE: one script reference for the session
            }
            if (code == RDRVR_PUSH_OK && h) {
                g.h = h;
                g.st = 2;
                ++g_grabs;
                body::BodyPoints bp;
                if (body::body_points(&bp)) {  // the prop's offset from the palm in the hand's axes, kept while held
                    const int jh = bp.ctrl[0] == c ? 0 : 1;
                    const float* R = bp.target_rot[jh];
                    float d[3];
                    for (int k = 0; k < 3; ++k) d[k] = r.vec[k] - (bp.hand[jh][k] - R[k * 3 + 2] * 0.08f);
                    for (int i = 0; i < 3; ++i) g.off[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2];
                }
                controllers::pulse(c, 0.5f, 25);
                std::snprintf(g_last, sizeof(g_last), "the %s hand grabbed prop 0x%x at (%.2f %.2f %.2f)", c ? "right" : "left", h, r.vec[0], r.vec[1],
                              r.vec[2]);
                log::info("[physics] %s", g_last);
            } else {
                g.st = 0;
                ++g_grab_none;
                g_grab_miss[c].fetch_add(1, std::memory_order_relaxed);
            }
        } else if (g.st == 4) {  // its collisions back: done with it
            g = Grab{};
        } else if (g.st == 2 && !(r.value & 0xff)) {  // a move found it gone (removed by the game)
            log::info("[physics] the held prop 0x%x is gone", g.h);
            g = Grab{};
            ++g_lost;
        }
    }
    if (g_probe) {
        RdrvrNativeResult r{};
        if (api::wait_native(g_probe, &r, 0)) {
            g_probe = 0;
            std::snprintf(g_last, sizeof(g_last), "probe: prop 0x%x, outcome %u, at (%.2f %.2f %.2f)", static_cast<uint32_t>(r.value),
                          static_cast<uint32_t>(r.value >> 32), r.vec[0], r.vec[1], r.vec[2]);
            log::info("[physics] %s", g_last);
        }
    }
    RdrvrActorState st{};
    body::BodyPoints bp;
    const bool grab_on = g_grab_on.load(std::memory_order_relaxed);
    const bool live = pose::anchor_active() && api::actor_state(&st) && st.actor && body::body_points(&bp);
    for (int c = 0; c < 2; ++c) {  // a held prop let go, unfrozen, when grabbing or first person ends (never left frozen)
        Grab& g = g_grab[c];
        if (g.st == 2 && (!grab_on || !live) && !g.req) {
            const uint64_t args[3] = {g.h, static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), 0};
            const float zero[3] = {0, 0, 0};
            g.req = api::queue_op(RDRVR_NATIVE_GRAB_END, args, 3, zero);
            g.req_ms = g.end_ms = now;
            g.st = 3;
            log::info("[physics] the held prop 0x%x let go (%s)", g.h, grab_on ? "first person ended" : "Grab turned off");
        }
        if (g.st == 3 && !g.req && now - g.end_ms > 500.0) {  // its collisions with the player back
            if (st.actor) {
                const uint64_t args[3] = {g.h, static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), 1};
                const float zero[3] = {0, 0, 0};
                g.req = api::queue_op(RDRVR_NATIVE_GRAB_END, args, 3, zero);
                g.req_ms = now;
                g.st = 4;
            } else {
                g = Grab{};
            }
        }
    }
    for (int c = 0; c < 2; ++c) g_grab_st[c].store(g_grab[c].st, std::memory_order_relaxed);
    if (!grab_on || !live) g_grab_ready[0] = g_grab_ready[1] = false;
    if (!live || menu::visible() || (!grab_on && !g_on.load(std::memory_order_relaxed))) {
        for (HandTrack& t : g_hand) t.have = false;
        return;
    }
    // the script reference pool (read only, once a second): stop LOCATE before it runs low (research 2.3)
    if (now - g_pool_ms > 1000.0) {
        g_pool_ms = now;
        uintptr_t pool = 0;
        int32_t free_nodes = -1;
        if (raw(anchors::addr(anchors::Id::ScriptRefPool), &pool, sizeof(pool)) && pool && raw(pool + 0x20, &free_nodes, sizeof(free_nodes)))
            g_pool_free = free_nodes;
        const bool stop = static_cast<int>(g_touched.size()) >= g_max_touched || (g_pool_free >= 0 && g_pool_free < g_pool_floor);
        if (stop && !g_stopped) log::warn("[physics] pushes stopped: %zu props touched, %d free script references", g_touched.size(), g_pool_free);
        g_stopped = stop;
    }
    if (grab_on) grab_frame(st, bp, now);
    for (int c = 0; c < 2; ++c) g_grab_st[c].store(g_grab[c].st, std::memory_order_relaxed);
    if (!g_on.load(std::memory_order_relaxed)) {
        for (HandTrack& t : g_hand) t.have = false;
        return;
    }
    const dual::State ds = dual::state();
    const int gun_h = controls::gun_hand();
    for (int c = 0; c < 2; ++c) {
        HandTrack& t = g_hand[c];
        const int jh = bp.ctrl[0] == c ? 0 : 1;
        if (!bp.hand_ok[jh]) {
            t.have = false;
            continue;
        }
        // the palm: 8 cm out of the wrist target along the hand (its -z)
        const float* R = bp.target_rot[jh];
        float palm[3];
        for (int k = 0; k < 3; ++k) palm[k] = bp.hand[jh][k] - R[k * 3 + 2] * 0.08f;
        float v[3] = {0, 0, 0};
        const double dt = t.have ? (now - t.ms) * 0.001 : 0.0;
        if (dt > 0.004 && dt < 0.2)
            for (int k = 0; k < 3; ++k) v[k] = static_cast<float>((palm[k] - t.p[k]) / dt);
        std::memcpy(t.p, palm, sizeof(palm));
        t.ms = now;
        t.have = true;
        const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        // an empty hand only: not the gun hand with anything in it, not the second gun's, not at a holster or the foregrip
        const bool busy = (c == gun_h && st.weapon >= 0) || (ds.on && ds.ctrl == c) || holster::grip_wanted(c) || holster::grip_consumed(c) ||
                          g_grab[c].st != 0;  // a hand holding a prop pushes nothing
        if (busy || g_stopped || speed < g_min_speed || speed > 15.0f || t.req || now - t.req_ms < g_cooldown_ms) continue;
        const uint64_t args[7] = {f32(palm[0]) | (f32(palm[1]) << 32), f32(palm[2]), f32(g_radius), f32(g_gain), f32(g_max),
                                  static_cast<uint64_t>(anchors::addr(anchors::Id::ScriptThreadCurrent)), 0};
        // the requests spaced by PushCooldownMs a hand; a prop already moving along the hand as fast is left (ALREADY)
        t.req = api::queue_op(RDRVR_NATIVE_HAND_PUSH, args, 7, v);
        t.req_ms = now;
    }
}

// [Physics] Grab, each frame (the frame end, under g_mutex): the grip's edges per controller; a rising edge with an empty
// hand that no holster or foregrip wants asks for the nearest loose prop at the palm; while held it follows the palm
// (one move in flight a hand); the falling edge lets it go with the palm's velocity (capped); a gun drawn into the hand
// drops it
void grab_frame(const RdrvrActorState& st, const body::BodyPoints& bp, double now) {
    const dual::State ds = dual::state();
    const int gun_h = controls::gun_hand();
    static float last[2][3];
    static double last_ms[2] = {0, 0};
    for (int c = 0; c < 2; ++c) {
        Grab& g = g_grab[c];
        const int jh = bp.ctrl[0] == c ? 0 : 1;
        if (!bp.hand_ok[jh]) continue;
        const float* R = bp.target_rot[jh];
        float palm[3];
        for (int k = 0; k < 3; ++k) palm[k] = bp.hand[jh][k] - R[k * 3 + 2] * 0.08f;
        float v[3] = {0, 0, 0};
        const double dt = last_ms[c] > 0 ? (now - last_ms[c]) * 0.001 : 0.0;
        if (dt > 0.004 && dt < 0.2)
            for (int k = 0; k < 3; ++k) v[k] = static_cast<float>((palm[k] - last[c][k]) / dt);
        std::memcpy(last[c], palm, sizeof(palm));
        last_ms[c] = now;
        const float gr = hands::get(c).grip;
        const bool down = gr > 0.7f ? true : gr < 0.5f ? false : g.grip;
        const bool rise = down && !g.grip, fall = !down && g.grip;
        g.grip = down;
        const bool busy = (c == gun_h && st.weapon >= 0) || (ds.on && ds.ctrl == c) || holster::grip_wanted(c) || holster::grip_consumed(c);
        g_grab_ready[c].store(!busy && !g_stopped, std::memory_order_relaxed);
        if (g.st == 0 && rise && (busy || g_stopped)) g_grab_miss[c].fetch_add(1, std::memory_order_relaxed);  // not asked: the game's
        if (g.st == 0 && rise && !busy && !g_stopped && !g.req) {
            const uint64_t args[8] = {f32(palm[0]) | (f32(palm[1]) << 32), f32(palm[2]), f32(g_grab_radius), 0, 0,
                                      static_cast<uint64_t>(anchors::addr(anchors::Id::ScriptThreadCurrent)), 0,
                                      static_cast<uint64_t>(static_cast<uint32_t>(st.actor))};
            const float zero[3] = {0, 0, 0};
            g.req = api::queue_op(RDRVR_NATIVE_GRAB, args, 8, zero);
            g.req_ms = now;
            g.st = 1;
        } else if (g.st == 2 && (fall || busy)) {  // let go: thrown with the palm (a gun drawn into it: dropped)
            if (g.req) continue;  // a move in flight: let go next frame
            float tv[3] = {0, 0, 0};
            if (fall && !busy) {
                for (int k = 0; k < 3; ++k) tv[k] = v[k] * g_throw_gain;
                const float l = std::sqrt(tv[0] * tv[0] + tv[1] * tv[1] + tv[2] * tv[2]);
                if (l > g_throw_max)
                    for (float& x : tv) x *= g_throw_max / l;
            }
            const uint64_t args[3] = {g.h, static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), 0};
            g.req = api::queue_op(RDRVR_NATIVE_GRAB_END, args, 3, tv);
            g.req_ms = g.end_ms = now;
            g.st = 3;
            ++g_throws;
            controllers::pulse(c, 0.3f, 15);
            std::snprintf(g_last, sizeof(g_last), "the %s hand let prop 0x%x go at (%.1f %.1f %.1f) m/s", c ? "right" : "left", g.h, tv[0], tv[1], tv[2]);
            log::info("[physics] %s", g_last);
        } else if (g.st == 2 && !g.req) {  // held: at the palm, its offset in the hand's axes
            float p[3];
            for (int k = 0; k < 3; ++k) p[k] = palm[k] + R[k * 3] * g.off[0] + R[k * 3 + 1] * g.off[1] + R[k * 3 + 2] * g.off[2];
            const uint64_t args[3] = {g.h, f32(p[0]) | (f32(p[1]) << 32), f32(p[2])};
            g.req = api::queue_op(RDRVR_NATIVE_GRAB_MOVE, args, 3, nullptr);
            g.req_ms = now;
        }
    }
}

}  // namespace

void init() {
    g_on = config::get_bool("Physics", "HandCollision", false);
    g_gain = config::get_float("Physics", "PushGain", 1.2f);
    g_max = config::get_float("Physics", "PushMax", 5.0f);
    g_min_speed = config::get_float("Physics", "PushMinSpeed", 0.5f);
    g_radius = config::get_float("Physics", "PushRadius", 0.35f);
    g_cooldown_ms = config::get_float("Physics", "PushCooldownMs", 120.0f);
    g_max_touched = static_cast<int>(config::get_float("Physics", "MaxTouched", 96.0f));
    g_pool_floor = static_cast<int>(config::get_float("Physics", "RefPoolFloor", 256.0f));
    g_grab_on = config::get_bool("Physics", "Grab", false);
    g_grab_radius = config::get_float("Physics", "GrabRadius", 0.15f);
    g_throw_gain = config::get_float("Physics", "ThrowGain", 1.0f);
    g_throw_max = config::get_float("Physics", "ThrowMax", 12.0f);
    log::info("[physics] grabbing loose props %d (radius %.2f m, thrown at %.1f x the hand, up to %.1f m/s)", g_grab_on.load() ? 1 : 0,
              g_grab_radius, g_throw_gain, g_throw_max);
    log::info("[physics] hands push loose props %d (gain %.1f, up to %.1f m/s, from %.1f m/s, radius %.2f m; at most %d props, %d free references)",
              g_on.load() ? 1 : 0, g_gain, g_max, g_min_speed, g_radius, g_max_touched, g_pool_floor);
    d3d::add_frame_end_listener([](uint64_t) { frame(); });
}

bool enabled() { return g_on.load(); }
bool grab_enabled() { return g_grab_on.load(); }
void set_grab_enabled(bool on) {
    if (g_grab_on.exchange(on) != on) log::info("[physics] grabbing loose props %d", on ? 1 : 0);
    config::set("Physics", "Grab", on ? "1" : "0");
}
bool grip_wanted(int ctrl) { return ctrl >= 0 && ctrl < 2 && g_grab_on.load(std::memory_order_relaxed) && g_grab_ready[ctrl].load(std::memory_order_relaxed); }
bool grab_held(int ctrl) { return ctrl >= 0 && ctrl < 2 && g_grab_st[ctrl].load(std::memory_order_relaxed) >= 2; }
uint32_t grab_misses(int ctrl) { return ctrl >= 0 && ctrl < 2 ? g_grab_miss[ctrl].load(std::memory_order_relaxed) : 0; }
bool holding(int ctrl) {
    std::lock_guard lock(g_mutex);
    return ctrl >= 0 && ctrl < 2 && g_grab[ctrl].st != 0;
}
void set_enabled(bool on) {
    if (g_on.exchange(on) != on) log::info("[physics] hands push loose props %d", on ? 1 : 0);
    config::set("Physics", "HandCollision", on ? "1" : "0");
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    while (in >> w) {
        if (w == "on" || w == "off") g_on = w == "on";  // the session only
        if (w == "grab") {  // grab on|off: [Physics] Grab for the session
            std::string v;
            in >> v;
            g_grab_on = v != "off";
            continue;
        }
        if (w == "probe") {  // physics probe <radius> [part 0-6]: the nearest prop to the right hand (no push: zero velocity)
            float r = 2.0f;
            int part = 0;
            in >> r >> part;
            body::BodyPoints bp;
            if (body::body_points(&bp)) {
                const int jh = bp.ctrl[0] == 1 ? 0 : 1;
                const float* p = bp.hand[jh];
                const uint64_t args[7] = {f32(p[0]) | (f32(p[1]) << 32), f32(p[2]), f32(r), f32(0.0f), f32(0.0f),
                                          static_cast<uint64_t>(anchors::addr(anchors::Id::ScriptThreadCurrent)),
                                          static_cast<uint64_t>(part < 0 ? 0 : part)};
                const float zero[3] = {0, 0, 0};
                std::lock_guard lock(g_mutex);
                g_probe = api::queue_op(RDRVR_NATIVE_HAND_PUSH, args, 7, zero);
            }
        }
    }
    char b[480];
    std::lock_guard lock(g_mutex);
    std::snprintf(b, sizeof(b),
                  "physics %d | pushes ok %llu failed %llu | none %llu actor %llu fixed %llu behind %llu already %llu guard %llu | props touched %zu "
                  "(stopped %d, free references %d) | grab %d: grabs %llu throws %llu lost %llu none %llu, held L %d R %d | last: %s",
                  g_on.load() ? 1 : 0, static_cast<unsigned long long>(g_count[RDRVR_PUSH_OK]), static_cast<unsigned long long>(g_count[RDRVR_PUSH_FAILED]),
                  static_cast<unsigned long long>(g_count[RDRVR_PUSH_NONE]), static_cast<unsigned long long>(g_count[RDRVR_PUSH_ACTOR]),
                  static_cast<unsigned long long>(g_count[RDRVR_PUSH_FIXED]), static_cast<unsigned long long>(g_count[RDRVR_PUSH_BEHIND]),
                  static_cast<unsigned long long>(g_count[RDRVR_PUSH_ALREADY]), static_cast<unsigned long long>(g_count[RDRVR_PUSH_GUARD]),
                  g_touched.size(), g_stopped ? 1 : 0, g_pool_free, g_grab_on.load() ? 1 : 0, static_cast<unsigned long long>(g_grabs),
                  static_cast<unsigned long long>(g_throws), static_cast<unsigned long long>(g_lost), static_cast<unsigned long long>(g_grab_none),
                  g_grab[0].st, g_grab[1].st, g_last);
    return b;
}

}  // namespace rdrvr::physics
