#include "core/test_channel.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
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
#include "core/scope.h"
#include "core/body.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/diag.h"
#include "core/dual_pass.h"
#include "core/frame_probe.h"
#include "core/playback.h"
#include "core/eye_grab.h"
#include "core/eye_shape.h"
#include "core/frame_grab.h"
#include "core/game_hooks.h"
#include "core/holster.h"
#include "core/aim.h"
#include "core/reload.h"
#include "core/actions.h"
#include "core/dlss.h"
#include "core/deadeye.h"
#include "core/dual.h"
#include "core/physics.h"
#include "core/gestures.h"
#include "core/offhand.h"
#include "core/pgo.h"
#include "core/probe.h"
#include "core/hooks.h"
#include "core/image_mem.h"
#include "core/log.h"
#include "core/lum_check.h"
#include "core/gpu_stages.h"
#include "core/eye2_collector.h"
#include "core/eye2_images.h"
#include "core/main_instanced.h"
#include "core/main_region.h"
#include "core/eye2_light.h"
#include "core/eye2_velocity.h"
#include "core/pass2_skip.h"
#include "core/fcensus.h"
#include "core/prepass_arrays.h"
#include "core/pso.h"
#include "core/rec_state.h"
#include "core/cut_velocity.h"
#include "core/inst_prev_mirror.h"
#include "core/prevworld_mirror.h"
#include "core/render_settings.h"
#include "core/align_grab.h"
#include "core/ring_probe.h"
#include "core/state.h"
#include "core/taa.h"
#include "core/xinput.h"
#include "core/wheel.h"
#include "core/whistle.h"
#include "core/hogtie.h"
#include "core/bgfocus.h"
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

// bench act (run 16 item 3, the input-only rule): while on, execute() answers only the forms below (read-only queries and
// status) and refuses every other command before it runs, so a scenario's act() moves the player only through the
// simulator's head, hands and buttons. Test only: off at every start, never read from the ini, never saved.
// tests\bench\ALLOWLIST.md lists every command and why each form is here.
std::atomic<bool> g_bench_act{false};
std::atomic<uint64_t> g_bench_refused{0};
std::string g_bench_last = "none";  // the worker thread's only (execute runs there alone)

// each form is the command's tokens exactly: "*" one token of any value, a final "..." any further tokens (or none)
constexpr const char* kBenchAllow[] = {
    "ping", "status", "log ...", "pos", "census", "recdump",
    // holster: the status forms, no argument that sets
    "holster", "holster zones", "holster bases", "holster slots", "holster weapons", "holster gun", "holster load", "holster fore",
    "holster fore trace", "holster grab", "holster anchor", "holster guns", "holster rings", "holster stock", "holster turret",
    "holster pieces", "holster model",
    // aim: the reports (aim::command matches substrings, so only these exact lines)
    "aim", "aim frames", "aim reticle", "aim reticle stats", "aim ledge", "aim armblock", "aim cover", "aim executions", "aim spawns",
    "props", "props diag", "hogtie", "actions", "actions all", "gestures", "gunmelee", "ammo", "dual", "physics", "round", "sound",
    "whistle", "wheel", "deadeye", "skel stock", "skel two",
    "pose", "pose crouch", "pose room", "mode", "perf", "prepass", "prepass views", "prepass cull", "prepass wvp", "prepass instprev",
    "controls", "controllers", "rings", "menu", "hud", "xr status", "xr sharpen", "taa", "taa status", "cuts", "dlss", "cam audio",
    "cam audio log", "cam fp", "renderres", "eyeshape", "eyeshape status", "playback", "shadows", "sun", "drops",
    "grabeyes *",  // an eye capture: observation
    // memory reads checked committed, readable and not a guard page, under SEH (peek and peekpp skip the guard check, and
    // peekabs reads without SEH after its check: refused, the review)
    "peekx * *", "peekx * * *",
};

// native <hash> ...: only these pure getters (names from extern\redhook-sdk\include\Natives.h)
constexpr uint32_t kBenchNatives[] = {
    0xE8CFDD53u,  // GET_PLAYER_ACTOR
    // (not GET_POSITION: it writes a Vector3 through its second argument, so a number there is a write anywhere: the
    // review; the read-only "pos" gives the player's)
    0x42DE39F0u,  // GET_HEADING
    0xAA364907u,  // GET_LASSO_TARGET
    0xA610DC79u,  // IS_ACTOR_HOGTIED
    0x0D798FFEu,  // IS_ACTOR_DEAD
    0x2F232639u,  // IS_ACTOR_ALIVE
    0xF246F15Du,  // GET_ACTOR_HEALTH
    0x44787A58u,  // GET_ACTOR_KO_POINTS
    0x0B28E9ECu,  // GET_ACTOR_ENUM
    0x7DF72579u,  // STREAMING_IS_ACTOR_LOADED
    0xA4B2016Du,  // GET_WEAPON_IN_HAND
    0x42C0FAAAu,  // GET_WEAPON_EQUIPPED
    0x118D085Eu,  // GET_NUM_WEAPONS_IN_INVENTORY
    0xA677B204u,  // GET_WEAPON_MAX_AMMO
    0x17883570u,  // GET_AMMOENUM_FOR_WEAPONENUM
    0x8DDE894Fu,  // GET_ACTORS_HORSE
    0xA0936EB6u,  // GET_VEHICLE
    0x2C0F211Du,  // GET_LAST_ATTACKER
    0x08308EBAu,  // GET_LAST_HIT_FLAGS
    0xF6BF4242u,  // IS_ACTOR_CROUCHING
    0x3918D335u,  // IS_ACTOR_RAGDOLL
    0x882C84DCu,  // IS_ACTOR_HUMAN
    0xDC99C124u,  // IS_ACTOR_DRIVING_VEHICLE
    0xD39C4A9Eu,  // IS_ACTOR_USING_COVER
    0x3612AC73u,  // IS_ACTOR_WHISTLING
    0x2C5983E0u,  // IS_USING_TURRET
    0x57246C02u,  // IS_GAME_PAUSED
    0xA61FA36Bu,  // CUTSCENE_MANAGER_IS_CUTSCENE_PLAYING
    // 2026-10-27 ([Holsters] WheelPause, research\run27\wheelpause.md 1.1): the game timer's clocks, no arguments, each
    // handler one float from the timer 0x142314d00 into the return slot (read: 0x140202e10 +0x3c, 0x140202e20 +0x44,
    // 0x140202ef0 +0x08), for the time ratio game / unwarped
    0x5842B9D1u,  // GET_CURRENT_GAME_TIME
    // (GET_CURRENT_UNWARPED_TIME 0xF83666A6 taken out again: cycle r167 read garbage from it, 0x7ffa... values)
    0x59466B4Du,  // GET_LAST_FRAME_TIME
};

