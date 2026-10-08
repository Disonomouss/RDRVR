#include "core/test_channel.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/anchors.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/burst_grab.h"
#include "core/camera_lever.h"
#include "core/body.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/diag.h"
#include "core/dual_pass.h"
#include "core/eye_grab.h"
#include "core/eye_shape.h"
#include "core/frame_grab.h"
#include "core/game_hooks.h"
#include "core/holster.h"
#include "core/aim.h"
#include "core/reload.h"
#include "core/actions.h"
#include "core/dlss.h"
#include "core/dual.h"
#include "core/physics.h"
#include "core/gestures.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/lum_check.h"
#include "core/pso.h"
#include "core/render_settings.h"
#include "core/align_grab.h"
#include "core/ring_probe.h"
#include "core/state.h"
#include "core/taa.h"
#include "core/xinput.h"
#include "core/xr.h"
#include "core/xr_blit.h"
#include "core/ui_layer.h"
#include "core/vr_mode.h"
#include "core/menu.h"
#include "core/hands.h"
#include "core/pose.h"
#include "core/config.h"
#include "core/round_draw.h"
#include "core/held_prop.h"
#include "core/render_res.h"
#include "core/zone_rings.h"

namespace rdrvr::test_channel {
namespace {

long g_seq = -1;            // last handled seq
std::string g_result = "none";
wchar_t g_cmd_path[MAX_PATH], g_status_path[MAX_PATH], g_status_tmp[MAX_PATH];

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// "0x..", decimal, or "1.5f" (a float passed in the low 32 bits); "v" marks a Vector3 argument (out), "v:x,y,z" one
// that starts with those values (in/out).
// native arguments: "s:text" a string (kept alive here: the native runs on a script tick), "p:x,y" a Vector2 (two
// floats in one 8-byte slot, as the natives' handlers read it), "1.5f" a float's bits, else an integer
std::string g_str_args[16];
size_t g_str_next = 0;
uint64_t parse_arg(const std::string& a) {
    if (a.rfind("s:", 0) == 0) {
        std::string& slot = g_str_args[g_str_next++ % 16];
        slot = a.substr(2);
        return reinterpret_cast<uint64_t>(slot.c_str());
    }
    if (a.rfind("p:", 0) == 0) {
        float xy[2] = {0, 0};
        sscanf_s(a.c_str() + 2, "%f,%f", &xy[0], &xy[1]);
        uint64_t packed = 0;
        std::memcpy(&packed, xy, sizeof(xy));
        return packed;
    }
    if (!a.empty() && (a.back() == 'f' || a.back() == 'F') && a.find('.') != std::string::npos) {
        float f = std::strtof(a.c_str(), nullptr);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return bits;
    }
    return std::strtoull(a.c_str(), nullptr, 0);
}

std::string run_native(uint32_t hash, const std::vector<uint64_t>& args, uint32_t vec_out, const float* vec_in = nullptr) {
    if (!api::plugin_attached()) return "ERROR gameplay plugin not attached (RedHook not loaded?)";
    uint64_t id = api::queue_native(hash, args.data(), static_cast<uint32_t>(args.size()), vec_out, vec_in);
    RdrvrNativeResult r{};
    if (!api::wait_native(id, &r, 3000)) return "ERROR native timed out (no script tick in 3 s)";
    char buf[192];
    float f;
    uint32_t lo = static_cast<uint32_t>(r.value);
    std::memcpy(&f, &lo, 4);
    std::snprintf(buf, sizeof(buf), "value %#llx (int %d, float %g) vec (%.3f %.3f %.3f) tick %llu",
                  static_cast<unsigned long long>(r.value), static_cast<int>(lo), f, r.vec[0], r.vec[1], r.vec[2],
                  static_cast<unsigned long long>(r.tick));
    return buf;
}

std::string execute(const std::vector<std::string>& t) {
    const std::string& c = t[0];
    if (c == "ping") return "pong";
    if (c == "status") return "ok";
    if (c == "log") {
        std::string text;
        for (size_t i = 1; i < t.size(); ++i) text += (i > 1 ? " " : "") + t[i];
        log::info("[harness] %s", text.c_str());
        return "ok";
    }
    if (c == "fg") {
        int n = t.size() > 1 ? std::atoi(t[1].c_str()) : 2;
        d3d::framegraph_arm(n);
        return std::string("armed ") + std::to_string(n);
    }
    if (c == "census") {
        game_hooks::census_log_now();
        return "logged";
    }
    if (c == "native" && t.size() >= 2) {
        std::vector<uint64_t> args;
        uint32_t vec = 0;
        float vin[3] = {0, 0, 0};
        bool has_vin = false;
        for (size_t i = 2; i < t.size(); ++i) {
            if (t[i] == "v" || t[i].rfind("v:", 0) == 0) {
                if (t[i].size() > 2) {
                    has_vin = sscanf_s(t[i].c_str() + 2, "%f,%f,%f", &vin[0], &vin[1], &vin[2]) == 3;
                }
                args.push_back(0);
                vec = static_cast<uint32_t>(args.size());
            } else {
                args.push_back(parse_arg(t[i]));
            }
        }
        return run_native(static_cast<uint32_t>(parse_arg(t[1])), args, vec, has_vin ? vin : nullptr);
    }
    if (c == "pos") {
        std::string actor = run_native(0xE8CFDD53u, {0}, 0);  // GET_PLAYER_ACTOR(0)
        if (actor.rfind("ERROR", 0) == 0) return actor;
        int handle = std::atoi(actor.c_str() + actor.find("int ") + 4);
        return std::string("actor ") + std::to_string(handle) + " " +
               run_native(0x99BD9D6Fu, {static_cast<uint64_t>(static_cast<uint32_t>(handle)), 0}, 2);  // GET_POSITION
    }
    if (c == "cam" && t.size() >= 5 && t[1] == "moveramp") {  // cam moveramp dx dy dz: the slide, metres a frame (0 0 0 stops)
        camera_lever::set_move_ramp(std::strtof(t[2].c_str(), nullptr), std::strtof(t[3].c_str(), nullptr), std::strtof(t[4].c_str(), nullptr));
        char st[400];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "cam" && t.size() >= 3 && t[1] == "yawramp") {  // cam yawramp <deg a frame>: the deterministic turn (0 stops)
        camera_lever::set_yaw_ramp(std::strtof(t[2].c_str(), nullptr));
        char st[400];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "cam" && t.size() >= 3 && t[1] == "yaw") {
        camera_lever::set_yaw(std::strtof(t[2].c_str(), nullptr));
        char st[400];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "cam" && t.size() >= 2) {
        // cam off | cam camera|view|world dx dy dz   (metres along the camera matrix rows a, b, c)
        camera_lever::Mode m = t[1] == "camera" ? camera_lever::Mode::Camera
                               : t[1] == "view"  ? camera_lever::Mode::View
                               : t[1] == "world" ? camera_lever::Mode::World
                                                 : camera_lever::Mode::Off;
        if (m != camera_lever::Mode::Off && t.size() < 5) return "ERROR usage: cam camera|view|world dx dy dz";
        auto f = [&](size_t i) { return i < t.size() ? std::strtof(t[i].c_str(), nullptr) : 0.0f; };
        camera_lever::set(m, f(2), f(3), f(4));
        char st[160];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "proj" && t.size() >= 6 && t[1] == "tan") {
        auto f = [&](size_t i) { return std::strtof(t[i].c_str(), nullptr); };
        camera_lever::set_tangents(f(2), f(3), f(4), f(5));
        char st[512];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "proj" && t.size() >= 2) {
        camera_lever::set_eye_projection(t[1] == "left" ? 0 : t[1] == "right" ? 1 : t[1] == "xr" ? 3 : -1);
        char st[512];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "imposters") {  // imposters game|eye: the tree imposters' capture direction
        if (t.size() >= 2) camera_lever::set_level_imposters(t[1] == "game");
        return camera_lever::imposter_status();
    }
    if (c == "billboards" && t.size() >= 3 && t[1] == "facing") {
        camera_lever::set_billboard_sign(t[2] == "toward");
        return "billboard facing " + t[2];
    }
    if (c == "billboards" && t.size() >= 3 && t[1] == "w2s") {
        camera_lever::set_level_w2s(t[2] == "level");
        return "forest view " + t[2];
    }
    if (c == "billboards" && t.size() >= 3 && t[1] == "vi") {
        camera_lever::set_vi_level(t[2] == "level");
        return "view inverse " + t[2];
    }
    if (c == "billboards" && t.size() >= 3 && t[1] == "grass") {
        camera_lever::set_level_grass(t[2] == "level");
        return "grass billboards " + t[2];
    }
    if (c == "billboards" && t.size() >= 2) {
        camera_lever::set_level_billboards(t[1] == "level");
        return "billboards " + t[1];
    }
    if (c == "cover" && t.size() >= 3 && t[1] == "head") {
        camera_lever::set_head_cover(t[2] == "on");
        return "head cover " + t[2];
    }
    if (c == "cover" && t.size() >= 2) {
        // cover off | cover <vfov degrees> <aspect>
        camera_lever::set_cover(t[1] == "off" ? 0.0f : std::strtof(t[1].c_str(), nullptr),
                                t.size() >= 3 ? std::strtof(t[2].c_str(), nullptr) : 1.7778f);
        char st[320];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "double" && t.size() >= 2) {
        // double off | double on [ipd metres] [swap]
        float ipd = t.size() >= 3 ? std::strtof(t[2].c_str(), nullptr) : 0.0f;
        bool swap = t.size() >= 4 && t[3] == "swap";
        camera_lever::set_double(t[1] == "on", ipd, swap);
        char st[400];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "split" && t.size() >= 3) {
        // split <name> on|off, the names in dual_pass.h
        if (!dual_pass::set_split(t[1].c_str(), t[2] == "on"))
            return "ERROR unknown split (grass gust lights forest post masks exposure rain pfxmap godrays damage clouds "
                   "sunvis hold)";
        char st[512];
        dual_pass::status_text(st, sizeof(st));
        return st;
    }
    if (c == "grabeyes" && t.size() >= 2) return eye_grab::grab(t[1]);
    if (c == "renderres") return render_res::command(c);  // renderres: [Render] RenderResolution's state this start
    if (c == "eyeshape") {  // eyeshape [on|off|scale <x>|status|rts]: the eyes in the headset's own shape (EyeShape; the session)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return eye_shape::command(line);
    }
    if (c == "aa" && t.size() >= 2) {
        int m = t[1] == "off" ? 0 : t[1] == "fxaa" ? 1 : t[1] == "taa" ? 2 : -1;
        if (m < 0 || !render_settings::set_aa(m)) return "ERROR usage: aa off|fxaa|taa (needs [Render] ForceAntiAliasing >= 0)";
        char st[256];
        render_settings::status_text(st, sizeof(st));
        return st;
    }
    if (c == "aligngrab" && t.size() >= 3)  // aligngrab <prefix> <frames> [crop_w crop_h]: the upscaler's alignment
        return align_grab::grab(t[1], std::atoi(t[2].c_str()), t.size() >= 5 ? std::atoi(t[3].c_str()) : 1536,
                                t.size() >= 5 ? std::atoi(t[4].c_str()) : 864);
    if (c == "xr" && t.size() >= 3 && t[1] == "rotramp") {  // xr rotramp <deg>: a head turning at deg a frame (0 stops)
        xr::set_rot_ramp(std::strtof(t[2].c_str(), nullptr));
        return "xr rotramp " + t[2];
    }
    if (c == "xr" && t.size() >= 4 && t[1] == "posnoise") {  // xr posnoise <rot_deg> <pos_m>: white noise a frame (0 0 stops)
        xr::set_pose_noise(std::strtof(t[2].c_str(), nullptr), std::strtof(t[3].c_str(), nullptr));
        return "xr posnoise " + t[2] + " " + t[3];
    }
    if (c == "cuts") {  // what discarded the upscaler's history (velocity clears, resets), per post run
        char st[600];
        taa::cut_text(st, sizeof(st));
        return st;
    }
    if (c == "dlss" && t.size() >= 3 && t[1] == "units") {  // dlss units on|off: the jitter in render pixels or the game's
        taa::set_jitter_units(t[2] == "on");
        char st[700];
        taa::status_text(st, sizeof(st));
        return st;
    }
    if (c == "dlss" && t.size() >= 3 && t[1] == "mvscale") {  // dlss mvscale <k>: the positive control
        dlss::set_mv_scale(std::strtof(t[2].c_str(), nullptr));
        return "dlss mvscale " + t[2];
    }
    if (c == "dlss" && t.size() >= 2 && t[1] == "phases") {  // dlss phases [on|off]: the jitter sequence's length
        if (t.size() >= 3) render_settings::set_full_jitter(t[2] == "on");
        char st[300];
        render_settings::jitter_text(st, sizeof(st));
        return st;
    }
    if (c == "dlss") {  // dlss [off]: per-eye DLSS's state and counters; off = the kill switch
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return dlss::command(line);
    }
    if (c == "taa" && (t.size() == 1 || t[1] == "status")) {  // taa [status]: the TAA/DLSS pass state and counters
        char st[700];
        taa::status_text(st, sizeof(st));
        return st;
    }
    if (c == "taa" && t.size() >= 6 && t[1] == "params") {
        const float v[4] = {std::strtof(t[2].c_str(), nullptr), std::strtof(t[3].c_str(), nullptr),
                            std::strtof(t[4].c_str(), nullptr), std::strtof(t[5].c_str(), nullptr)};
        return taa::set_params(v) ? "params set" : "ERROR no PostFx";
    }
    if (c == "taa" && t.size() >= 3 && t[1] == "monopass") {
        taa::set_mono_pass(t[2] == "on");
        char st[512];
        taa::status_text(st, sizeof(st));
        return st;
    }
    if (c == "taa" && t.size() >= 3 && t[1] == "shared") {
        taa::set_shared(t[2] == "on");
        char st[400];
        taa::status_text(st, sizeof(st));
        return st;
    }
    if (c == "lumcheck") return lum_check::run(t.size() >= 2 ? std::atoi(t[1].c_str()) : 8);
    if (c == "grabburst" && t.size() >= 3) {  // grabburst <prefix> <frames> [crop_w crop_h] [post|scene|vel]
        const int src = t.size() >= 6 ? (t[5] == "scene" ? 1 : t[5] == "vel" ? 2 : 0) : 0;
        return burst_grab::grab(t[1], std::atoi(t[2].c_str()), t.size() >= 5 ? std::atoi(t[3].c_str()) : 640,
                                t.size() >= 5 ? std::atoi(t[4].c_str()) : 360, 10000, src);
    }
    if (c == "xr" && t.size() >= 5 && t[1] == "posramp") {  // xr posramp dx dy dz: metres an XR frame (0 0 0 stops)
        xr::set_pos_ramp(std::strtof(t[2].c_str(), nullptr), std::strtof(t[3].c_str(), nullptr), std::strtof(t[4].c_str(), nullptr));
        return "xr posramp " + t[2] + " " + t[3] + " " + t[4];
    }
    if (c == "pose" && t.size() >= 2 && (t[1] == "xr" || t[1] == "off")) {
        // pose xr | pose off: eye cameras from the located XR views (R5); "recentre" takes the current views as origin
        camera_lever::set_xr_pose(t[1] == "xr");
        char st[400];
        camera_lever::status_text(st, sizeof(st));
        return st;
    }
    if (c == "xr" && t.size() >= 3 && t[1] == "submit") {
        xr::set_submit(t[2] == "on");
        char st[1024];
        xr::submit_status(st, sizeof(st));
        return st;
    }
    if (c == "xr" && t.size() >= 3 && t[1] == "gamma") {
        xr_blit::set_gamma_override(t[2] == "live" ? 0.0f : static_cast<float>(std::atof(t[2].c_str())));
        char st[1024];
        xr::submit_status(st, sizeof(st));
        return st;
    }
    if (c == "xr" && t.size() >= 2 && (t[1] == "status" || t[1] == "resized")) {
        // xr status: the submission's (also the status file's xr_submit); xr resized: the live-resize guard's stop as at
        // a real resize, for the rest of this start (the eyes stop, the cinema screen shows the flat game)
        if (t[1] == "resized") xr::force_frame_resized();
        char st[1024];
        xr::submit_status(st, sizeof(st));
        return st;
    }
    if (c == "ui" && t.size() >= 3 && t[1] == "redirect") {
        ui_layer::set_redirect(t[2] == "on");
        char st[256];
        ui_layer::status_text(st, sizeof(st));
        return st;
    }
    if (c == "grabui" && t.size() >= 2) return ui_layer::grab(t[1]);
    if (c == "menu") {
        // menu | menu on|off | menu find <label> | menu scroll <y> | menu grab <path.bmp>
        if (t.size() >= 3 && t[1] == "grab") {
            std::string path;
            for (size_t i = 2; i < t.size(); ++i) path += (i > 2 ? " " : "") + t[i];
            return menu::grab(path);
        }
        if (t.size() >= 3 && t[1] == "scroll") {
            menu::scroll(static_cast<float>(std::atof(t[2].c_str())));
            return "menu scrolled to " + t[2];
        }
        if (t.size() >= 3 && t[1] == "find") {
            std::string label;
            for (size_t i = 2; i < t.size(); ++i) label += (i > 2 ? " " : "") + t[i];
            return menu::find(label);
        }
        if (t.size() >= 2) menu::set_visible(t[1] == "on");
        char st[200];
        menu::status_text(st, sizeof(st));
        return st;
    }
    if (c == "pose" && (t.size() == 1 || t[1] != "xr" && t[1] != "off")) {
        // pose | pose anchor on|off  ("pose xr" / "pose off" are the eye-camera switch above)
        if (t.size() >= 3 && t[1] == "anchor") pose::set_anchor(t[2] == "on");
        if (t.size() >= 3 && t[1] == "orient") pose::set_orient_mode(std::atoi(t[2].c_str()));
        // pose saddle on|off [seconds]: the saddle anchor this session (not saved); pose horse stick|head writes the ini
        if (t.size() >= 4 && t[1] == "saddle" && t[2] == "climb") pose::set_saddle_climb(t[3] == "on", false);  // pose saddle climb on|off
        else if (t.size() >= 3 && t[1] == "saddle")
            pose::set_saddle(t[2] == "on", t.size() >= 4 ? std::strtof(t[3].c_str(), nullptr) : pose::saddle_smoothing(), false);
        if (t.size() >= 3 && t[1] == "trace") return pose::trace_command(t[2], t.size() >= 4 ? t[3] : "");  // pose trace on|off|reset|dump <file>
        if (t.size() >= 3 && t[1] == "horse") pose::set_steer_by_head(t[2] == "head");
        if (t.size() >= 3 && t[1] == "jitter") pose::reset_ride_jitter();
        if (t.size() >= 3 && t[1] == "turn")  // pose turn snap|smooth [speed] [snap angle]
            pose::set_turning(t[2] == "snap", t.size() >= 4 ? std::strtof(t[3].c_str(), nullptr) : pose::turn_speed(),
                              t.size() >= 5 ? std::strtof(t[4].c_str(), nullptr) : pose::snap_angle());
        char st[600];
        pose::status_text(st, sizeof(st));
        return st;
    }
    if (c == "skel") {
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return body::command(line);
    }
    if (c == "aim") {
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return aim::command(line);
    }
    if (c == "holster") {
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return holster::command(line);
    }
    if (c == "hud") {  // hud wrist|quad (writes the user ini)
        if (t.size() >= 2) xr::set_hud_on_wrist(t[1] == "wrist");
        char st[300];
        xr::hud_status(st, sizeof(st));
        return st;
    }
    if (c == "actions") {  // actions [on|off] [open] [reset]: working the guns' actions by hand (ManualActions)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return actions::command(line);
    }
    if (c == "physics") {  // physics [on|off] [probe <radius>]: hands pushing loose props (HandCollision)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return physics::command(line);
    }
    if (c == "round") {  // round [on|off] [offset|axis x y z] [held <hand> [family]] [grab <path.bmp> [eye]]: RoundInHand
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return round_draw::command(line);
    }
    if (c == "props") {  // the props the mod made and holds (held_prop); props diag [on|off|reset]: the draws near each
        if (t.size() >= 2 && t[1] == "diag") return held_prop::diag_command(t.size() >= 3 ? t[2] : "");
        return held_prop::status();
    }
    if (c == "dual") {  // dual [on|off] [begin <slot> [left|right]] [fire] [end]: the second gun (DualWield)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return dual::command(line);
    }
    if (c == "gestures") {
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return gestures::command(line);
    }
    if (c == "gunmelee") {  // gunmelee [on|off] [dry on|off] [reset] [scan [radius]]: the gun-butt melee (GunMelee; the session)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return gestures::gun_melee_command(line);
    }
    if (c == "rings") {  // the holster rings' layers; rings atlas <path.bmp>: the atlas (the rings, the dots, the reticles)
        if (t.size() >= 3 && t[1] == "atlas") {
            std::string path;
            for (size_t i = 2; i < t.size(); ++i) path += (i > 2 ? " " : "") + t[i];
            return zone_rings::write_atlas(path);
        }
        char b[240];
        xr::rings_status(b, sizeof(b));
        return b;
    }
    if (c == "sound") {  // the empty gun's click sound
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return audio::command(line);
    }
    if (c == "ammo") {  // the hand reload ("reload" reloads the config)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return reload::command(line);
    }
    if (c == "controls") {
        // controls map <Source> <target> | controls lefthanded on|off | controls reset | controls: the mapping
        if (t.size() >= 4 && t[1] == "map") {
            for (int s = 0; s < controls::kSources; ++s)
                if (_stricmp(t[2].c_str(), controls::source_key(s)) == 0)
                    for (int g = 0; g < controls::kTargets; ++g)
                        if (_stricmp(t[3].c_str(), controls::target_name(g)) == 0) controls::set_mapping(s, g);
        }
        if (t.size() >= 3 && t[1] == "lefthanded") controls::set_left_handed(t[2] == "on");
        if (t.size() >= 3 && t[1] == "triggeraims") controls::set_trigger_aims_session(t[2] == "on");  // the session only
        if (t.size() >= 3 && t[1] == "raised") controls::set_aim_when_raised(t[2] == "on", false);  // the session only
        if (t.size() >= 3 && t[1] == "drawhand") controls::set_draw_to_grabbing_hand(t[2] == "on", false);  // the session only
        if (t.size() >= 3 && t[1] == "clickbrake") controls::set_click_brake_session(t[2] == "on");
        if (t.size() >= 3 && t[1] == "sprint") controls::set_sprint_drops_aim(t[2] == "on", false);  // this session only
        if (t.size() >= 3 && t[1] == "jump") controls::set_jump_drops_aim(t[2] == "on", false);  // run 7 item 1f, this session only
        if (t.size() >= 2 && t[1] == "reset") controls::reset_mapping();
        std::string r = "map:";
        for (int s = 0; s < controls::kSources; ++s)
            r += std::string(" ") + controls::source_key(s) + "=" + controls::target_name(controls::mapping(s));
        char b[900];
        controls::status_text(b, sizeof(b));
        return r + " || " + b;
    }
    if (c == "controllers") {  // controllers [latch on|off] [fit x y z pitch yaw roll]: the late latch; the right type's fit (session)
        if (t.size() >= 3 && t[1] == "latch") xr::set_latch_hands(t[2] == "on");
        if (t.size() >= 8 && t[1] == "fit") {
            const float o[3] = {std::strtof(t[2].c_str(), nullptr), std::strtof(t[3].c_str(), nullptr), std::strtof(t[4].c_str(), nullptr)};
            const float a[3] = {std::strtof(t[5].c_str(), nullptr), std::strtof(t[6].c_str(), nullptr), std::strtof(t[7].c_str(), nullptr)};
            controllers::set_fit(1, o, a, false);
        }
        char a[1024], b[900];
        controllers::status_text(a, sizeof(a));
        controls::status_text(b, sizeof(b));
        return std::string(a) + " || " + b;
    }
    if (c == "hand") {
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return hands::command(line);
    }
    if (c == "cfg" && t.size() >= 3 && t[1] == "userini") {
        std::string p;
        for (size_t i = 2; i < t.size(); ++i) p += (i > 2 ? " " : "") + t[i];
        std::wstring w(p.begin(), p.end());
        config::set_user_path(w.c_str());
        return "user ini for this run: " + p;
    }
    if (c == "sun") {
        // sun | sun check on|off
        if (t.size() >= 3 && t[1] == "check") dual_pass::set_sun_behind_check(t[2] == "on");
        char st[200];
        dual_pass::sun_status(st, sizeof(st));
        return st;
    }
    if (c == "drops") {
        // drops | drops on|off
        if (t.size() >= 2) dual_pass::set_lens_drops(t[1] == "on");
        char st[400];
        dual_pass::lens_drops_status(st, sizeof(st));
        return st;
    }
    if (c == "perf") {
        if (t.size() >= 2 && t[1] == "reset") xr::perf_reset();
        char st[400];
        xr::perf_status(st, sizeof(st));
        return st;
    }
    if (c == "mode") {
        // mode | mode auto on|off | mode cutscene screen|3d
        if (t.size() >= 3 && t[1] == "auto") vr_mode::set_auto(t[2] == "on");
        if (t.size() >= 3 && t[1] == "cutscene") vr_mode::set_cutscene_3d(t[2] == "3d");
        if (t.size() >= 4 && t[1] == "force" && t[2] == "cutscene")
            vr_mode::force_cutscene(t[3] == "on" ? 1 : t[3] == "off" ? 0 : -1);
        char st[256];
        vr_mode::status_text(st, sizeof(st));
        return st;
    }
    if (c == "cinema" && t.size() >= 2) {
        xr::set_cinema(t[1] == "on");
        char st[1024];
        xr::submit_status(st, sizeof(st));
        return st;
    }
    if (c == "recentre") {
        camera_lever::recentre();
        return "recentre on the next pass";
    }
    if (c == "findcam") {
        camera_lever::find_centre_camera();
        return "findcam armed for the next double frame (results in the log)";
    }
    if (c == "drawlog") {
        dual_pass::arm_drawlog();
        return "drawlog armed for the next double frame";
    }  // absolute prefix: <prefix>-L/-R/-M.rde
    if (c == "mcheck" && t.size() >= 2) {
        ring_probe::matrix_check(t[1] == "on");
        return "ok";
    }
    if (c == "camlog") {
        camera_lever::request_log();
        return "logging the next scene camera";
    }
    if (c == "grab" && t.size() >= 2) return frame_grab::grab(t[1]);
    if (c == "grabdepth" && t.size() >= 2) return frame_grab::grab_depth(t[1]);
    if (c == "renderscale" && t.size() >= 2) {
        if (!render_settings::set_fixed_scale(std::strtof(t[1].c_str(), nullptr))) return "ERROR needs [Render] ForceAntiAliasing and 0 or 0.25..1";
        char st[256];
        render_settings::status_text(st, sizeof(st));
        return st;
    }
    if (c == "teleport" && t.size() >= 4) {
        // TELEPORT_ACTOR(player, &pos, 0, 0, 0), exactly as RedTrainer calls it (research\mods\RedTrainer).
        std::string actor = run_native(0xE8CFDD53u, {0}, 0);  // GET_PLAYER_ACTOR(0)
        if (actor.rfind("ERROR", 0) == 0) return actor;
        uint64_t handle = static_cast<uint32_t>(std::atoi(actor.c_str() + actor.find("int ") + 4));
        float pos[3] = {std::strtof(t[1].c_str(), nullptr), std::strtof(t[2].c_str(), nullptr), std::strtof(t[3].c_str(), nullptr)};
        std::string r = run_native(0x2D54B916u, {handle, 0, 0, 0, 0}, 2, pos);
        if (t.size() >= 5) {  // SET_ACTOR_HEADING(player, heading, 0)
            uint64_t h = parse_arg(t[4] + (t[4].find('.') == std::string::npos ? ".0f" : "f"));
            run_native(0xECE8520Bu, {handle, h, 0}, 0);
        }
        return "teleported: " + r;
    }
    if (c == "pso") {
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return pso::command(line);
    }
    if (c == "descheap") {
        // The game's descriptor heap partitions (FUN_140f732b0): per type t at device +0x1f0 + t*0x40: start (int),
        // capacity +0x208 and used +0x218 (descriptors); free blocks at +0x3a8 + t*0x10. Type 6 is "ConstantBuffer";
        // used > capacity is the game's deliberate crash (write to 0x3).
        uintptr_t g = anchors::addr(anchors::Id::D3dDeviceSingleton);
        char* dev = *reinterpret_cast<char**>(g);
        if (!dev) return "ERROR no device";
        std::string out;
        char b[96];
        for (int ty = 0; ty < 8; ++ty) {
            const char* p2 = dev + ty * 0x40;
            std::snprintf(b, sizeof(b), "%st%d start %d cap %llu used %llu free %d", ty ? " | " : "", ty, *reinterpret_cast<const int*>(p2 + 0x1f0),
                          static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(p2 + 0x208)),
                          static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(p2 + 0x218)),
                          *reinterpret_cast<const int*>(dev + 0x3a8 + ty * 0x10));
            out += b;
        }
        return out;
    }
    if (c == "peekpp" && t.size() >= 4) {
        // peekpp <rva> <off1> <off2> [n]: n floats at (*(*(RDR.exe+rva) + off1)) + off2, each pointer checked readable
        auto readable = [](uintptr_t a, size_t n) {
            MEMORY_BASIC_INFORMATION mi{};
            return a && VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
                   (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                   a + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
        };
        uintptr_t a = anchors::base() + static_cast<uintptr_t>(std::strtoull(t[1].c_str(), nullptr, 16));
        if (!readable(a, 8)) return "ERROR global not readable";
        uintptr_t p1 = *reinterpret_cast<uintptr_t*>(a) + std::strtoull(t[2].c_str(), nullptr, 16);
        if (!readable(p1, 8)) return "ERROR first pointer not readable";
        uintptr_t p2 = *reinterpret_cast<uintptr_t*>(p1) + std::strtoull(t[3].c_str(), nullptr, 16);
        int n = t.size() >= 5 ? std::atoi(t[4].c_str()) : 4;
        if (n < 1 || n > 32) n = 4;
        if (!readable(p2, n * 4)) return "ERROR target not readable";
        std::string out;
        char b[32];
        for (int i = 0; i < n; ++i) {
            std::snprintf(b, sizeof(b), "%s%.4f", i ? " " : "", reinterpret_cast<const float*>(p2)[i]);
            out += b;
        }
        return out;
    }
    if (c == "peek" && t.size() >= 2) {
        // peek <rva> [n]: n (<= 32) floats at RDR.exe+rva, read only after VirtualQuery shows them committed and readable
        uintptr_t a = anchors::base() + static_cast<uintptr_t>(std::strtoull(t[1].c_str(), nullptr, 16));
        int n = t.size() >= 3 ? std::atoi(t[2].c_str()) : 16;
        if (n < 1 || n > 32) n = 16;
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
            !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) ||
            a + n * 4 > reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize)
            return "ERROR not readable";
        std::string out;
        char b[32];
        for (int i = 0; i < n; ++i) {
            std::snprintf(b, sizeof(b), "%s%.4f", i ? " " : "", reinterpret_cast<const float*>(a)[i]);
            out += b;
        }
        return out;
    }
    if (c == "stick" && t.size() >= 6) {
        auto f = [&](size_t i) { return std::strtof(t[i].c_str(), nullptr); };
        xinput::press_sticks(f(1), f(2), f(3), f(4), static_cast<uint32_t>(parse_arg(t[5])));
        return "holding";
    }
    if (c == "pad" && t.size() >= 3) {
        xinput::press(static_cast<uint16_t>(parse_arg(t[1])), static_cast<uint32_t>(parse_arg(t[2])));
        return "pressed";
    }
    if (c == "forceremove") return diag::force_device_removal() ? "removing a WARP test device" : "ERROR could not start the removal test";
    if (c == "sample") {  // where the busy threads are: "[sample]" lines (RIP histograms; no unwind)
        return diag::sample_threads(t.size() >= 2 ? std::atoi(t[1].c_str()) : 20);
    }
    if (c == "recdump") {  // the hang's recorder dump now (reads only): "[hang] rec ..." lines in the log
        diag::dump_recorder();
        return "logged the recorder's state ([hang] rec lines)";
    }
    if (c == "dred") {
        diag::dump_dred("requested");
        return "dumped";
    }
    if (c == "dumptext") return game_hooks::dump_text_region("requested") ? "written" : "ERROR";
    if (c == "reload") {
        config::load();
        return "reloaded";
    }
    return "ERROR unknown command '" + c + "'";
}

