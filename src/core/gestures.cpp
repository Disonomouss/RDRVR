#include "core/gestures.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/aim.h"
#include "core/api.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/hands.h"
#include "core/log.h"
#include "core/pose.h"

namespace rdrvr::gestures {
namespace {

std::atomic<bool> g_throw{false}, g_melee{false}, g_lasso{false};
std::atomic<float> g_swing_speed{2.5f};  // [Gestures] SwingSpeed: m/s for a punch, a stab or the lasso's throw
std::atomic<float> g_throw_gain{2.0f};   // [Gestures] ThrowGain: the thrown weapon's speed over the hand's
// [Gestures] FistLeadMs (run 3: with bare fists RT alone does not punch; the game wants its fighting stance, LT, first):
// a fist's swing holds LT from the swing and presses RT this many ms later (the knife and the torch strike on RT alone)
std::atomic<int> g_fist_lead_ms{40};
constexpr float kYankSpeed = 1.8f;  // m/s back against where the hand points: the lasso's yank
constexpr float kMaxSpeed = 15.0f;  // m/s: faster steps are jumps, not hand motion
constexpr int kLassoWeapon = 21, kKnife = 22, kTorch = 33;

// each controller over the last frames in the recentred local frame (x right, y up, z back: the hand's own motion,
// so walking, turning, riding and the head-bone anchor are not hand motion); its latest drawn hand (world) and the game
// camera's back row (x, z), which takes a local velocity into the world (the anchor camera is level)
struct Sample {
    double ms;
    float p[3];
};
constexpr int kSamples = 32;
Sample g_hist[2][kSamples] = {};
int g_count[2] = {0, 0}, g_head[2] = {0, 0};
float g_world[2][3] = {}, g_back[2] = {0.0f, 1.0f};
std::mutex g_mutex;  // the history: the frame end writes, the projectile launch (the game thread) reads

std::atomic<uint64_t> g_punches{0}, g_lasso_throws{0}, g_yanks{0}, g_throws{0};
double g_last_swing_ms = 0;  // the last punch's swing (frame end only): the strike probe's reference
float g_last_speed[2] = {0, 0}, g_peak_speed[2] = {0, 0};  // frame end only (status)
float g_last_throw[3] = {0, 0, 0};
double g_last_throw_age = 0;  // ms between the hand's fastest moment and the release

// The fastest velocity of a hand (local frame) within the last `window_ms` (over two-frame steps), where its drawn
// hand is now (world) and the camera's back row.
bool velocity(int h, double now, double window_ms, float v[3], float* speed, float pos[3], float* back = nullptr,
              double* age_ms = nullptr) {
    std::lock_guard lock(g_mutex);
    const int n = g_count[h];
    if (n < 3) return false;
    *speed = 0;
    v[0] = v[1] = v[2] = 0;
    const Sample& last = g_hist[h][(g_head[h] + kSamples - 1) % kSamples];
    if (now - last.ms > 100.0) return false;  // no hand lately
    if (pos) std::memcpy(pos, g_world[h], sizeof(g_world[h]));
    if (back) std::memcpy(back, g_back, sizeof(g_back));
    for (int i = 0; i + 2 < n; ++i) {
        const Sample& a = g_hist[h][(g_head[h] + kSamples - 1 - i) % kSamples];
        const Sample& b = g_hist[h][(g_head[h] + kSamples - 3 - i) % kSamples];
        if (now - a.ms > window_ms) break;
        const double dt = (a.ms - b.ms) * 0.001;
        if (dt < 1e-3) continue;
        float w[3];
        for (int k = 0; k < 3; ++k) w[k] = static_cast<float>((a.p[k] - b.p[k]) / dt);
        const float s = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (s > kMaxSpeed) continue;  // a jump (tracking lost and found, a teleport, the recentre), not a swing
        if (s > *speed) {
            *speed = s;
            std::memcpy(v, w, sizeof(w));
            if (age_ms) *age_ms = now - a.ms;
        }
    }
    return true;
}

void frame() {
    static double next_ok[2] = {0, 0}, lasso_out_until = 0, yank_ok = 0;
    const double now = log::now_ms();
    aim::melee_probe(g_last_swing_ms);  // the punch's strike, timed (log only)
    body::BodyPoints bp;
    RdrvrActorState st{};
    if (!pose::anchor_active() || !body::body_points(&bp) || !api::actor_state(&st)) {
        std::lock_guard lock(g_mutex);
        g_count[0] = g_count[1] = 0;
        return;
    }
    float bx = 0.0f, bz = 1.0f;
    camera_lever::game_heading(&bx, &bz);
    const float right[3] = {bz, 0.0f, -bx}, back[3] = {bx, 0.0f, bz};  // the camera's rows a and c (world)
    {
        std::lock_guard lock(g_mutex);
        g_back[0] = bx;
        g_back[1] = bz;
        for (int h = 0; h < 2; ++h) {
            const int jh = bp.ctrl[0] == h ? 0 : 1;  // John's hand on the controller (body.cpp's mapping)
            const hands::Hand hd = hands::get(h);
            const float* lp = hd.grip_valid ? hd.grip_pos : hd.pos;
            Sample& s = g_hist[h][g_head[h]];
            if (!bp.hand_ok[jh] || !(hd.grip_valid || hd.valid) || !camera_lever::local_rel(lp, s.p)) {
                g_count[h] = 0;
                continue;
            }
            s.ms = now;
            std::memcpy(g_world[h], bp.hand[jh], sizeof(g_world[h]));
            g_head[h] = (g_head[h] + 1) % kSamples;
            if (g_count[h] < kSamples) ++g_count[h];
        }
    }
    // forward: where the head faces, in the local frame (positive yaw turns left)
    float hy = 0.0f;
    camera_lever::head_yaw_deg(&hy);
    const float fwd_l[3] = {-std::sin(hy * 0.0174532925f), 0.0f, -std::cos(hy * 0.0174532925f)};
    const bool melee = g_melee.load(std::memory_order_relaxed), lasso = g_lasso.load(std::memory_order_relaxed);
    // where the gun hand points (aim.cpp's hand ray: the IK target with the gun's barrel axis): the lasso is swung
    // that way and yanked back against it
    float point[3];  // in the local frame
    {
        float bt[3] = {0.0f, 0.174f, -0.985f}, w[3];
        aim::barrel_in_target(bt);
        const int gj = bp.gun;
        for (int k = 0; k < 3; ++k) w[k] = bp.target_rot[gj][k * 3] * bt[0] + bp.target_rot[gj][k * 3 + 1] * bt[1] + bp.target_rot[gj][k * 3 + 2] * bt[2];
        point[0] = w[0] * right[0] + w[2] * right[2];
        point[1] = w[1];
        point[2] = w[0] * back[0] + w[2] * back[2];
    }
    const int gun_h = controls::gun_hand();
    for (int h = 0; h < 2; ++h) {
        float v[3], s = 0;
        if (!velocity(h, now, 60.0, v, &s, nullptr)) continue;  // the latest step or two
        g_last_speed[h] = s;
        if (s > g_peak_speed[h]) g_peak_speed[h] = s;
        if (!melee && !lasso) continue;
        const float along = v[0] * point[0] + v[1] * point[1] + v[2] * point[2];
        // a punch or a stab: fists (nothing in hand), the knife or the torch; either hand, past the threshold and
        // forward (pulling the hand back after a punch is not another)
        const float fwd_speed = v[0] * fwd_l[0] + v[2] * fwd_l[2];
        if (melee && (st.weapon < 0 || st.weapon == kKnife || st.weapon == kTorch) && s > g_swing_speed.load(std::memory_order_relaxed) &&
            fwd_speed > 0.4f * s && now >= next_ok[h]) {
            next_ok[h] = now + 400.0;
            if (st.weapon < 0)
                controls::inject(0, 255, 0, 120, 255, g_fist_lead_ms.load(std::memory_order_relaxed));  // the fists: the stance first
            else
                controls::inject(0, 255, 0, 120);
            g_punches.fetch_add(1, std::memory_order_relaxed);
            g_last_swing_ms = now;
            log::info("[gestures] %s hand swing %.1f m/s: the attack (weapon %d)", h ? "right" : "left", s, st.weapon);
        }
        if (lasso && st.weapon == kLassoWeapon && h == gun_h) {
            // thrown by a forward swing; then a yank back steps John back on the rope for a moment
            if (s > g_swing_speed.load(std::memory_order_relaxed) && along > 0.5f * s && now >= next_ok[h]) {
                next_ok[h] = now + 800.0;
                lasso_out_until = now + 15000.0;
                controls::inject(0, 255, 0, 150);
                g_lasso_throws.fetch_add(1, std::memory_order_relaxed);
                log::info("[gestures] lasso thrown by a %.1f m/s swing", s);
            } else if (now < lasso_out_until && now > lasso_out_until - 15000.0 + 600.0 && along < -kYankSpeed && now >= yank_ok) {
                yank_ok = now + 600.0;
                controls::inject(0, 0, -32767, 350);
                g_yanks.fetch_add(1, std::memory_order_relaxed);
                log::info("[gestures] lasso yanked back (%.1f m/s)", -along);
            }
        }
    }
}

void save(const char* key, bool on) { config::set("Gestures", key, on ? "1" : "0"); }

}  // namespace

void init() {
    g_throw = config::get_bool("Gestures", "Throw", false);
    g_melee = config::get_bool("Gestures", "Melee", false);
    g_lasso = config::get_bool("Gestures", "Lasso", false);
    g_swing_speed = config::get_float("Gestures", "SwingSpeed", 2.5f);
    g_throw_gain = config::get_float("Gestures", "ThrowGain", 2.0f);
    g_fist_lead_ms = config::get_int("Gestures", "FistLeadMs", 40);
    d3d::add_frame_end_listener([](uint64_t) { frame(); });
    log::info("[gestures] throw by hand %d, melee by swing %d, lasso by hand %d (swing %.1f m/s, throw gain %.2f)", g_throw.load() ? 1 : 0,
              g_melee.load() ? 1 : 0, g_lasso.load() ? 1 : 0, g_swing_speed.load(), g_throw_gain.load());
}

bool throw_by_hand() { return g_throw.load(); }
void set_throw_by_hand(bool on) {
    if (g_throw.exchange(on) != on) log::info("[gestures] throw by hand: %s", on ? "on" : "off");
    save("Throw", on);
}
bool melee_by_swing() { return g_melee.load(); }
void set_melee_by_swing(bool on) {
    if (g_melee.exchange(on) != on) log::info("[gestures] melee by swing: %s", on ? "on" : "off");
    save("Melee", on);
}
bool lasso_by_hand() { return g_lasso.load(); }
void set_lasso_by_hand(bool on) {
    if (g_lasso.exchange(on) != on) log::info("[gestures] lasso by hand: %s", on ? "on" : "off");
    save("Lasso", on);
}

float swing_speed() { return g_swing_speed.load(); }
float throw_gain() { return g_throw_gain.load(); }
void set_tuning(float swing_speed, float throw_gain, bool save) {
    g_swing_speed = swing_speed < 1.0f ? 1.0f : swing_speed > 8.0f ? 8.0f : swing_speed;
    g_throw_gain = throw_gain < 0.5f ? 0.5f : throw_gain > 4.0f ? 4.0f : throw_gain;
    if (!save) return;
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", g_swing_speed.load());
    config::set("Gestures", "SwingSpeed", b);
    std::snprintf(b, sizeof(b), "%.2f", g_throw_gain.load());
    config::set("Gestures", "ThrowGain", b);
}

bool is_thrown(int32_t w) { return w == 23 || w == 24 || w == 25 || w == 29 || w == 32 || w == 35 || w == 36 || w == 37; }

bool throw_launch(float vel[3], float origin[3]) {
    if (!g_throw.load(std::memory_order_relaxed) || !pose::anchor_active()) return false;
    float v[3], s = 0, b[2] = {0.0f, 1.0f};
    double age = 0;
    // the game lets go some way into its throw clip after the trigger: the fastest moment of the last 0.8 s
    if (!velocity(controls::gun_hand(), log::now_ms(), 800.0, v, &s, origin, b, &age) || s < 1.5f) return false;
    g_last_throw_age = age;
    // the local velocity in the world through the camera's rows (the player's own walking is not added)
    const float right[3] = {b[1], 0.0f, -b[0]}, back[3] = {b[0], 0.0f, b[1]};
    for (int k = 0; k < 3; ++k) {
        vel[k] = (right[k] * v[0] + (k == 1 ? v[1] : 0.0f) + back[k] * v[2]) * g_throw_gain.load(std::memory_order_relaxed);
        g_last_throw[k] = vel[k];
    }
    g_throws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w, v;
    in >> c;
    while (in >> w >> v) {
        const bool on = v == "on";
        if (w == "throw") set_throw_by_hand(on);
        if (w == "melee") set_melee_by_swing(on);
        if (w == "lasso") set_lasso_by_hand(on);
        if (w == "peak") g_peak_speed[0] = g_peak_speed[1] = 0;  // "peak reset"
        if (w == "strike") aim::set_melee_strike(static_cast<float>(std::atof(v.c_str())));  // the session only
        if (w == "fistlead") g_fist_lead_ms = std::atoi(v.c_str());                            // the session only
    }
    char b[400];
    std::snprintf(b, sizeof(b),
                  "throw %d melee %d lasso %d | swing %.1f m/s gain %.2f strike %.2f fist lead %d ms | speed L %.2f R %.2f, peak L %.2f R %.2f | "
                  "punches %llu "
                  "lasso throws %llu yanks %llu throws %llu, last throw (%.2f %.2f %.2f) %.0f ms after the hand's peak",
                  g_throw.load() ? 1 : 0, g_melee.load() ? 1 : 0, g_lasso.load() ? 1 : 0, g_swing_speed.load(), g_throw_gain.load(),
                  aim::melee_strike(), g_fist_lead_ms.load(), g_last_speed[0],
                  g_last_speed[1], g_peak_speed[0], g_peak_speed[1], static_cast<unsigned long long>(g_punches.load()),
                  static_cast<unsigned long long>(g_lasso_throws.load()), static_cast<unsigned long long>(g_yanks.load()),
                  static_cast<unsigned long long>(g_throws.load()), g_last_throw[0], g_last_throw[1], g_last_throw[2], g_last_throw_age);
    return b;
}

}  // namespace rdrvr::gestures
