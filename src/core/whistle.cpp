#include "core/whistle.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "common/rdrvr_api.h"
#include "core/api.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/dual.h"
#include "core/hands.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/reload.h"
#include "core/xr.h"

namespace rdrvr::whistle {
namespace {

constexpr double kPressMs = 200.0;  // the whistle's button held (the game reads presses, not holds)
std::atomic<bool> g_on{false}, g_gun_hand{false};
std::mutex g_mutex;  // the offset and radius (the menu, the test channel) against the poll
float g_off[3] = {0.0f, -0.08f, 0.08f};  // right, up, forward of the eyes' midpoint (m)
float g_radius = 0.10f;
std::atomic<bool> g_supp[2] = {false, false}, g_mouth[2] = {false, false};
bool g_trig_was[2] = {false, false};
double g_press_until = 0.0;
std::atomic<uint64_t> g_whistles{0}, g_polls_at_mouth{0}, g_suppressed_pulls{0};
std::atomic<float> g_last_d[2] = {-1.0f, -1.0f};  // each hand's distance from the mouth point at the last poll (m)

// q (x y z w) applied to v
void rotate(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// the mouth point in LOCAL (the eyes' midpoint, the offset turned with the head: -z forward, +y up, +x right)
bool mouth(float out[3]) {
    xr::EyeView v[2];
    if (!xr::eye_views_peek(v)) return false;  // a peek: not a scene pass's hand-out (the pose check)
    float off[3];
    {
        std::lock_guard lock(g_mutex);
        off[0] = g_off[0], off[1] = g_off[1], off[2] = -g_off[2];
    }
    float r[3];
    rotate(v[0].orientation, off, r);
    for (int k = 0; k < 3; ++k) out[k] = 0.5f * (v[0].position[k] + v[1].position[k]) + r[k];
    return true;
}

}  // namespace

void init() {
    g_on = config::get_bool("Gestures", "Whistle", false);
    g_gun_hand = config::get_bool("Gestures", "WhistleGunHand", false);
    float o[3] = {0.0f, -0.08f, 0.08f};
    {
        const std::string s = config::get_string("Gestures", "WhistleOffset", "0.000 -0.080 0.080");
        float a = 0, b = 0, c = 0;
        if (sscanf_s(s.c_str(), "%f %f %f", &a, &b, &c) == 3 && std::fabs(a) < 0.5f && std::fabs(b) < 0.5f && std::fabs(c) < 0.5f) o[0] = a, o[1] = b, o[2] = c;
    }
    float r = config::get_float("Gestures", "WhistleRadius", 0.10f);
    r = !(r >= 0.03f) ? 0.03f : r > 0.30f ? 0.30f : r;
    {
        std::lock_guard lock(g_mutex);
        std::memcpy(g_off, o, sizeof(o));
        g_radius = r;
    }
    log::info("[whistle] the horse whistle by the hand at the mouth %d: the point (%.3f %.3f %.3f) right/up/forward of the eyes, radius %.2f m, "
              "the gun hand too %d (the game's whistle: D-pad up)",
              g_on.load() ? 1 : 0, o[0], o[1], o[2], r, g_gun_hand.load() ? 1 : 0);
}

bool poll(double now, float trig[2]) {
    if (!g_on.load(std::memory_order_relaxed)) {
        g_supp[0] = g_supp[1] = g_mouth[0] = g_mouth[1] = false;
        return false;
    }
    float m[3];
    const bool have = mouth(m);
    RdrvrActorState st{};
    const bool gun = api::actor_state(&st) && reload::is_gun(st.weapon);
    const int gun_h = controls::gun_hand();
    const dual::State ds = dual::state();
    float r;
    {
        std::lock_guard lock(g_mutex);
        r = g_radius;
    }
    for (int h = 0; h < 2; ++h) {
        const hands::Hand hd = hands::get(h);
        const float* p = hd.grip_valid ? hd.grip_pos : hd.pos;
        // the gun hand (or the second gun's) with a gun in it: no whistle unless WhistleGunHand
        const bool armed = (gun && h == gun_h) || (ds.on && ds.ctrl == h);
        bool at_pt = false;
        if (have && (hd.grip_valid || hd.valid)) {  // the distance for every hand (the readback); at_pt only where it may whistle
            const float d = std::sqrt((p[0] - m[0]) * (p[0] - m[0]) + (p[1] - m[1]) * (p[1] - m[1]) + (p[2] - m[2]) * (p[2] - m[2]));
            g_last_d[h].store(d, std::memory_order_relaxed);
            at_pt = d < r && (!armed || g_gun_hand.load(std::memory_order_relaxed)) && !menu::visible();
        } else {
            g_last_d[h].store(-1.0f, std::memory_order_relaxed);
        }
        g_mouth[h].store(at_pt, std::memory_order_relaxed);
        if (at_pt) g_polls_at_mouth.fetch_add(1, std::memory_order_relaxed);
        const bool down = trig[h] > 0.5f, was = g_trig_was[h];
        g_trig_was[h] = down;
        if (at_pt && down && !was) {  // the press at the mouth: the whistle
            g_press_until = now + kPressMs;
            g_whistles.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(h, 0.3f, 25);
            log::info("[whistle] the %s hand at the mouth (%.2f m from the point), the trigger: the horse whistled", h ? "right" : "left",
                      g_last_d[h].load());
        }
        // the trigger is the whistle's while the hand is at the mouth, and after a press there until it is let go
        bool s = g_supp[h].load(std::memory_order_relaxed);
        if (at_pt) s = true;
        else if (trig[h] < 0.15f) s = false;
        g_supp[h].store(s, std::memory_order_relaxed);
        if (s) {
            if (down && !was) g_suppressed_pulls.fetch_add(1, std::memory_order_relaxed);
            trig[h] = 0.0f;
        }
    }
    return now < g_press_until;
}

bool suppressed(int ctrl) { return ctrl >= 0 && ctrl < 2 && g_supp[ctrl].load(std::memory_order_relaxed); }
bool at_mouth(int ctrl) { return ctrl >= 0 && ctrl < 2 && g_mouth[ctrl].load(std::memory_order_relaxed); }
bool enabled() { return g_on.load(std::memory_order_relaxed); }
void set_enabled(bool on, bool save) {
    if (g_on.exchange(on) != on) log::info("[whistle] the horse whistle by gesture %s (%s)", on ? "on" : "off", save ? "saved" : "the session");
    if (save) config::set("Gestures", "Whistle", on ? "1" : "0");
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    while (in >> w) {
        if (w == "on" || w == "off") set_enabled(w == "on", false);
        if (w == "offset") {
            float o[3];
            if (in >> o[0] >> o[1] >> o[2]) {
                std::lock_guard lock(g_mutex);
                std::memcpy(g_off, o, sizeof(o));
            }
        }
        if (w == "radius") {
            float r;
            if (in >> r && r >= 0.03f && r <= 0.30f) {
                std::lock_guard lock(g_mutex);
                g_radius = r;
            }
        }
        if (w == "gunhand") {
            std::string v;
            in >> v;
            g_gun_hand = v == "on";
        }
        if (w == "reset") g_whistles = g_polls_at_mouth = g_suppressed_pulls = 0;
    }
    float m[3] = {};
    const bool have = mouth(m);
    float o[3], r;
    {
        std::lock_guard lock(g_mutex);
        std::memcpy(o, g_off, sizeof(o));
        r = g_radius;
    }
    char b[480];
    std::snprintf(b, sizeof(b),
                  "whistle %d (the gun hand too %d) | the point (%.3f %.3f %.3f) radius %.2f, in LOCAL %s (%.3f %.3f %.3f) | the hands from it: left %.3f "
                  "right %.3f m, at the mouth %d %d, the trigger the whistle's %d %d | whistles %llu, polls at the mouth %llu, pulls kept from the game %llu",
                  g_on.load() ? 1 : 0, g_gun_hand.load() ? 1 : 0, o[0], o[1], o[2], r, have ? "" : "(no views)", m[0], m[1], m[2], g_last_d[0].load(),
                  g_last_d[1].load(), g_mouth[0].load() ? 1 : 0, g_mouth[1].load() ? 1 : 0, g_supp[0].load() ? 1 : 0, g_supp[1].load() ? 1 : 0,
                  static_cast<unsigned long long>(g_whistles.load()), static_cast<unsigned long long>(g_polls_at_mouth.load()),
                  static_cast<unsigned long long>(g_suppressed_pulls.load()));
    return b;
}

}  // namespace rdrvr::whistle