bool bench_form(const char* form, const std::vector<std::string>& t) {
    size_t i = 0;
    for (const char* p = form;;) {
        while (*p == ' ') ++p;
        if (!*p) return i == t.size();
        const char* e = p;
        while (*e && *e != ' ') ++e;
        const size_t n = static_cast<size_t>(e - p);
        if (n == 3 && std::strncmp(p, "...", 3) == 0) return true;
        if (i >= t.size()) return false;
        if (!(n == 1 && *p == '*') && (t[i].size() != n || std::strncmp(p, t[i].c_str(), n) != 0)) return false;
        ++i;
        p = e;
    }
}

bool bench_allowed(const std::vector<std::string>& t) {
    if (t[0] == "native" && t.size() >= 2) {
        // the hash as execute() reads it (parse_arg: a plain number); a string, pair or float form is refused
        const char* s = t[1].c_str();
        char* end = nullptr;
        const unsigned long long h = std::strtoull(s, &end, 0);
        if (end == s || *end || h > 0xffffffffull) return false;
        // the arguments plain integers only (no s: or p: forms, no floats): a getter is given handles and enums, never a
        // pointer to write through (the review)
        for (size_t i = 2; i < t.size(); ++i) {
            const char* a = t[i].c_str();
            char* ae = nullptr;
            (void)std::strtoll(a, &ae, 0);
            if (ae == a || *ae) return false;
        }
        for (uint32_t k : kBenchNatives)
            if (k == static_cast<uint32_t>(h)) return true;
        return false;
    }
    for (const char* f : kBenchAllow)
        if (bench_form(f, t)) return true;
    return false;
}

std::string join(const std::vector<std::string>& t) {
    std::string s;
    for (size_t i = 0; i < t.size(); ++i) s += (i ? " " : "") + t[i];
    return s;
}

