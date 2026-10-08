#include "core/wheel.h"

#include <windows.h>
#include <xinput.h>

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
#include "core/hands.h"
#include "core/log.h"
#include "core/xinput.h"
#include "core/xr.h"

namespace rdrvr::wheel {
namespace {

constexpr double kMinHoldMs = 300.0;   // the game opens the wheel after 0.2 s of LB; a shorter press is its holster tap
constexpr double kStickTailMs = 150.0; // the stick kept after LB is let go (the game equips on the release)
constexpr uint32_t kRefreshMs = 250;   // the override's life, refreshed each poll (a stalled poll lets it go)
std::atomic<bool> g_mode{false};
std::atomic<float> g_dead{0.04f}, g_angle{0.0f};
std::atomic<bool> g_flip_y{false};
std::atomic<int> g_grips{3};  // bit 0 the left controller's grip, bit 1 the right's

int g_ctrl = -1;              // the controller holding the wheel open (-1: closed)
double g_open_ms = 0.0, g_release_ms = 0.0;
bool g_releasing = false;     // the grip let go before the minimum hold: LB held to it, then a cancel
float g_centre[3] = {};       // the hand at the press (LOCAL)
float g_right[3] = {1, 0, 0}, g_up[3] = {0, 1, 0};  // the head's axes at the press (LOCAL)
float g_head_q[4] = {0, 0, 0, 1};                   // the head's turn at the press (the hand quad faces it)
std::atomic<bool> g_at_hand{true};                  // [Holsters] WheelAtHand
float g_rect[4] = {0.33f, 0.04f, 0.71f, 0.74f};     // [Holsters] WheelRect (set at init only)
std::mutex g_quad_mutex;                            // the press's centre and turn: the pad's poll writes, the XR frame reads
float g_stick[2] = {0, 0};    // the stick now (-1..1)
int g_sector = -1;            // 0..7 (45-degree sectors from the right, counter-clockwise), -1 the dead zone
std::atomic<bool> g_open{false};
std::atomic<uint64_t> g_opens{0}, g_commits{0}, g_cancels{0};
std::atomic<int> g_last_sector{-1};
std::atomic<int> g_ctrl_pub{-1}, g_sector_pub{-1};   // the poll's state for the status (another thread)
std::atomic<float> g_stick_pub[2] = {0.0f, 0.0f};
bool g_free_was[2] = {false, false};                // a press opens the wheel, not a grip still held
int g_pend_ctrl = -1, g_pend_weapon = -1;            // a commit's hand, until the weapon in hand changes (or 1.5 s)
double g_pend_until = 0.0;
void publish() {
    g_ctrl_pub.store(g_ctrl, std::memory_order_relaxed);
    g_sector_pub.store(g_sector, std::memory_order_relaxed);
    g_stick_pub[0].store(g_stick[0], std::memory_order_relaxed);
    g_stick_pub[1].store(g_stick[1], std::memory_order_relaxed);
}
std::atomic<float> g_last_off{0.0f};

void rotate(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

bool hand_pos(int c, float out[3]) {
    const hands::Hand hd = hands::get(c);
    if (!(hd.grip_valid || hd.valid)) return false;
    std::memcpy(out, hd.grip_valid ? hd.grip_pos : hd.pos, 12);
    return true;
}

void begin(int c, double now) {
    float p[3];
    if (!hand_pos(c, p)) return;
    xr::EyeView v[2];
    if (xr::eye_views_peek(v)) {  // a peek: not a scene pass's hand-out
        const float rx[3] = {1, 0, 0}, uy[3] = {0, 1, 0};
        rotate(v[0].orientation, rx, g_right);
        rotate(v[0].orientation, uy, g_up);
        std::lock_guard lock(g_quad_mutex);
        std::memcpy(g_head_q, v[0].orientation, sizeof(g_head_q));
    } else {
        g_right[0] = 1, g_right[1] = g_right[2] = 0;
        g_up[1] = 1, g_up[0] = g_up[2] = 0;
    }
    {
        std::lock_guard lock(g_quad_mutex);
        std::memcpy(g_centre, p, sizeof(p));
    }
    g_ctrl = c;
    g_open_ms = now;
    g_releasing = false;
    g_stick[0] = g_stick[1] = 0.0f;
    g_sector = -1;
    g_open = true;
    g_opens.fetch_add(1, std::memory_order_relaxed);
    controllers::pulse(c, 0.2f, 15);
    log::info("[wheel] the %s grip: the weapon wheel (LB held), the centre at the hand", c ? "right" : "left");
}

void steer() {
    float p[3];
    if (!hand_pos(g_ctrl, p)) return;
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = p[k] - g_centre[k];
    const float x = d[0] * g_right[0] + d[1] * g_right[1] + d[2] * g_right[2];
    const float y = d[0] * g_up[0] + d[1] * g_up[1] + d[2] * g_up[2];
    const float r = std::sqrt(x * x + y * y);
    g_last_off.store(r, std::memory_order_relaxed);
    if (r < g_dead.load(std::memory_order_relaxed)) {
        g_stick[0] = g_stick[1] = 0.0f;
        g_sector = -1;
        return;
    }
    const float a = std::atan2(y, x) + g_angle.load(std::memory_order_relaxed) * 0.0174532925f;
    g_stick[0] = std::cos(a);
    g_stick[1] = (g_flip_y.load(std::memory_order_relaxed) ? -1.0f : 1.0f) * std::sin(a);
    float deg = std::atan2(g_stick[1], g_stick[0]) * 57.29578f;  // the stick sent (WheelAngle, WheelFlipY): the game's sector
    if (deg < 0) deg += 360.0f;
    const int s = static_cast<int>((deg + 22.5f) / 45.0f) % 8;
    if (s != g_sector) {
        g_sector = s;
        controllers::pulse(g_ctrl, 0.12f, 10);  // a tick at each new sector
    }
}

}  // namespace

void init() {
    const std::string m = config::get_string("Holsters", "Mode", "holsters");
    g_mode = m == "wheel";
    float dz = config::get_float("Holsters", "WheelDeadZone", 0.04f);
    g_dead = !(dz >= 0.01f) ? 0.01f : dz > 0.15f ? 0.15f : dz;
    float an = config::get_float("Holsters", "WheelAngle", 0.0f);
    g_angle = !(an >= -180.0f) ? 0.0f : an > 180.0f ? 180.0f : an;
    g_flip_y = config::get_bool("Holsters", "WheelFlipY", false);
    g_at_hand = config::get_bool("Holsters", "WheelAtHand", true);
    {
        const std::string r = config::get_string("Holsters", "WheelRect", "0.33 0.04 0.71 0.74");
        float a = 0, b = 0, c = 0, d = 0;
        if (sscanf_s(r.c_str(), "%f %f %f %f", &a, &b, &c, &d) == 4 && a >= 0 && b >= 0 && c <= 1 && d <= 1 && c - a > 0.05f && d - b > 0.05f)
            g_rect[0] = a, g_rect[1] = b, g_rect[2] = c, g_rect[3] = d;
    }
    const std::string g = config::get_string("Holsters", "WheelGrips", "both");
    g_grips = g == "left" ? 1 : g == "right" ? 2 : 3;
    log::info("[wheel] the weapon wheel mode %d (Mode=%s): the grips %s, the dead zone %.2f m, the angle %+.0f deg, y %s", g_mode.load() ? 1 : 0,
              m.c_str(), g.c_str(), g_dead.load(), g_angle.load(), g_flip_y.load() ? "flipped" : "as the hand");
}

bool wheel_mode() { return g_mode.load(std::memory_order_relaxed); }
void set_wheel_mode(bool on, bool save) {
    if (g_mode.exchange(on) != on) log::info("[wheel] the mode: %s (%s)", on ? "the weapon wheel" : "the holsters", save ? "saved" : "the session");
    if (save) config::set("Holsters", "Mode", on ? "wheel" : "holsters");
}
bool open() { return g_open.load(std::memory_order_relaxed); }
bool hand_quad(float pos[3], float orient[4], float rect[4]) {
    if (!g_open.load(std::memory_order_relaxed) || !g_at_hand.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_quad_mutex);
    std::memcpy(pos, g_centre, 12);
    std::memcpy(orient, g_head_q, 16);
    std::memcpy(rect, g_rect, 16);
    return true;
}
void cancel() {
    if (g_ctrl < 0) return;
    xinput::set_override(0, false, 0, 0, 0);
    g_ctrl = -1;
    g_open = false;
    publish();
    g_cancels.fetch_add(1, std::memory_order_relaxed);
    log::info("[wheel] cancelled: the controllers' pad stopped (the menu, or not in use)");
}

void poll(double now, const bool free[2], bool taken[2]) {
    taken[0] = taken[1] = false;
    const bool pressed[2] = {free[0] && !g_free_was[0], free[1] && !g_free_was[1]};
    g_free_was[0] = free[0];
    g_free_was[1] = free[1];
    if (g_pend_ctrl >= 0) {  // a commit: the gun hand moves once the game has changed the weapon (the review's finding)
        RdrvrActorState st{};
        if (api::actor_state(&st) && st.weapon != g_pend_weapon) {
            controls::set_draw_hand(g_pend_ctrl, -1, true);
            g_pend_ctrl = -1;
        } else if (now > g_pend_until) {
            g_pend_ctrl = -1;
        }
    }
    if (!g_mode.load(std::memory_order_relaxed)) {
        if (g_ctrl >= 0) {  // switched off while open: let go at once
            xinput::set_override(0, false, 0, 0, 0);
            g_ctrl = -1;
            g_open = false;
            publish();
        }
        return;
    }
    const int grips = g_grips.load(std::memory_order_relaxed);
    if (g_ctrl < 0 && now >= g_release_ms) {
        for (int c = 0; c < 2; ++c)
            if (pressed[c] && (grips & (1 << c))) {
                begin(c, now);
                break;
            }
    }
    if (g_ctrl >= 0) {
        taken[g_ctrl] = true;
        const bool held = free[g_ctrl] && !g_releasing;
        if (held) {
            steer();
            xinput::set_override(XINPUT_GAMEPAD_LEFT_SHOULDER, true, g_stick[0], g_stick[1], kRefreshMs);
        } else if (now - g_open_ms < kMinHoldMs) {  // let go too soon: LB held to the minimum, then a cancel (no holster tap)
            g_releasing = true;
            g_sector = -1;
            xinput::set_override(XINPUT_GAMEPAD_LEFT_SHOULDER, true, 0, 0, kRefreshMs);
        } else {
            // LB let go: the stick kept (the game equips the highlight at the release), then let go too
            const bool commit = g_sector >= 0 && !g_releasing;
            xinput::set_override(0, true, commit ? g_stick[0] : 0.0f, commit ? g_stick[1] : 0.0f, static_cast<uint32_t>(kStickTailMs));
            g_last_sector.store(commit ? g_sector : -1, std::memory_order_relaxed);
            if (commit) {
                g_commits.fetch_add(1, std::memory_order_relaxed);
                RdrvrActorState st{};  // the new weapon into the gripping hand, once the game has changed it
                g_pend_ctrl = g_ctrl;
                g_pend_weapon = api::actor_state(&st) ? st.weapon : -2;
                g_pend_until = now + 1500.0;
                log::info("[wheel] let go: the sector %d (%d degrees from the hand's right), the weapon into the %s hand", g_sector, g_sector * 45,
                          g_ctrl ? "right" : "left");
            } else {
                g_cancels.fetch_add(1, std::memory_order_relaxed);
                log::info("[wheel] let go in the dead zone (or too soon): cancelled");
            }
            g_ctrl = -1;
            g_open = false;
            g_release_ms = now + kStickTailMs + 100.0;  // no new wheel until the stick is let go
        }
    }
    publish();
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    while (in >> w) {
        if (w == "on" || w == "off") set_wheel_mode(w == "on", false);
        if (w == "reset") g_opens = g_commits = g_cancels = 0;
        if (w == "angle") {
            float a;
            if (in >> a) g_angle = a;
        }
        if (w == "athand") {
            std::string v;
            in >> v;
            g_at_hand = v != "off";
        }
        if (w == "flipy") {
            std::string v;
            in >> v;
            g_flip_y = v == "on";
        }
    }
    char b[360];
    std::snprintf(b, sizeof(b),
                  "wheel mode %d, open %d (the %s grip), the stick (%.2f %.2f), sector %d, the hand %.3f m from the centre | opens %llu, commits %llu, "
                  "cancels %llu, the last sector %d | dead zone %.2f, angle %+.0f, flip y %d",
                  g_mode.load() ? 1 : 0, g_open.load() ? 1 : 0, g_ctrl_pub.load() == 1 ? "right" : g_ctrl_pub.load() == 0 ? "left" : "no",
                  g_stick_pub[0].load(), g_stick_pub[1].load(), g_sector_pub.load(),
                  g_last_off.load(), static_cast<unsigned long long>(g_opens.load()), static_cast<unsigned long long>(g_commits.load()),
                  static_cast<unsigned long long>(g_cancels.load()), g_last_sector.load(), g_dead.load(), g_angle.load(), g_flip_y.load() ? 1 : 0);
    return b;
}

}  // namespace rdrvr::wheel