void json_escape(const char* in, char* out, size_t len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < len; ++i) {
        char ch = in[i];
        if (ch == '"' || ch == '\\') out[j++] = '\\';
        out[j++] = (ch >= 0x20) ? ch : ' ';
    }
    out[j] = 0;
}

void write_status() {
    char res[2048], fg[256], census[512], hooks_txt[2048], hooks_esc[2048], rt[160], ring_txt[320], ring[400], cam[400], rset[256],
        mchk[900], dual_txt[768], dual[900], taa_txt[400];
    json_escape(g_result.c_str(), res, sizeof(res));
    json_escape(d3d::framegraph_status(), fg, sizeof(fg));
    game_hooks::census_text(census, sizeof(census));
    hooks::describe(hooks_txt, sizeof(hooks_txt));
    json_escape(hooks_txt, hooks_esc, sizeof(hooks_esc));
    json_escape(xr::runtime_name(), rt, sizeof(rt));
    ring_probe::status_text(ring_txt, sizeof(ring_txt));
    json_escape(ring_txt, ring, sizeof(ring));
    camera_lever::status_text(cam, sizeof(cam));
    render_settings::status_text(rset, sizeof(rset));
    ring_probe::matrix_text(mchk, sizeof(mchk));
    dual_pass::status_text(dual_txt, sizeof(dual_txt));
    json_escape(dual_txt, dual, sizeof(dual));
    char xsub[1024], vmode[256];
    vr_mode::status_text(vmode, sizeof(vmode));
    xr::submit_status(xsub, sizeof(xsub));
    taa::status_text(taa_txt, sizeof(taa_txt));
    char buf[16384];  // every field at its buffer's length fits (the XR status is up to 1024)
    int n = std::snprintf(
        buf, sizeof(buf),
        "{\n  \"pid\": %lu,\n  \"uptime_ms\": %.0f,\n  \"cmd_seq\": %ld,\n  \"cmd_result\": \"%s\",\n"
        "  \"anchors_ok\": %s,\n  \"hooks\": %d,\n  \"hook_names\": \"%s\",\n"
        "  \"presents\": %llu,\n  \"cpu_frame_ms\": %.3f,\n  \"gpu_frame_ms\": %.3f,\n  \"gpu_span_ms\": %.3f,\n"
        "  \"gpu_submissions_timed\": %u,\n"
        "  \"swapchain\": \"%ux%u fmt %u x%u\",\n  \"queues\": \"direct %u compute %u copy %u\",\n"
        "  \"frame_ecl\": \"calls %u (present queue %u) lists %u (direct %u compute %u copy %u)\",\n"
        "  \"set_current_per_frame\": %u,\n  \"push_globals_per_frame\": %u,\n  \"census\": \"%s\",\n"
        "  \"device_removed\": %s,\n  \"dred\": %s,\n  \"framegraph\": \"%s\",\n"
        "  \"script_ticks\": %llu,\n  \"plugin_attached\": %s,\n  \"xinput_polls\": %llu,\n  \"xinput_injected\": %llu,\n"
        "  \"xinput_caps\": %llu,\n  \"xinput_rumble\": \"%#010x (%llu changes)\",\n  \"ring\": \"%s\",\n  \"cam\": \"%s\",\n  \"removal_test\": \"%s\",\n  \"render\": \"%s\",\n  \"mcheck\": \"%s\",\n  \"dual\": \"%s\",\n"
        "  \"taa\": \"%s\",\n  \"xr_state\": \"%s\",\n  \"xr_frames\": %llu,\n  \"xr_runtime\": \"%s\",\n  \"xr_submit\": \"%s\",\n  \"mode\": \"%s\"\n}\n",
        GetCurrentProcessId(), log::now_ms(), g_seq, res, anchors::stand_down() ? "false" : "true", hooks::count(), hooks_esc,
        static_cast<unsigned long long>(state::presents.load()), state::cpu_frame_ms.load(), state::gpu_frame_ms.load(),
        state::gpu_span_ms.load(), state::gpu_brackets.load(),
        state::swap_width.load(), state::swap_height.load(), state::swap_format.load(), state::swap_buffers.load(),
        state::queues_direct.load(), state::queues_compute.load(), state::queues_copy.load(), state::frame_ecl_calls.load(),
        state::frame_ecl_present.load(), state::frame_ecl_lists.load(), state::frame_ecl_direct.load(), state::frame_ecl_compute.load(),
        state::frame_ecl_copy.load(), state::frame_set_current.load(), state::frame_push_globals.load(), census,
        state::device_removed ? "true" : "false", state::dred_enabled ? "true" : "false", fg,
        static_cast<unsigned long long>(api::script_ticks()), api::plugin_attached() ? "true" : "false",
        static_cast<unsigned long long>(xinput::polls()), static_cast<unsigned long long>(xinput::injected()),
        static_cast<unsigned long long>(xinput::caps_queries()), xinput::rumble(),
        static_cast<unsigned long long>(xinput::rumble_changes()), ring, cam, diag::removal_test_status(), rset, mchk, dual, taa_txt, xr::session_state(), static_cast<unsigned long long>(xr::frames()), rt, xsub, vmode);
    if (n <= 0) return;
    HANDLE f = CreateFileW(g_status_tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, buf, static_cast<DWORD>(n < static_cast<int>(sizeof(buf)) ? n : sizeof(buf) - 1), &w, nullptr);
    CloseHandle(f);
    MoveFileExW(g_status_tmp, g_status_path, MOVEFILE_REPLACE_EXISTING);
}