std::string bench_command(const std::vector<std::string>& t) {
    if (t.size() >= 2 && t[1] == "clock") {
        // bench clock: the log's clock (QPC since the mod's start) beside the raw QPC in ms, read together, so a script maps
        // the simulator's raw QPC times onto the log's
        LARGE_INTEGER f{}, a{}, b{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const double lm = log::now_ms();
        QueryPerformanceCounter(&b);
        if (f.QuadPart <= 0) return "ERROR no QPC frequency";
        const double qm = (static_cast<double>(a.QuadPart) + static_cast<double>(b.QuadPart)) * 0.5 * 1000.0 / static_cast<double>(f.QuadPart);
        char st[128];
        std::snprintf(st, sizeof(st), "bench clock: log %.3f ms, qpc %.3f ms", lm, qm);
        return st;
    }
    if (t.size() >= 2 && t[1] == "act") {  // bench act [on|off]
        if (t.size() >= 3 && (t[2] == "on" || t[2] == "off")) {
            g_bench_act.store(t[2] == "on", std::memory_order_relaxed);
            log::info("[bench] act %s", t[2].c_str());
        } else if (t.size() >= 3) {
            return "ERROR usage: bench act [on|off]";
        }
        char st[1200];
        std::snprintf(st, sizeof(st), "bench act %s, refused %llu (last: %s)", g_bench_act.load(std::memory_order_relaxed) ? "on" : "off",
                      static_cast<unsigned long long>(g_bench_refused.load(std::memory_order_relaxed)), g_bench_last.c_str());
        return st;
    }
    return "ERROR usage: bench act [on|off] | bench clock";
}

std::string execute(const std::vector<std::string>& t) {
    const std::string& c = t[0];
    if (c == "bench") return bench_command(t);  // answered while acting too (act off, act, clock)
    if (g_bench_act.load(std::memory_order_relaxed) && !bench_allowed(t)) {
        g_bench_last = join(t);
        g_bench_refused.fetch_add(1, std::memory_order_relaxed);
        log::info("[bench] refused %s", g_bench_last.c_str());
        return "REFUSED (bench): " + g_bench_last;
    }
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
    if (c == "cam" && t.size() >= 3 && t[1] == "audio" && t[2] == "log")  // cam audio log on|off|mark <n>|dump <file> (run 12 item 10)
        return camera_lever::audio_log_command(t.size() >= 4 ? t[3] : "", t.size() >= 5 ? t[4] : "");
    if (c == "cam" && t.size() >= 2 && t[1] == "audio") {  // cam audio [on|off] | fold on|off: AudioFollowsHead, AudioRearFold (the session)
        if (t.size() >= 3 && (t[2] == "on" || t[2] == "off")) camera_lever::set_audio_follows_head(t[2] == "on");
        if (t.size() >= 4 && t[2] == "fold" && (t[3] == "on" || t[3] == "off")) camera_lever::set_audio_rear_fold(t[3] == "on");
        return camera_lever::audio_status();
    }
    if (c == "cam" && t.size() >= 2 && t[1] == "heading") {  // (2026-11-06, GOAL-remaining 6c) the game camera's heading, degrees
        float bx = 0.0f, bz = 0.0f;
        if (!camera_lever::game_heading(&bx, &bz)) return "ERROR no game camera yet";
        char b[96];
        float vh = 0.0f;
        const bool vok = camera_lever::view_heading(&vh);
        char b2[160];
        std::snprintf(b2, sizeof(b2), "game camera heading %.2f deg (back row %.4f %.4f) | view heading %s%.2f, the view's yaw %.2f", std::atan2(bx, bz) * 57.2957795f,
                      bx, bz, vok ? "" : "(none) ", vh, camera_lever::view_yaw_offset());
        return b2;
    }
    if (c == "cam" && t.size() >= 3 && t[1] == "viewyaw") {  // cam viewyaw <deg>: the view's yaw over the game camera (6c's test)
        camera_lever::set_view_yaw_offset(std::strtof(t[2].c_str(), nullptr));
        return "view yaw " + t[2];
    }
    if (c == "cam" && t.size() >= 2 && t[1] == "fp") {  // cam fp [on|off|mark|check]: [Body] FirstPersonMoments (the session)
        if (t.size() >= 3 && (t[2] == "on" || t[2] == "off")) camera_lever::set_first_person_moments(t[2] == "on");
        if (t.size() >= 3 && (t[2] == "mark" || t[2] == "check")) return camera_lever::fp_check(t[2] == "mark");
        return camera_lever::fp_status();
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
                   "sunvis hold shadows shadowunion)";
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
    if (c == "postfx") {  // postfx [blur off|game] [haze on|off]: run 10 items 8c and 8d (the session only)
        for (size_t i = 1; i + 1 < t.size(); i += 2) {
            if (t[i] == "blur") render_settings::set_motion_blur_off(t[i + 1] == "off", false);
            if (t[i] == "haze") render_settings::set_heat_haze(t[i + 1] == "on", false);
            if (t[i] == "style" || t[i] == "hazetest") render_settings::post_test(t[i].c_str(), std::strtof(t[i + 1].c_str(), nullptr));
        }
        char st[400];
        render_settings::post_text(st, sizeof(st));
        return st;
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
    if (c == "grabburst" && t.size() >= 3) {  // grabburst <prefix> <frames> [crop_w crop_h] [post|scene|vel] [pereye]
        const int src = t.size() >= 6 ? (t[5] == "scene" ? 1 : t[5] == "vel" ? 2 : 0) : 0;
        const bool pereye = t.size() >= 7 && t[6] == "pereye";  // (scene/vel: the first eye at pass 1's bind, run 19)
        return burst_grab::grab(t[1], std::atoi(t[2].c_str()), t.size() >= 5 ? std::atoi(t[3].c_str()) : 640,
                                t.size() >= 5 ? std::atoi(t[4].c_str()) : 360, 10000, src, pereye);
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
    if (c == "xr" && t.size() >= 2 && t[1] == "sharpen") {  // xr sharpen [0..1]: [XR] Sharpness for the session (run 14)
        if (t.size() >= 3) xr_blit::set_sharpness(static_cast<float>(std::atof(t[2].c_str())), false);
        char st[64];
        std::snprintf(st, sizeof(st), "sharpening %.2f", xr_blit::sharpness());
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
        // menu | menu on|off | menu find <label> | menu scroll <y> | menu grab <path.bmp> | menu desktop on|off
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
        if (t.size() >= 2 && t[1] == "desktop") {  // (2026-10-29: the session only; "menu desktop" alone: the status)
            if (t.size() >= 3) menu::set_desktop(t[2] == "on");
        } else if (t.size() >= 2) {
            menu::set_visible(t[1] == "on");
        }
        char st[512];
        menu::status_text(st, sizeof(st));
        return st;
    }
    if (c == "pose" && t.size() >= 3 && t[1] == "tpsnapangle") {  // pose tpsnapangle <deg>: ThirdPersonSnapAngle for the session (round 98)
        pose::set_third_person_snap_angle(std::strtof(t[2].c_str(), nullptr), false);
        return "third person snap angle " + std::to_string(static_cast<int>(pose::third_person_snap_angle()));
    }
    if (c == "pose" && t.size() >= 2 && t[1] == "tpsnap") return pose::tps_command(t.size() >= 3 ? t[2] : "");
    if (c == "pose" && (t.size() == 1 || t[1] != "xr" && t[1] != "off")) {
        // pose | pose anchor on|off  ("pose xr" / "pose off" are the eye-camera switch above)
        if (t.size() >= 3 && t[1] == "anchor") pose::set_anchor(t[2] == "on");
        if (t.size() >= 3 && t[1] == "orient") pose::set_orient_mode(std::atoi(t[2].c_str()));
        // pose saddle on|off [seconds]: the saddle anchor this session (not saved); pose horse stick|head writes the ini
        if (t.size() >= 4 && t[1] == "saddle" && t[2] == "climb") pose::set_saddle_climb(t[3] == "on", false);  // pose saddle climb on|off
        else if (t.size() >= 3 && t[1] == "saddle")
            pose::set_saddle(t[2] == "on", t.size() >= 4 ? std::strtof(t[3].c_str(), nullptr) : pose::saddle_smoothing(), false);
        if (t.size() >= 3 && t[1] == "trace") return pose::trace_command(t[2], t.size() >= 4 ? t[3] : "");  // pose trace on|off|reset|dump <file>
        if (t.size() >= 2 && t[1] == "bodycam") {  // pose bodycam [on|off]: [Horse] BodyWithCamera for the session (run 12 item 8)
            if (t.size() >= 3 && (t[2] == "on" || t[2] == "off")) body::set_body_with_camera(t[2] == "on");
            return std::string("body with camera ") + (body::body_with_camera() ? "on" : "off");
        }
        if (t.size() >= 3 && t[1] == "horse") pose::set_steer_by_head(t[2] == "head");
        if (t.size() >= 3 && t[1] == "jitter") pose::reset_ride_jitter();
        if (t.size() >= 3 && t[1] == "keepcam") pose::set_keep_camera(t[2] == "on");
        if (t.size() >= 3 && t[1] == "rideturn") pose::set_stick_turn_riding(t[2] == "on", false);  // run 9 item 4: the session only
        if (t.size() >= 3 && t[1] == "vehicle") pose::set_vehicle_heading(t[2] == "on", false);    // run 10 item 3: the session only
        if (t.size() >= 3 && t[1] == "armed") pose::set_armed_stand(t[2] == "on", false);          // run 10 item 8b: the session only
        if (t.size() >= 2 && t[1] == "room") {
            // 2026-10-27 ([XR] RoomScale, the session only): pose room [on|off | fade on|off | judder reset | tune <dead zone>
            // <stop radius> <min stick> <max stick> <max offset>]: its status (the offset, the walk, blocked, the fade, the take-off)
            if (t.size() >= 3 && (t[2] == "on" || t[2] == "off")) pose::set_room_scale(t[2] == "on", false);
            if (t.size() >= 4 && t[2] == "fade") pose::set_room_fade(t[3] == "on", false);
            if (t.size() >= 4 && t[2] == "judder") camera_lever::room_reset_judder();
            if (t.size() >= 4 && t[2] == "take") pose::set_room_take(t[3] == "on");  // pose room take on|off: the judder's negative control
            // (2026-10-28) pose room quick <lean slack m> <start lead s>: the lean's slack and the early start (0 0: as before)
            if (t.size() >= 4 && t[2] == "direct") pose::set_room_direct(t[3] == "on", false);  // (2026-11-02) pose room direct on|off
            if (t.size() >= 5 && t[2] == "quick") pose::set_room_response(std::strtof(t[3].c_str(), nullptr), std::strtof(t[4].c_str(), nullptr), false);
            if (t.size() >= 8 && t[2] == "tune")
                pose::set_room_tuning(std::strtof(t[3].c_str(), nullptr), std::strtof(t[4].c_str(), nullptr), std::strtof(t[5].c_str(), nullptr),
                                      std::strtof(t[6].c_str(), nullptr), std::strtof(t[7].c_str(), nullptr), false);
            return pose::room_status();
        }
        if (t.size() >= 2 && t[1] == "crouch") {
            // 2026-10-27 ([XR] PhysicalCrouch, the session only): pose crouch [on|off | calibrate [ms] | tune <crouch share>
            // <stand share> <hold ms> <standing m> [quick hold ms]]: its status (the drop, H, y_stand, want/have/owned, the presses)
            if (t.size() >= 3 && (t[2] == "on" || t[2] == "off")) pose::set_physical_crouch(t[2] == "on", false);
            if (t.size() >= 3 && t[2] == "calibrate") pose::calibrate_standing(t.size() >= 4 ? std::strtod(t[3].c_str(), nullptr) : 0.0);
            if (t.size() >= 7 && t[2] == "tune")
                pose::set_crouch_tuning(std::strtof(t[3].c_str(), nullptr), std::strtof(t[4].c_str(), nullptr), std::strtof(t[5].c_str(), nullptr),
                                        std::strtof(t[6].c_str(), nullptr), false, t.size() >= 8 ? std::strtof(t[7].c_str(), nullptr) : -1.0f);
            return pose::crouch_status();
        }
        if (t.size() >= 2 && t[1] == "bodyturn") {
            // 2026-10-27 ([XR] BodyFollowsTurn, the session only): pose bodyturn [on|off | reset | tune <dead zone deg> <hold ms>
            // <speed deg/s> [<gain /s> <ease s>] | walk on|off]: its status (the gate, the head over the body, following, the origin's match, the view's yaw and
            // its largest step with the head still; reset zeroes that step)
            if (t.size() >= 3 && (t[2] == "on" || t[2] == "off")) pose::set_body_follows_turn(t[2] == "on", false);
            if (t.size() >= 3 && t[2] == "reset") camera_lever::body_turn_reset_view();
            if (t.size() >= 6 && t[2] == "tune")
                pose::set_body_turn_tuning(std::strtof(t[3].c_str(), nullptr), std::strtof(t[4].c_str(), nullptr), std::strtof(t[5].c_str(), nullptr), false,
                                           t.size() >= 8 ? std::strtof(t[6].c_str(), nullptr) : -1.0f, t.size() >= 8 ? std::strtof(t[7].c_str(), nullptr) : -1.0f);
            if (t.size() >= 4 && t[2] == "walk") pose::set_body_turn_during_walk(t[3] == "on", false);  // (2026-10-28) the walk's gate lifted
            return pose::body_turn_status();
        }
        if (t.size() >= 3 && t[1] == "turn")  // pose turn snap|smooth [speed] [snap angle]
            pose::set_turning(t[2] == "snap", t.size() >= 4 ? std::strtof(t[3].c_str(), nullptr) : pose::turn_speed(),
                              t.size() >= 5 ? std::strtof(t[4].c_str(), nullptr) : pose::snap_angle());
        char st[700];
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
    if (c == "hud") {  // hud wrist|quad (writes the user ini) | hud shape rect|radar|half (the session) | hud nudge <right%> <down%> <size%> (the session)
        if (t.size() >= 3 && t[1] == "shape") xr::hud_shape_test(t[2] == "rect" ? 0 : t[2] == "half" ? 2 : 1);
        else if (t.size() >= 2 && (t[1] == "nudge" || t[1] == "shape")) {  // (too few values: the status only, never the wrist switch)
            if (t.size() >= 5 && t[1] == "nudge") xr::set_hud_radar_nudge(std::strtof(t[2].c_str(), nullptr), std::strtof(t[3].c_str(), nullptr), std::strtof(t[4].c_str(), nullptr), false);
        } else if (t.size() >= 2) xr::set_hud_on_wrist(t[1] == "wrist");
        char st[512];
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
        if (t.size() >= 3 && t[1] == "inst") held_prop::set_inst_check(t[2] == "on");  // run 10 item 2's control
        if (t.size() >= 3 && t[1] == "gate") held_prop::set_gate(std::strtof(t[2].c_str(), nullptr));  // run 10 item 2 (the session)
        if (t.size() >= 3 && t[1] == "puts") held_prop::set_puts(std::atoi(t[2].c_str()));  // 2026-10-12 (the session)
        if (t.size() >= 3 && t[1] == "twins") held_prop::set_twins(t[2] == "on");  // 2026-10-12 (the session)
        if (t.size() >= 3 && t[1] == "census" && (t[2] == "world" || t[2] == "own")) held_prop::set_census_matched(t[2] == "own");  // item 13's test
        if (t.size() >= 3 && t[1] == "show") {
            // run 11 item 1 (the props census): props show <fragment|off> [distance m] [height m] [spin deg] [tilt deg]: the
            // fragment made in the census slot before John (his heading, level), its axes X right, Y forward, Z up turned
            // by the spin about up and the tilt about right; the object's own angles set too (a prop whose draws are not
            // matched is drawn where it is placed, in its own turn)
            // the name kept in one of eight fixed buffers, used in turn: held_prop keeps the pointer (a static string), so
            // an older name stays valid while the game thread may still read it
            static char names[8][64];
            static int next_name = 0;
            if (t[2] == "off") {
                held_prop::want(held_prop::kCensusSlot, nullptr, nullptr);
                held_prop::want_angles(held_prop::kCensusSlot, nullptr);
                return "census off | " + held_prop::status();
            }
            RdrvrActorState st{};
            if (!api::actor_state(&st)) return "props show: no actor";
            char* frag = names[next_name];
            next_name = (next_name + 1) % 8;
            std::snprintf(frag, sizeof(names[0]), "%s", t[2].c_str());
            const float dist = t.size() >= 4 ? std::strtof(t[3].c_str(), nullptr) : 1.2f;
            const float up_m = t.size() >= 5 ? std::strtof(t[4].c_str(), nullptr) : 1.1f;
            const float spin = t.size() >= 6 ? std::strtof(t[5].c_str(), nullptr) : 0.0f;
            const float tilt = t.size() >= 7 ? std::strtof(t[6].c_str(), nullptr) : 0.0f;
            const float h = st.heading_deg * 0.0174532925f, h2 = (st.heading_deg + spin) * 0.0174532925f, tr = tilt * 0.0174532925f;
            const float f[3] = {-std::sin(h), 0.0f, -std::cos(h)};
            const float r2[3] = {std::cos(h2), 0.0f, -std::sin(h2)}, f2[3] = {-std::sin(h2), 0.0f, -std::cos(h2)};
            float pz[12];
            for (int k = 0; k < 3; ++k) {
                const float upk = k == 1 ? 1.0f : 0.0f;
                pz[k] = r2[k];
                pz[3 + k] = f2[k] * std::cos(tr) + upk * std::sin(tr);
                pz[6 + k] = upk * std::cos(tr) - f2[k] * std::sin(tr);
                pz[9 + k] = st.pos[k] + f[k] * dist + upk * up_m;
            }
            const float ang[3] = {tilt, st.heading_deg + spin, 0.0f};
            held_prop::want(held_prop::kCensusSlot, frag, pz);
            held_prop::want_angles(held_prop::kCensusSlot, ang);
            char b[200];
            std::snprintf(b, sizeof(b), "census %s at (%.2f %.2f %.2f), spin %.0f tilt %.0f | ", frag, pz[9], pz[10], pz[11], spin, tilt);
            return b + held_prop::status();
        }
        return held_prop::status();
    }
    if (c == "deadeye") {  // deadeye [level 0-3] [sepia on|off] [marks on|off] [barrel on|off] [deg d] [pitch px] [shift x y]: the session, the state
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return deadeye::command(line);
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
    if (c == "bgfocus") {  // bgfocus [on|off]: [Debug] BackgroundFocus (test only), its counts
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return bgfocus::command(line);
    }
    if (c == "hogtie") {  // hogtie [on|off]: [Gestures] HogtieClearProps for the session, and the state
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return hogtie::command(line);
    }
    if (c == "whistle" || c == "wheel") {  // whistle [on|off] [offset r u f] [radius m] [gunhand on|off] [reset]; wheel [on|off] [angle deg] [flipy on|off] [reset] (the session)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return c == "whistle" ? whistle::command(line) : wheel::command(line);
    }
    if (c == "offhand") {  // offhand [on|off] [dry on|off] [reset]: [Hands] OffHandItems (the session), each hand's item, the stab
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return offhand::command(line);
    }
    if (c == "pgo") {  // pgo [set <key> <value>] [save] [reload]: Ped Gunfight Overhaul's settings (the Gunfights page's)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return pgo::command(line);
    }
    if (c == "probe" && t.size() >= 2 && (t[1] == "snap" || t[1] == "fuse" || t[1] == "fuseinfo")) {  // test-only game calls
        // (research/run33/probes_brief.md; "probe on|off", the frame probe's timers, below)
        std::string line;
        for (size_t i = 0; i < t.size(); ++i) line += (i ? " " : "") + t[i];
        return probe::command(line);
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
        if (t.size() >= 2 && t[1] == "fists") {  // controls fists [on|off]: [Gestures] FistsBothTriggers (the session only)
            if (t.size() >= 3) controls::set_fists_both_triggers(t[2] == "on", false);
            return controls::fists_status();
        }
        if (t.size() >= 3 && t[1] == "raised") controls::set_aim_when_raised(t[2] == "on", false);  // the session only
        if (t.size() >= 3 && t[1] == "wristsatchel") controls::set_wrist_satchel(t[2] == "on", false);  // the session only
        if (t.size() >= 3 && t[1] == "drawhand") controls::set_draw_to_grabbing_hand(t[2] == "on", false);  // the session only
        // controls gunhand left|right|layout: the hand the gun in hand belongs to, as a draw by that hand would set it (a
        // test's native give is the game's own equip, which goes to the layout's hand: reg4's left-hand long guns)
        if (t.size() >= 3 && t[1] == "gunhand") controls::set_draw_hand(t[2] == "left" ? 0 : t[2] == "right" ? 1 : -1, -1, true);
        if (t.size() >= 3 && t[1] == "clickbrake") controls::set_click_brake_session(t[2] == "on");
        if (t.size() >= 3 && t[1] == "sprint") controls::set_sprint_drops_aim(t[2] == "on", false);  // this session only
        if (t.size() >= 3 && t[1] == "jump") controls::set_jump_drops_aim(t[2] == "on", false);  // run 7 item 1f, this session only
        if (t.size() >= 2 && t[1] == "reset") controls::reset_mapping();
        std::string r = "map:";
        for (int s = 0; s < controls::kSources; ++s)
            r += std::string(" ") + controls::source_key(s) + "=" + controls::target_name(controls::mapping(s));
        char b[1400];
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
    if (c == "shadows") {  // the shared shadow passes' state (dual_pass.h)
        char st[400];
        dual_pass::shadow_status(st, sizeof(st));
        return st;
    }
    if (c == "framelimit") {  // framelimit [off|on]: the game's own frame limiter, in memory (frame_probe.h)
        char st[200];
        frame_probe::game_frame_limit(t.size() >= 2 ? t[1].c_str() : "", st, sizeof(st));
        return st;
    }
    if (c == "probe" && t.size() >= 2) {  // probe on|off: the single-pass probe's timers (frame_probe.h)
        return frame_probe::set_on(t[1] == "on") ? "probe " + t[1] : "ERROR the probe is not installed ([XR] TimingProbe=0)";
    }
    if (c == "perf") {
        if (t.size() >= 2 && t[1] == "reset") xr::perf_reset();
        char st[400], pr[768], vq[400];
        xr::perf_status(st, sizeof(st));
        xr::perf_probe(pr, sizeof(pr));
        xr::perf_vq(vq, sizeof(vq));  // (2026-10-29) the checked reads' VirtualQuery over the window (image_mem.h)
        return std::string(st) + " || " + pr + " || " + vq;
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
    // grab <path.bmp> [late]: late (2026-10-29), the back buffer as presented: after every frame-end listener (the UI
    // mirror, the XR frame end and the menu's desktop copy), not before them
    if (c == "grab" && t.size() >= 2) return frame_grab::grab(t[1], 3000, t.size() >= 3 && t[2] == "late");
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
    if (c == "prepass") {  // prepass [on|off]: [Stereo] PrepassArrays (test only, stageVI-prepass.md milestone 2)
        if (t.size() >= 2 && (t[1] == "on" || t[1] == "off")) prepass::set_on(t[1] == "on");
        if (t.size() >= 2 && t[1] == "trace") prepass::trace_start(t.size() >= 3 ? std::atoi(t[2].c_str()) : 2);
        if (t.size() >= 4 && t[1] == "views" && t[2] == "size") prepass::views_size(t[3] == "on");
        if (t.size() >= 2 && t[1] == "views") return prepass::views_status();
        if (t.size() >= 3 && t[1] == "inst") prepass::set_instanced(t[2] == "on");  // [Stereo] PrepassInstanced (test only)
        if (t.size() >= 3 && t[1] == "cut" && t[2] == "holdpending") {  // [Stereo] PrepassCutHoldPending (test only): on|off
            if (t.size() != 4 || (t[3] != "on" && t[3] != "off")) return "ERROR usage: prepass cut holdpending on|off";
            if (!prepass::set_cut_hold_pending(t[3] == "on")) return "ERROR prepass cut holdpending: not armed (start with [Stereo] PrepassCutHoldPending=1)";
            return prepass::status();
        }
        if (t.size() >= 3 && t[1] == "cut") prepass::set_walk_cut(t[2]);  // [Stereo] PrepassWalkCut: on|off|force (test only)
        if (t.size() >= 2 && t[1] == "cuthold") {  // [Stereo] PrepassCutHoldStarted (test only): started|all
            if (t.size() != 3 || (t[2] != "started" && t[2] != "all")) return "ERROR usage: prepass cuthold started|all";
            prepass::set_cut_hold_started(t[2] == "started");
        }
        if (t.size() >= 2 && t[1] == "cull") {  // prepass cull [on|off]: [Stereo] PrepassUnionCull (2026-10-14, test only)
            if (t.size() >= 3) prepass::set_union_cull(t[2] == "on");
            return prepass::cull_status();
        }
        // prepass slicebarriers|skipvel [on|off] (run 21 item 4, test only): [Stereo] PrepassSliceBarriers / PrepassSkipVelocityCopy
        if (t.size() >= 2 && (t[1] == "slicebarriers" || t[1] == "skipvel")) {
            if (t.size() == 2) return prepass::reductions_status();
            if (t.size() != 3 || (t[2] != "on" && t[2] != "off")) return "ERROR usage: prepass " + t[1] + " [on|off]";
            return t[1] == "slicebarriers" ? prepass::set_slice_barriers(t[2] == "on") : prepass::set_skip_velocity(t[2] == "on");
        }
        if (t.size() == 2 && t[1] == "singles") return prepass::singles_status();  // (run 18, read only)
        if (t.size() == 3 && t[1] == "singles" && t[2] == "reset") {  // (a test: the counts from now)
            prepass::singles_reset();
            return prepass::singles_status();
        }
        // prepass reemit [on|off|skip on|skip off|reset]: [Stereo] RecorderReemit / PrepassRecordSkip (run 18 P2, test only);
        // bare: the re-emits, the refusals by reason, the bytes written, the drift census and the skips by op
        if (t.size() >= 2 && t[1] == "reemit") return rec_state::command(t);
        // prepass cutvel [on|off|d1|d2|d3|reset]: [Stereo] PrepassCutVelocity (run 19, test only); bare: D1's samples by class
        if (t.size() >= 2 && t[1] == "cutvel") return cut_velocity::command(t);
        // prepass instprev [on|off|observe|compare [on|off]|notaa on|off|reset]: [Stereo] PrepassInstPrevMirror (run 19, test only)
        if (t.size() >= 2 && t[1] == "instprev") return inst_prev::command(t);
        // prepass prevworld [on|off|observe|compare [on|off]|reset|kv]: [Stereo] PrepassPrevWorldMirror (run 19 item 6, test only)
        if (t.size() >= 2 && t[1] == "prevworld") return prevworld::command(t);
        // prepass gpu [on|off|reset]: [Stereo] PrepassGpuStages (run 19 item 3, test only): the stages' GPU means since the reset
        if (t.size() >= 2 && t[1] == "gpu") return gpu_stages::command(t);
        // prepass e2sc [on|off|compare N|reset|kv]: [Stereo] PrepassEye2Collector (run 20 item 5, test only): eye 2's collector term in pass 1
        if (t.size() >= 2 && t[1] == "e2sc") return eye2_collector::command(t);
        // prepass e2lb [on|off|compare N|reset|kv]: [Stereo] PrepassEye2Light (run 21 item 5, test only): eye 2's Light Buffer in pass 1
        if (t.size() >= 2 && t[1] == "e2lb") return eye2_light::command(t);
        // prepass e2img [on|off|reset|kv|table]: [Stereo] PrepassEye2Images (run 23, test only): eye 2's images' bindless indices in global_textures; table: the runtime global variable table (read only, also when not installed)
        if (t.size() >= 2 && t[1] == "e2img") return eye2_images::command(t);
        // prepass mi [on|off|compare N|reset|kv|control off|nudge|eye1]: [Stereo] MainInstanced (run 25 item 3, test only): the billboard compare
        if (t.size() >= 2 && t[1] == "mi") return main_inst::command(t);
        // prepass diag depthlog [N]: (r117 DG4, test only) the next N presents' list calls on the game's depth logged ([depthlog] lines); no N: the state
        if (t.size() == 4 && t[1] == "diag" && t[2] == "layerlog" && t[3] == "reset") {  // (r117 DG4, test only: the layer messages' log caps from 0)
            log::reset_limited("d3d12.msg.");
            return "layerlog: the [d3d12] layer messages' line counts reset";
        }
        // prepass diag pixcap <file.wpix> [frames=N]: ([Debug] PixCapturer, test only) PIX's PIXGpuCaptureNextFrames for the next N frames (1)
        if (t.size() >= 4 && t.size() <= 5 && t[1] == "diag" && t[2] == "pixcap") {
            unsigned frames = 1;
            if (t.size() == 5) {
                const size_t at = t[4].rfind("frames=", 0) == 0 ? 7 : 0;  // (frames=N or a bare N)
                if (at >= t[4].size() || t[4].find_first_not_of("0123456789", at) != std::string::npos)
                    return "ERROR prepass diag pixcap <file.wpix> [N|frames=N]";
                frames = static_cast<unsigned>(std::strtoul(t[4].c_str() + at, nullptr, 10));
            }
            return d3d::pixcap(t[3], frames);
        }
        if (t.size() >= 3 && t[1] == "diag" && t[2] == "depthlog") {
            if (t.size() >= 4) {
                // ("... depthlog N draws A B": also the draws whose per-list ordinal is in [A, B]; without: no draws)
                d3d::depthlog_draws(t.size() >= 7 && t[4] == "draws" ? std::atoi(t[5].c_str()) : -1, t.size() >= 7 && t[4] == "draws" ? std::atoi(t[6].c_str()) : -1);
                d3d::depthlog_arm(std::atoi(t[3].c_str()));
            }
            return d3d::depthlog_status();
        }
        // prepass e2vel [on|off|compare N|control off|nudge|eye1|reset|kv]: [Stereo] PrepassEye2Velocity (run 25 item 4, test only): eye 2's velocity pass in pass 1
        if (t.size() >= 2 && t[1] == "e2vel") return eye2_velocity::command(t);
        // prepass p2skip [on|off|kv|reset|control none|nocopy lb|sc|d|all|stale]: [Stereo] Pass2FrontSkip (run 25 item 7's S2, test only): pass 2's front half record-skipped, eye 2's images copied in (P2IN)
        if (t.size() >= 2 && t[1] == "p2skip") return p2skip::command(t);
        // prepass fcensus [on|off|reset|dump|kv]: [Stereo] RouteFCensus (run 25 item 8h' step 1, test only, read only): route F's state census, pass 1's CPU caches against pass 2's in DVL call 1's e1
        if (t.size() >= 2 && t[1] == "fcensus") return fcensus::command(t);
        // prepass mr [off|observe|reset|kv|dump|control none|nopw]: [Stereo] MainRegion (run 25 item 8d, test only, observe only): pass 1's e1 draws by 8f's would-be verdict, the misses census
        if (t.size() >= 2 && t[1] == "mr") return main_region::command(t);
        // prepass pprefresh [on|off|reset|kv]: [Stereo] PrepassPrevProjRefresh (run 25 item 4, test only): eye 2's rage_prev_proj rows 4-7 at pass 1's walk start
        if (t.size() >= 2 && t[1] == "pprefresh") return prepass::pp_refresh_command(t.size() >= 3 ? t[2] : std::string());
        if (t.size() >= 2 && t[1] == "wvp") {  // prepass wvp [on|off]: [Stereo] PrepassWvpMirror (run 17, test only); bare: the compare
            if (t.size() >= 3 && (t[2] == "on" || t[2] == "off")) prepass::set_wvp(t[2] == "on");
            return prepass::wvp_status();
        }
        return prepass::status();
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
    if (c == "menuctl" && t.size() >= 2) {  // menuctl on|off: [Controls] MenuControls for the session (test only)
        controls::set_menu_controls(t[1] == "on", false);
        return std::string("menu controls ") + (controls::menu_controls() ? "on" : "off");
    }
    if (c == "peekx" && t.size() >= 3) {
        // each read also SEH-guarded: a heap object in the chain may be freed between the check and the read
        struct G {
            static bool read(uintptr_t a, void* out, size_t n) {
                __try {
                    std::memcpy(out, reinterpret_cast<const void*>(a), n);
                    return true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    return false;
                }
            }
        };
        // peekx <rva> <offsets> [n]: n (<= 256) bytes in hex at a pointer chain from RDR.exe+rva. <offsets> is "-" (the
        // global itself) or a comma list: each hop reads a pointer at the address so far, then adds the offset (so
        // "0" is the global's pointee, "0,38" a field of it followed). Every read is checked committed and readable.
        auto readable = [](uintptr_t a, size_t n) {
            MEMORY_BASIC_INFORMATION mi{};
            return a && VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
                   (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                   !(mi.Protect & PAGE_GUARD) && a + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
        };
        uintptr_t a = anchors::base() + static_cast<uintptr_t>(std::strtoull(t[1].c_str(), nullptr, 16));
        if (t[2] != "-") {
            size_t pos = 0;
            while (pos <= t[2].size()) {
                const size_t comma = t[2].find(',', pos);
                const std::string tok = t[2].substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                uintptr_t next = 0;
                if (!readable(a, 8) || !G::read(a, &next, 8)) return "ERROR pointer not readable at hop " + tok;
                a = next + static_cast<uintptr_t>(std::strtoull(tok.c_str(), nullptr, 16));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        int n = t.size() >= 4 ? std::atoi(t[3].c_str()) : 64;
        if (n < 1 || n > 256) n = 64;
        unsigned char bytes[256];
        if (!readable(a, static_cast<size_t>(n)) || !G::read(a, bytes, static_cast<size_t>(n))) return "ERROR target not readable";
        std::string out;
        char b[8];
        for (int i = 0; i < n; ++i) {
            std::snprintf(b, sizeof(b), "%s%02x", i ? " " : "", bytes[i]);
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
    if (c == "gpcam") {  // (2026-11-06, GOAL-remaining 6c, read only) the gameplay camera object, [CamManager +0x28], and the one on channel 0
        auto readable = [](uintptr_t a, size_t n) {
            MEMORY_BASIC_INFORMATION mi{};
            return a && VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
                   (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                   !(mi.Protect & PAGE_GUARD) && a + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
        };
        const uintptr_t ga = anchors::addr(anchors::Id::CamManager);
        if (!ga || !readable(ga, 8)) return "ERROR no camera manager";
        const uintptr_t cm = *reinterpret_cast<const uintptr_t*>(ga);
        if (!cm || !readable(cm, 0x30)) return "ERROR camera manager not readable";
        const uintptr_t gp = *reinterpret_cast<const uintptr_t*>(cm + 0x28);
        // gpcam turn <deg> (r140: +0x7e4 holds the follow camera's heading in radians): the heading moved by <deg>, only when
        // +0x7e4 reads the game camera's heading now (within 3 deg), so a moved field is never written
        // (rdrvr_CameraFovUpdate 0x140378500: +0x7e4 = **(L +0x68) + **(L +0x18) + **(L +0x48), L = [[[[cam +0x620] +0x10]
        // +0x10] +8], rebuilt each frame) gpcam parts: the three; gpcam turnpart <0|1|2> <deg>: one of them moved by <deg>
        float* part[3] = {nullptr, nullptr, nullptr};
        {
            uintptr_t q = gp;
            const uint32_t offs[4] = {0x620, 0x10, 0x10, 8};
            for (uint32_t o2 : offs) q = q && readable(q + o2, 8) ? *reinterpret_cast<const uintptr_t*>(q + o2) : 0;
            const uint32_t po[3] = {0x68, 0x18, 0x48};
            for (int i = 0; i < 3 && q; ++i) {
                const uintptr_t pp = readable(q + po[i], 8) ? *reinterpret_cast<const uintptr_t*>(q + po[i]) : 0;
                part[i] = pp && readable(pp, 4) ? reinterpret_cast<float*>(pp) : nullptr;
            }
        }
        if (t.size() >= 2 && t[1] == "parts") {
            char e[200];
            std::snprintf(e, sizeof(e), "heading parts %.4f %.4f %.4f (at %llx %llx %llx), +0x7e4 %.4f", part[0] ? *part[0] : NAN, part[1] ? *part[1] : NAN,
                          part[2] ? *part[2] : NAN, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(part[0])),
                          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(part[1])), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(part[2])),
                          gp && readable(gp + 0x7e4, 4) ? *reinterpret_cast<const float*>(gp + 0x7e4) : NAN);
            return e;
        }
        // gpcam addabs <deg> <hex address>...: each float there (within 0x1000 of part 0, read as an angle within 2 pi) moved by
        // <deg> together (r143: the part's neighbours that follow the heading)
        if (t.size() >= 4 && t[1] == "addabs") {
            if (!part[0]) return "ERROR no part 0";
            const uintptr_t p0 = reinterpret_cast<uintptr_t>(part[0]);
            const float step = std::strtof(t[2].c_str(), nullptr) * 0.0174532925f;
            float* at[32];
            int n = 0;
            for (size_t i = 3; i < t.size() && n < 32; ++i) {
                const uintptr_t a = static_cast<uintptr_t>(std::strtoull(t[i].c_str(), nullptr, 16));
                if (a % 4 || (a > p0 ? a - p0 : p0 - a) > 0x1000 || !readable(a, 4)) return "ERROR " + t[i] + " not near part 0";
                float* f = reinterpret_cast<float*>(a);
                if (!std::isfinite(*f) || std::fabs(*f) > 6.4f) return "ERROR " + t[i] + " not an angle";
                at[n++] = f;
            }
            std::string r = "moved";
            for (int i = 0; i < n; ++i) {
                const float v = *at[i];
                *at[i] = std::remainder(v + step, 6.2831853f);
                char e[48];
                std::snprintf(e, sizeof(e), " %.3f->%.3f", v, *at[i]);
                r += e;
            }
            return r;
        }
        if (t.size() >= 4 && t[1] == "turnpart") {
            const int i = std::atoi(t[2].c_str());
            if (i < 0 || i > 2 || !part[i] || !std::isfinite(*part[i])) return "ERROR no such part";
            const float before = *part[i];
            *part[i] = std::remainder(before + std::strtof(t[3].c_str(), nullptr) * 0.0174532925f, 6.2831853f);
            char e[96];
            std::snprintf(e, sizeof(e), "heading part %d %.4f -> %.4f", i, before, *part[i]);
            return e;
        }
        if (t.size() >= 3 && t[1] == "turn") {
            if (!gp || !readable(gp + 0x7e4, 4)) return "ERROR no gameplay camera";
            float bx = 0.0f, bz = 0.0f;
            if (!camera_lever::game_heading(&bx, &bz)) return "ERROR no game camera heading";
            float* hp = reinterpret_cast<float*>(gp + 0x7e4);
            const float now = *hp, cam = std::atan2(bx, bz);
            float diff = std::remainder(now - cam, 6.2831853f);
            if (!std::isfinite(now) || std::fabs(diff) > 0.0524f) {
                char e[120];
                std::snprintf(e, sizeof(e), "ERROR +0x7e4 reads %.4f, the camera's heading %.4f: not written", now, cam);
                return e;
            }
            const float want = std::remainder(now + std::strtof(t[2].c_str(), nullptr) * 0.0174532925f, 6.2831853f);
            *hp = want;
            char e[120];
            std::snprintf(e, sizeof(e), "gameplay camera heading %.4f -> %.4f rad", now, want);
            return e;
        }
        char b[96];
        std::snprintf(b, sizeof(b), "gameplay camera %llx (manager %llx)", static_cast<unsigned long long>(gp), static_cast<unsigned long long>(cm));
        return b;
    }
    if (c == "peekabs" && t.size() >= 2) {
        // peekabs <hex address> [n] [q]: n (<= 128) floats (q: 64-bit words in hex) at an absolute address, read only after
        // VirtualQuery shows them committed and readable (run 11 item 1: a census prop's drawable, for its bounds)
        uintptr_t a = static_cast<uintptr_t>(std::strtoull(t[1].c_str(), nullptr, 16));
        int n = t.size() >= 3 ? std::atoi(t[2].c_str()) : 16;
        if (n < 1 || n > 128) n = 16;
        const bool q = t.size() >= 4 && t[3] == "q";
        const size_t bytes = static_cast<size_t>(n) * (q ? 8 : 4);
        MEMORY_BASIC_INFORMATION mi{};
        if (!a || !VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
            !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) || (mi.Protect & PAGE_GUARD) ||
            a + bytes > reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize)
            return "ERROR not readable";
        std::string out;
        char b[32];
        for (int i = 0; i < n; ++i) {
            if (q) std::snprintf(b, sizeof(b), "%s%llx", i ? " " : "", static_cast<unsigned long long>(reinterpret_cast<const uint64_t*>(a)[i]));
            else std::snprintf(b, sizeof(b), "%s%.4g", i ? " " : "", reinterpret_cast<const float*>(a)[i]);
            out += b;
        }
        return out;
    }
    if (c == "scope") return scope::command(t);  // (r55 item 11, [Scope]; "scope game": the run 29 spike's reads)
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
    if (c == "padover" && t.size() >= 5) {  // padover <buttons> <rx> <ry> <ms>: the override (run 8 item 5's probe), the right stick reaching the game
        xinput::set_override(static_cast<uint16_t>(parse_arg(t[1])), true, std::strtof(t[2].c_str(), nullptr), std::strtof(t[3].c_str(), nullptr),
                             static_cast<uint32_t>(parse_arg(t[4])));
        return "overriding";
    }
    if (c == "pad" && t.size() >= 3) {
        xinput::press(static_cast<uint16_t>(parse_arg(t[1])), static_cast<uint32_t>(parse_arg(t[2])));
        return "pressed";
    }
    if (c == "forceremove") return diag::force_device_removal() ? "removing a WARP test device" : "ERROR could not start the removal test";
    if (c == "playbatch" && t.size() >= 2)  // playbatch <K>: [Render] PlaybackBatch at run time (0 = one packet a claim)
        return playback::set_batch(std::atoi(t[1].c_str())) ? "playback batch " + t[1] : "ERROR not installed ([Render] PlaybackBatch=-1)";
    if (c == "playback") {
        char st[256];
        playback::status(st, sizeof(st));
        return st;
    }
    if (c == "affinity" && t.size() >= 2) return diag::set_affinity(t[1].c_str());  // same|split|off (test only)
    if (c == "stacks") {  // stacks <render|playback|main> [n]: a thread's stack by function and scene phase ("[stack]" lines)
        std::string split;  // stacks render n split <walk> <sites,> <entity> <draws,>
        if (t.size() >= 8 && t[3] == "split") split = t[4] + " " + t[5] + " " + t[6] + " " + t[7];
        return diag::sample_stacks(t.size() >= 2 ? t[1].c_str() : "render", t.size() >= 3 ? std::atoi(t[2].c_str()) : 2000,
                                   split.empty() ? nullptr : split.c_str());
    }
    if (c == "sample") {  // where the busy threads are: "[sample]" lines (RIP histograms; no unwind)
        return diag::sample_threads(t.size() >= 2 ? std::atoi(t[1].c_str()) : 20);
    }
    if (c == "vq") {  // (2026-10-29, read only) RDR.exe's .data walked, VirtualQuery timed at the anchors' globals, the process's regions (image_mem.h)
        return image_mem::report();
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
    // (run 25) res was 2048, which cut long replies such as "prepass mi kv" at 2046 chars
    char res[12288], fg[256], census[512], hooks_txt[2048], hooks_esc[2048], rt[160], ring_txt[320], ring[400], cam[400], rset[256],
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
    char buf[32768];  // every field at its buffer's length fits (the XR status is up to 1024; res up to 12288)
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
