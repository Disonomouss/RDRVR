#include "core/pose.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "core/api.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/state.h"
#include "core/vr_mode.h"
#include "core/xinput.h"
#include "core/xr.h"

namespace rdrvr::pose {
namespace {

std::atomic<bool> g_anchor{false}, g_recentre{false};
// presenting thread
bool g_active = false, g_have_heading = false, g_snapped = false;
std::atomic<bool> g_active_shared{false};
std::atomic<float> g_heading_shared{0.0f};
// headset round 5: the eye height follows the head bone's height over the root (crouching, riding), low-passed so the
// walk bob stays out ([Body] EyeHeightFollow, HeightSmoothing seconds)
bool g_height_follow = true;
float g_height_tau = 0.35f, g_height_s = 0.0f;
bool g_height_s_valid = false;
bool g_no_idles = true;  // [Body] NoIdleAnims
bool g_hide_reticle = true;  // [Hands] HideReticle
float g_heading = 0, g_height = 1.60f, g_lift = 0.05f, g_forward = 0.05f, g_turn_speed = 90, g_snap_angle = 30;
int g_up_axis = 1;
int g_orient_mode = 1;  // SET_CAMERA_ORIENTATION: 1 = heading on the vector's y (round 1: 0, the heading on z, rolled the view)
bool g_auto_height = true, g_have_scale = false;
float g_measured_head = 0;
double g_last_ms = 0;
bool g_snap = false;
uint64_t g_turns = 0, g_walk_frames = 0;
float g_last_head_yaw = 0;
// riding ([Horse]): steering by the stick (the view faces the horse, the stick steers it) or by the head (as on foot:
// the stick turned by the head's yaw); the saddle anchor (the camera above the horse's own root, the rider's head
// offset from it averaged over about a second, the height smoothed by the plugin)
std::atomic<bool> g_steer_head{false}, g_saddle{true};
std::atomic<bool> g_saddle_climb{true};  // [Horse] SaddleClimb (run 6 item 8): the plugin's climb feed-forward and floor
// run 6 item 8, "pose trace on|off|reset|dump <file>": one row per script tick while riding (the presenting thread
// appends; the command thread dumps a CSV): the mount's position and heading, the camera, the rider's head and root
// (the seat), the 1 s averages of the head over the root
struct TraceRow {
    double ms;
    uint64_t tick;
    float mount[3], mh, cam[3], head[3], seat[3], h, f;
};
std::mutex g_trace_mutex;
std::vector<TraceRow> g_trace;  // under g_trace_mutex
std::atomic<bool> g_trace_on{false};
uint64_t g_trace_tick = 0;
std::atomic<float> g_saddle_tau{0.15f};
float g_saddle_h = 0.0f, g_saddle_f = 0.0f;
bool g_saddle_valid = false, g_was_riding = false;
uint64_t g_ride_frames = 0;
// the view's steadiness while riding: the camera's height over the horse's root, its change per script tick (RMS)
double g_jit_sum = 0.0, g_jit_abs = 0.0;  // the camera over the horse; the camera's own height
uint64_t g_jit_n = 0, g_jit_tick = 0;
float g_jit_last = 0.0f, g_jit_last_y = 0.0f;

int16_t stick(float v) {
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    return static_cast<int16_t>(v * 32767.0f);
}

void on_frame_end(uint64_t) {
    // the controllers mapped onto the game's pad (every frame: the menu button and the recentre combo are watched there)
    xinput::PadState pad;
    bool controllers = controls::pad(&pad);
    bool want = g_anchor.load(std::memory_order_relaxed) && xr::submitting() && vr_mode::gameplay_stereo();
    if (want != g_active) {
        g_active = want;
        g_active_shared = want;
        g_have_heading = false;
        g_last_ms = 0;
        log::info("[pose] camera anchor %s", want ? "on: the scripted camera follows the actor" : "off");
        if (!want) {
            xinput::set_source(false, {});
            xinput::set_pad_turn(false, 0);
        }
    }
    RdrvrCameraJob job{};
    job.enabled = want ? 1 : 0;
    job.up_axis = static_cast<uint32_t>(g_up_axis);
    job.height = g_height + g_lift;
    job.forward = g_forward;
    job.orient_mode = static_cast<uint32_t>(g_orient_mode);
    job.no_idles = g_no_idles ? 1 : 0;
    job.hide_reticle = g_hide_reticle ? 1 : 0;
    job.weapons = holster::weapon_choice() ? 1 : 0;
    if (want) {
        RdrvrActorState st{};
        if (!g_have_scale && g_auto_height && api::actor_state(&st) && st.head_valid) {
            float up = g_up_axis == 1 ? st.head_pos[1] - st.pos[1] : st.head_pos[2] - st.pos[2];
            if (up > 0.5f && up < 2.5f) {  // a standing human's head bone above the root
                g_measured_head = up;
                g_height = up;
                g_have_scale = true;
                log::info("[pose] world scale: the head bone is %.3f m above the actor's root; eye height %.3f m (+ lift %.2f)",
                          up, g_height, g_lift);
            }
        }
        if (!g_have_heading) {
            job.heading_from_actor = 1;
            if (api::actor_state(&st) && st.camera) {
                g_heading = st.heading_deg;
                g_have_heading = true;
                log::info("[pose] body heading from the actor: %.1f deg; actor (%.2f %.2f %.2f), camera (%.2f %.2f %.2f)",
                          g_heading, st.pos[0], st.pos[1], st.pos[2], st.camera_pos[0], st.camera_pos[1], st.camera_pos[2]);
            }
        }
        if (g_recentre.exchange(false)) {
            log::info("[pose] recentre: body heading %.1f -> %.1f deg (the head's yaw %.1f)", g_heading, g_heading + g_last_head_yaw,
                      g_last_head_yaw);
            g_heading += g_last_head_yaw;
            g_last_head_yaw = 0;
        }
        // turning on the right stick (positive heading turns left): the controllers' or the gamepad's, the larger
        double now = log::now_ms();
        float dt = g_last_ms > 0 ? static_cast<float>((now - g_last_ms) / 1000.0) : 0.0f;
        g_last_ms = now;
        if (dt <= 0 || dt > 0.1f) dt = 1.0f / 90.0f;
        // riding: the rider's head over the horse's root, averaged (the gait's bounce out), for the saddle anchor
        const bool riding = api::actor_state(&st) && st.mount != 0;
        if (riding != g_was_riding) {
            g_was_riding = riding;
            g_saddle_valid = false;
            log::info("[pose] %s (steering by the %s, saddle anchor %s)", riding ? "riding" : "on foot", g_steer_head.load() ? "head" : "stick",
                      g_saddle.load() ? "on" : "off");
        }
        if (riding && st.head_valid && g_up_axis == 1) {
            const float mh = st.mount_heading * 0.0174532925f;
            const float up = st.head_pos[1] - st.mount_pos[1];
            const float fw = (st.head_pos[0] - st.mount_pos[0]) * -std::sin(mh) + (st.head_pos[2] - st.mount_pos[2]) * -std::cos(mh);
            if (up > 0.5f && up < 3.5f && std::fabs(fw) < 2.0f) {
                const float a = 1.0f - std::exp(-dt / 1.0f);
                g_saddle_h = g_saddle_valid ? g_saddle_h + (up - g_saddle_h) * a : up;
                g_saddle_f = g_saddle_valid ? g_saddle_f + (fw - g_saddle_f) * a : fw;
                g_saddle_valid = true;
            }
            ++g_ride_frames;
            if (st.tick != g_jit_tick) {
                const float rel = st.camera_pos[1] - st.mount_pos[1];
                if (g_jit_tick && st.tick == g_jit_tick + 1) {
                    g_jit_sum += static_cast<double>(rel - g_jit_last) * (rel - g_jit_last);
                    g_jit_abs += static_cast<double>(st.camera_pos[1] - g_jit_last_y) * (st.camera_pos[1] - g_jit_last_y);
                    ++g_jit_n;
                }
                g_jit_last = rel;
                g_jit_last_y = st.camera_pos[1];
                g_jit_tick = st.tick;
            }
        }
        const bool stick_steer = riding && !g_steer_head.load(std::memory_order_relaxed);
        if (riding && st.tick != g_trace_tick && g_trace_on.load(std::memory_order_relaxed)) {
            g_trace_tick = st.tick;
            TraceRow r{};
            r.ms = now;
            r.tick = st.tick;
            std::memcpy(r.mount, st.mount_pos, sizeof(r.mount));
            r.mh = st.mount_heading;
            std::memcpy(r.cam, st.camera_pos, sizeof(r.cam));
            std::memcpy(r.head, st.head_pos, sizeof(r.head));
            std::memcpy(r.seat, st.pos, sizeof(r.seat));
            r.h = g_saddle_h;
            r.f = g_saddle_f;
            std::lock_guard lock(g_trace_mutex);
            if (g_trace.size() < 20000) g_trace.push_back(r);
        }
        if (riding && g_saddle_valid && g_saddle.load(std::memory_order_relaxed)) {
            job.saddle = g_saddle_climb.load(std::memory_order_relaxed) ? 3u : 1u;
            job.saddle_height = g_saddle_h + g_lift;
            job.saddle_forward = g_saddle_f + g_forward;
            job.saddle_tau = g_saddle_tau.load(std::memory_order_relaxed);
        }
        if (stick_steer) {
            // the view faces the horse and the stick steers it; the right stick does not turn the view
            job.heading_from_mount = 1;
            g_heading = st.mount_heading;
        }
        float sx = xinput::pad_right_x();
        if (controllers && std::fabs(pad.rx / 32767.0f) > std::fabs(sx)) sx = pad.rx / 32767.0f;
        if (stick_steer) sx = 0.0f;
        if (g_snap) {
            if (std::fabs(sx) > 0.7f && !g_snapped) {
                g_heading += sx > 0 ? -g_snap_angle : g_snap_angle;
                g_snapped = true;
                ++g_turns;
            } else if (std::fabs(sx) < 0.3f) {
                g_snapped = false;
            }
        } else if (std::fabs(sx) > 0.15f) {
            g_heading -= sx * g_turn_speed * dt;
            ++g_turns;
        }
        g_heading = std::fmod(g_heading + 540.0f, 360.0f) - 180.0f;
        job.heading_deg = g_heading;
        g_heading_shared = g_heading;
        if (g_height_follow && g_have_scale && api::actor_state(&st) && st.head_valid) {
            float up = g_up_axis == 1 ? st.head_pos[1] - st.pos[1] : st.head_pos[2] - st.pos[2];
            if (up > 0.2f && up < 2.5f) {
                float a = 1.0f - std::exp(-dt / (g_height_tau > 0.01f ? g_height_tau : 0.01f));
                g_height_s = g_height_s_valid ? g_height_s + (up - g_height_s) * a : up;
                g_height_s_valid = true;
                job.height = g_height_s + g_lift;
            }
        }
        // walking: the left stick turned by the head's yaw relative to the body. Riding: the game steers the horse
        // relative to itself (a stick held left keeps turning it), so steering by the head turns the stick by the
        // angle from the horse's heading to where the head faces, which goes to zero as the horse comes round; with
        // stick steering it is not turned at all
        float yaw = 0;
        if (camera_lever::head_yaw_deg(&yaw)) g_last_head_yaw = yaw;
        float stick_yaw = stick_steer ? 0.0f : g_last_head_yaw;
        if (riding && !stick_steer) stick_yaw = std::fmod(g_heading + g_last_head_yaw - st.mount_heading + 540.0f, 360.0f) - 180.0f;
        // the gamepad's left stick turned by the head's yaw (and its right x kept for turning), and the controllers' too
        xinput::set_pad_turn(true, stick_yaw);
        if (controllers) {
            float a = stick_yaw * 0.0174532925f, lx = pad.lx / 32767.0f, ly = pad.ly / 32767.0f;
            pad.lx = stick(lx * std::cos(a) - ly * std::sin(a));
            pad.ly = stick(lx * std::sin(a) + ly * std::cos(a));
            pad.rx = 0;  // the anchored camera is not the game's to turn
            if (lx != 0 || ly != 0) ++g_walk_frames;
        }
    }
    xinput::set_source(controllers, controllers ? pad : xinput::PadState{});
    api::set_camera_job(job);
}

}  // namespace

void init() {
    g_anchor = config::get_bool("Body", "CameraAnchor", false);
    g_height = config::get_float("Body", "EyeHeight", 0.0f);  // 0 = measured from the head bone
    g_auto_height = g_height <= 0;
    if (g_auto_height) g_height = 1.60f;  // until measured
    g_lift = config::get_float("Body", "SeatedLift", 0.09f);
    g_no_idles = config::get_bool("Body", "NoIdleAnims", true);
    g_hide_reticle = config::get_bool("Hands", "HideReticle", true);
    g_forward = config::get_float("Body", "EyeForward", 0.05f);
    g_up_axis = config::get_int("Body", "UpAxis", 1);
    g_orient_mode = config::get_int("Body", "OrientMode", 1);
    g_height_follow = config::get_bool("Body", "EyeHeightFollow", true);
    g_height_tau = config::get_float("Body", "HeightSmoothing", 0.35f);
    g_snap = config::get_string("Comfort", "TurnMode", "smooth") == "snap";
    g_turn_speed = config::get_float("Comfort", "TurnSpeed", 90.0f);
    g_snap_angle = config::get_float("Comfort", "SnapAngle", 30.0f);
    g_steer_head = config::get_string("Horse", "Steering", "stick") == "head";
    g_saddle = config::get_bool("Horse", "SaddleAnchor", true);
    g_saddle_tau = config::get_float("Horse", "SaddleSmoothing", 0.15f);
    g_saddle_climb = config::get_bool("Horse", "SaddleClimb", true);
    log::info("[pose] horse: steering by the %s, saddle anchor %d (smoothing %.2f s)", g_steer_head.load() ? "head" : "stick",
              g_saddle.load() ? 1 : 0, g_saddle_tau.load());
    d3d::add_frame_end_listener(on_frame_end);
}

void set_turning(bool snap, float speed_deg_s, float snap_deg) {
    g_snap = snap;
    g_turn_speed = speed_deg_s;
    g_snap_angle = snap_deg;
    log::info("[pose] turning: %s (%.0f deg/s, snap %.0f deg)", snap ? "snap" : "smooth", speed_deg_s, snap_deg);
}
bool snap_turning() { return g_snap; }
float turn_speed() { return g_turn_speed; }
float snap_angle() { return g_snap_angle; }

void on_recentre() { g_recentre = true; }

void set_orient_mode(int m) {
    g_orient_mode = m;
    log::info("[pose] scripted camera orientation mode %d", m);
}

bool steer_by_head() { return g_steer_head.load(); }
void set_steer_by_head(bool on) {
    if (g_steer_head.exchange(on) != on) log::info("[pose] horse steering by the %s", on ? "head" : "stick");
    config::set("Horse", "Steering", on ? "head" : "stick");
}
bool saddle_anchor() { return g_saddle.load(); }
bool saddle_climb() { return g_saddle_climb.load(); }
void set_saddle_climb(bool on, bool save) {
    if (g_saddle_climb.exchange(on) != on) log::info("[pose] the saddle anchor follows climbs %d", on ? 1 : 0);
    if (save) config::set("Horse", "SaddleClimb", on ? "1" : "0");
}
std::string trace_command(const std::string& verb, const std::string& arg) {
    if (verb == "on" || verb == "off") {
        g_trace_on = verb == "on";
    } else if (verb == "reset") {
        std::lock_guard lock(g_trace_mutex);
        g_trace.clear();
    } else if (verb == "dump") {
        std::vector<TraceRow> rows;
        {
            std::lock_guard lock(g_trace_mutex);
            rows = g_trace;
        }
        FILE* fp = nullptr;
        if (arg.empty() || fopen_s(&fp, arg.c_str(), "w") != 0 || !fp) return "ERROR cannot write " + arg;
        std::fprintf(fp, "ms,tick,mount_x,mount_y,mount_z,mount_heading,cam_x,cam_y,cam_z,head_x,head_y,head_z,seat_x,seat_y,seat_z,h,f\n");
        for (const TraceRow& r : rows)
            std::fprintf(fp, "%.1f,%llu,%.4f,%.4f,%.4f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", r.ms,
                         static_cast<unsigned long long>(r.tick), r.mount[0], r.mount[1], r.mount[2], r.mh, r.cam[0], r.cam[1], r.cam[2], r.head[0],
                         r.head[1], r.head[2], r.seat[0], r.seat[1], r.seat[2], r.h, r.f);
        std::fclose(fp);
        return "trace: " + std::to_string(rows.size()) + " rows written to " + arg;
    }
    std::lock_guard lock(g_trace_mutex);
    return std::string("trace ") + (g_trace_on.load() ? "on" : "off") + ", " + std::to_string(g_trace.size()) + " rows; saddle climb " +
           (g_saddle_climb.load() ? "on" : "off");
}
float saddle_smoothing() { return g_saddle_tau.load(); }
void reset_ride_jitter() {
    g_jit_sum = 0.0;
    g_jit_abs = 0.0;
    g_jit_n = 0;
}
void set_saddle(bool on, float tau, bool save) {
    g_saddle = on;
    g_saddle_tau = tau < 0.0f ? 0.0f : tau > 1.0f ? 1.0f : tau;
    if (!save) return;
    config::set("Horse", "SaddleAnchor", on ? "1" : "0");
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", g_saddle_tau.load());
    config::set("Horse", "SaddleSmoothing", b);
}

bool anchor_active() { return g_active_shared.load(std::memory_order_relaxed); }
bool anchor_enabled() { return g_anchor.load(std::memory_order_relaxed); }
float body_heading_deg() { return g_heading_shared.load(std::memory_order_relaxed); }
float eye_lift() { return g_lift; }
void set_eye_lift(float m) {
    g_lift = m < -0.3f ? -0.3f : m > 0.4f ? 0.4f : m;
}

void set_anchor(bool on) {
    g_anchor = on;
    log::info("[pose] camera anchor switch %s", on ? "on" : "off");
}

void status_text(char* out, size_t len) {
    RdrvrActorState st{};
    bool have = api::actor_state(&st);
    std::snprintf(out, len,
                  "anchor %s (%s), heading %.1f deg, head yaw %.1f, eye height %.3f%s, turns %llu, walk frames %llu | actor %s (%.2f %.2f %.2f) "
                  "heading %.1f, camera %d at (%.2f %.2f %.2f), tick %llu | mount %d (%.2f %.2f %.2f) heading %.1f, saddle %s h %.2f f %.2f, "
                  "steer %s, ride frames %llu, jitter %.4f m/tick (camera height %.4f) over %llu",
                  g_anchor.load() ? "on" : "off", g_active ? "active" : "idle", g_heading, g_last_head_yaw, g_height,
                  g_have_scale ? " (head bone)" : g_auto_height ? " (not yet measured)" : " (ini)",
                  static_cast<unsigned long long>(g_turns), static_cast<unsigned long long>(g_walk_frames), have ? "" : "none",
                  st.pos[0], st.pos[1], st.pos[2], st.heading_deg, st.camera, st.camera_pos[0], st.camera_pos[1], st.camera_pos[2],
                  static_cast<unsigned long long>(st.tick), st.mount, st.mount_pos[0], st.mount_pos[1], st.mount_pos[2], st.mount_heading,
                  g_saddle.load() ? "on" : "off", g_saddle_h, g_saddle_f, g_steer_head.load() ? "head" : "stick",
                  static_cast<unsigned long long>(g_ride_frames), g_jit_n ? std::sqrt(g_jit_sum / static_cast<double>(g_jit_n)) : 0.0,
                  g_jit_n ? std::sqrt(g_jit_abs / static_cast<double>(g_jit_n)) : 0.0,
                  static_cast<unsigned long long>(g_jit_n));
}

}  // namespace rdrvr::pose