bool read_command(long* seq, std::string* text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_cmd_path, L"rb") != 0 || !f) return false;
    char line[1024] = {};
    std::fgets(line, sizeof(line), f);
    std::fclose(f);
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
    char* rest = nullptr;
    long s = std::strtol(line, &rest, 10);
    if (rest == line) return false;
    *seq = s;
    while (*rest == ' ') ++rest;
    *text = rest;
    return true;
}

DWORD WINAPI worker(void*) {
    long seq = 0;
    std::string text;
    if (read_command(&seq, &text)) {
        g_seq = seq;  // adopt: a command left from an earlier run is never executed
        log::info("[cmd] adopted existing command seq %ld (\"%s\") without running it", seq, text.c_str());
    }
    ULONGLONG last_status = 0;
    for (;;) {
        Sleep(50);
        if (read_command(&seq, &text) && seq > g_seq) {
            g_seq = seq;
            std::vector<std::string> tokens = split(text);
            double t0 = log::now_ms();
            g_result = tokens.empty() ? "ERROR empty command" : execute(tokens);
            log::info("[cmd] %ld \"%s\" -> %s (%.0f ms)", seq, text.c_str(), g_result.c_str(), log::now_ms() - t0);
            last_status = 0;
        }
        if (GetTickCount64() - last_status >= 250) {
            write_status();
            last_status = GetTickCount64();
        }
    }
}

}  // namespace

void start() {
    log::path_in_game_dir(L"RDRVR_cmd.txt", g_cmd_path, MAX_PATH);
    log::path_in_game_dir(L"RDRVR_status.json", g_status_path, MAX_PATH);
    log::path_in_game_dir(L"RDRVR_status.json.tmp", g_status_tmp, MAX_PATH);
    if (HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) {
        SetThreadDescription(t, L"RDRVR test channel");
        CloseHandle(t);
    }
}

}  // namespace rdrvr::test_channel
