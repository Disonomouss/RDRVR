#include "core/horseshoe.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include "common/rdrvr_api.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/config.h"
#include "core/gestures.h"
#include "core/hands.h"
#include "core/holster.h"
#include "core/hooks.h"
#include "core/log.h"

// [Gestures] HorseshoeThrow (round 133; off by default). The game's horseshoes (content/Mini_Games/HorseShoes/HorseShoes,
// research/after-08132/minigames_research.md 2) is a meter: two held actions push a spring model of John's arm back and
// forward (the phase of his horseshoe/Sweep clip), a third lets go, and the shoe's launch is made from the model alone
// (SET_PROP_VELOCITY: the heading to the stake, the elevation from the phase, the speed from the model's). Seen from its own
// third-person cameras; from John's head, with the arms on the controllers, nothing of it shows.
// With the option on, while horseshoes' script runs (holster::minigame bit 8):
//   - John's arms stay on the controllers ([Body] TableGameArms does not apply), and the shoe the script hangs on his right
//     wrist ("wrist_r_attachment": ATTACH_OBJECTS_USING_LOCATOR's handler is hooked to know it) is his right hand's at once
//     (body.cpp's hand-held props);
//   - holding the right grip holds it; letting go with the hand moving (kMinSpeed) throws it: the script's own release is
//     asked for (its three actions are read by IS_DIGITAL_ACTION_DOWN / _PRESSED with their names: both handlers are hooked
//     and answer for "@HORSESHOES.SWING_FORWARD", "SWING_BACKWARD", "RELEASE", "AIM" and "ZOOM_AT_STAKE" while a throw
//     waits), so the game's turn, sound, score and cameras go on as its own throw's;
//   - the launch's velocity is the hand's: the script's SET_PROP_VELOCITY lands in FUN_1406adad0(physinst, vector), hooked:
//     its vector is replaced by the hand's velocity at the let-go ([Gestures] ThrowGain times HorseshoeThrowGain) as it is
//     after the time since (gravity), and on the next script tick the shoe is put on the throw's own path from the hand
//     (SET_OBJECT_POSITION, SET_PROP_VELOCITY), since the script lets it go from John's animated wrist, some frames late.
// The script takes a release only while its arm model moves forward: the swing actions are answered from the hand's own
// move along John's facing while the grip holds the shoe, and held forward from the let-go (the other way after 0.3 s
// should the model's sign be the other: said in the log).
// Every read here goes through a pointer the game just used (a native's context, its arguments), byte by byte for a name:
// none can fault (the game's crash reporter sees a first-chance fault: rdrvr-rdrmessage-crash-box)
namespace rdrvr::horseshoe {
namespace {

constexpr uint32_t kGetPhysinstFromObject = 0xDB70DF0Cu, kSetObjectPosition = 0xC5D796F8u, kSetPropVelocity = 0x28425D8Cu;
constexpr float kMinSpeed = 1.0f;     // the hand's speed (m/s, before the gains) a let-go throws at
constexpr float kGravity = 9.81f;
constexpr double kGiveUpMs = 2500.0;  // a release the script never took

using NativeHandler_t = void (*)(uintptr_t ctx);
using PropSetVelocity_t = bool (*)(uintptr_t inst, float* v);
using RemoveAttachments_t = uint64_t (*)(uintptr_t mgr, uint32_t object, uintptr_t p3);
NativeHandler_t o_action_down = nullptr, o_action_pressed = nullptr, o_attach = nullptr;
PropSetVelocity_t o_prop_velocity = nullptr;
RemoveAttachments_t o_remove_attachments = nullptr;

std::atomic<bool> g_on{false}, g_hooked{false};
std::atomic<bool> g_active{false};   // the option on and the script running (frame(): the hooks read this alone)
std::atomic<bool> g_game{false};     // horseshoes' script running (the attach hook's)
std::atomic<float> g_gain{1.0f};
std::atomic<int> g_swing{0};         // the script's swing answered: +1 forward, -1 backward, 0 the pad's own
std::atomic<bool> g_press{false};    // its release answered pressed
std::atomic<int32_t> g_shoe{0};      // the shoe on John's right wrist: its object's handle, 0 none
std::atomic<int32_t> g_inst{0};      // its physinst's handle (asked once a shoe)
// (the review) the shoe the script took off John's wrist (REMOVE_ALL_OBJECT_ATTACHMENTS(John's object): its own throw's
// release, a round's end): no longer the hand's, kept only for a waiting throw's put on its path
std::atomic<int32_t> g_thrown{0}, g_thrown_inst{0};
std::atomic<int32_t> g_john_obj{0};  // John's object's handle (frame(): the attach hook compares with it)
std::atomic<double> g_launch_ms{-1.0};  // the script's launch of a waiting throw, seen by the velocity's hook (-1: not yet)
std::atomic<uint64_t> g_throws{0}, g_launched{0}, g_given_up{0}, g_slow{0}, g_attached{0}, g_answers{0};
struct Pending {
    bool on = false;
    double t0 = 0.0;  // the let-go (ms)
    float vel[3] = {}, org[3] = {};
    float speed = 0.0f;
};
std::mutex g_mu;
Pending g_pend;      // g_mu
char g_last[200] = "";  // g_mu

bool rd(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool wr(uintptr_t a, const void* in, size_t n) {
    __try {
        std::memcpy(reinterpret_cast<void*>(a), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// a script's string, a byte at a time up to its end (never past it)
bool cstr(uintptr_t p, char* out, size_t cap) {
    __try {
        size_t i = 0;
        for (; i + 1 < cap; ++i) {
            const char c = *reinterpret_cast<const char*>(p + i);
            out[i] = c;
            if (!c) return true;
        }
        out[i] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

// the answer of a native read through its context (the return slot's pointer at +0)
void set_answer(uintptr_t ctx, int32_t v) {
    uintptr_t ret = 0;
    if (rd(ctx, &ret, sizeof(ret)) && ret && wr(ret, &v, sizeof(v))) g_answers.fetch_add(1, std::memory_order_relaxed);
}
// a horseshoes action's name out of a native's first argument (after "@HORSESHOES."), false for any other
bool action_name(uintptr_t ctx, char* tail, size_t cap) {
    uintptr_t args = 0, name = 0;
    char s[40];
    if (!rd(ctx + 0x10, &args, sizeof(args)) || !args || !rd(args, &name, sizeof(name)) || !name || !cstr(name, s, sizeof(s))) return false;
    if (std::strncmp(s, "@HORSESHOES.", 12) != 0) return false;
    std::snprintf(tail, cap, "%s", s + 12);
    return true;
}
// (cycle HH1: the name read after the game's handler faulted 24 times: the handler's answer is written over its first
// argument (the context's return slot is its arguments' own), leaving half a pointer) the name is read BEFORE the game's
// handler runs, the answer set after it
void hk_action_down(uintptr_t ctx) {
    const int sw = g_swing.load(std::memory_order_relaxed);
    const bool waiting = g_press.load(std::memory_order_relaxed);
    char t[28];
    const bool ours = g_active.load(std::memory_order_relaxed) && ctx && (sw || waiting) && action_name(ctx, t, sizeof(t));
    o_action_down(ctx);
    if (!ours) return;
    if (!std::strcmp(t, "SWING_FORWARD")) {
        if (sw) set_answer(ctx, sw > 0 ? 1 : 0);
    } else if (!std::strcmp(t, "SWING_BACKWARD")) {
        if (sw) set_answer(ctx, sw < 0 ? 1 : 0);
    } else if (waiting && (!std::strcmp(t, "AIM") || !std::strcmp(t, "ZOOM_AT_STAKE"))) {
        set_answer(ctx, 0);  // (either held, the script takes no release)
    }
}
void hk_action_pressed(uintptr_t ctx) {
    char t[28];
    const bool ours = g_active.load(std::memory_order_relaxed) && ctx && g_press.load(std::memory_order_relaxed) && action_name(ctx, t, sizeof(t));
    o_action_pressed(ctx);
    if (!ours) return;
    if (!std::strcmp(t, "RELEASE")) set_answer(ctx, 1);
    else if (!std::strcmp(t, "AIM")) set_answer(ctx, 0);
}
// ATTACH_OBJECTS_USING_LOCATOR(object, parent object, parent's locator, object's locator, int): the shoe hung on John
void hk_attach(uintptr_t ctx) {
    uintptr_t args = 0, loc = 0;
    int32_t obj = 0, parent = 0;
    char s[28] = "";
    // (the arguments read before the game's handler: its answer goes over the first)
    const bool read = g_game.load(std::memory_order_relaxed) && ctx && rd(ctx + 0x10, &args, sizeof(args)) && args && rd(args, &obj, sizeof(obj)) &&
                      rd(args + 8, &parent, sizeof(parent)) && rd(args + 0x10, &loc, sizeof(loc)) && loc && cstr(loc, s, sizeof(s));
    o_attach(ctx);
    if (!read) return;
    if (!obj || parent != g_john_obj.load(std::memory_order_relaxed) || _stricmp(s, "wrist_r_attachment") != 0) return;
    g_inst.store(0, std::memory_order_relaxed);
    g_shoe.store(obj, std::memory_order_relaxed);
    g_attached.fetch_add(1, std::memory_order_relaxed);
}
// REMOVE_ALL_OBJECT_ATTACHMENTS's work (FUN_1402db610(the attachments' keeper, the object's handle, ...)): John's object's
// attachments gone: the shoe is off his wrist
uint64_t hk_remove_attachments(uintptr_t mgr, uint32_t object, uintptr_t p3) {
    if (g_game.load(std::memory_order_relaxed) && object && static_cast<int32_t>(object) == g_john_obj.load(std::memory_order_relaxed)) {
        g_thrown_inst.store(g_inst.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        g_thrown.store(g_shoe.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return o_remove_attachments(mgr, object, p3);
}
// SET_PROP_VELOCITY's work (the physinst, the vector): a waiting throw's launch takes the hand's velocity
bool hk_prop_velocity(uintptr_t inst, float* v) {
    if (v && g_active.load(std::memory_order_relaxed) && g_launch_ms.load(std::memory_order_relaxed) < 0.0) {
        Pending p;
        {
            std::lock_guard lock(g_mu);
            p = g_pend;
        }
        if (p.on) {
            const double now = log::now_ms();
            const float t = static_cast<float>((now - p.t0) * 0.001);
            const float nv[3] = {p.vel[0], p.vel[1] - kGravity * t, p.vel[2]};
            if (std::isfinite(nv[0] + nv[1] + nv[2]) && wr(reinterpret_cast<uintptr_t>(v), nv, sizeof(nv))) {
                g_launch_ms.store(now, std::memory_order_relaxed);
                g_launched.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return o_prop_velocity(inst, v);
}

void clear_throw() {
    {
        std::lock_guard lock(g_mu);
        g_pend.on = false;
    }
    g_press.store(false, std::memory_order_relaxed);
    g_swing.store(0, std::memory_order_relaxed);
    g_launch_ms.store(-1.0, std::memory_order_relaxed);
}

}  // namespace

bool enabled() { return g_on.load(std::memory_order_relaxed); }
void set_enabled(bool on, bool save) {
    if (g_on.exchange(on) != on) log::info("[horseshoe] the shoe thrown by your own arm: %s%s", on ? "on" : "off", g_hooked.load() ? "" : " (not on this build of the game)");
    if (save) config::set("Gestures", "HorseshoeThrow", on ? "1" : "0");
}
bool active() { return g_active.load(std::memory_order_relaxed); }
bool holds() { return g_active.load(std::memory_order_relaxed) && g_shoe.load(std::memory_order_relaxed) != 0; }

void init() {
    g_on = config::get_bool("Gestures", "HorseshoeThrow", false);
    g_gain = config::get_float("Gestures", "HorseshoeThrowGain", 1.0f);
}
// (cycle HS3: the hooks made in init(), before the anchors were checked, patched four of them: every anchor's bytes are read
// after the modules' init, so the whole mod stood down) the hooks with the other game hooks, after that check
void install() {
    using anchors::Id;
    if (anchors::present(Id::NativeActionDown) && anchors::present(Id::NativeActionPressed) && anchors::present(Id::NativeAttachLocator) &&
        anchors::present(Id::PropSetVelocity) && anchors::present(Id::ObjectRemoveAttachments)) {
        const bool a = hooks::install("RDR IS_DIGITAL_ACTION_DOWN (horseshoes' swing by the hand)", reinterpret_cast<void*>(anchors::addr(Id::NativeActionDown)),
                                      hk_action_down, &o_action_down);
        const bool b = a && hooks::install("RDR IS_DIGITAL_ACTION_PRESSED (horseshoes' release by the hand)",
                                           reinterpret_cast<void*>(anchors::addr(Id::NativeActionPressed)), hk_action_pressed, &o_action_pressed);
        const bool c = b && hooks::install("RDR ATTACH_OBJECTS_USING_LOCATOR (the horseshoe in John's hand)",
                                           reinterpret_cast<void*>(anchors::addr(Id::NativeAttachLocator)), hk_attach, &o_attach);
        const bool d = c && hooks::install("RDR prop velocity (the horseshoe's launch by the hand)", reinterpret_cast<void*>(anchors::addr(Id::PropSetVelocity)),
                                           hk_prop_velocity, &o_prop_velocity);
        const bool e = d && hooks::install("RDR object attachments removed (the horseshoe off John's wrist)",
                                           reinterpret_cast<void*>(anchors::addr(Id::ObjectRemoveAttachments)), hk_remove_attachments, &o_remove_attachments);
        g_hooked = e;
    }
    log::info("[horseshoe] the shoe thrown by your own arm %d (hooks %d, gain %.2f)", g_on.load() ? 1 : 0, g_hooked.load() ? 1 : 0, g_gain.load());
}

void frame() {
    const double now = log::now_ms();
    static bool was = false, grip_was = false;
    static uint64_t inst_req = 0;
    static int32_t inst_for = 0;
    static double inst_ms = 0.0;
    const bool game = (holster::minigame() & 8) != 0 && holster::minigame_fresh();
    g_game.store(game, std::memory_order_relaxed);
    RdrvrActorState st{};
    const bool have_st = api::actor_state(&st) && st.valid;
    g_john_obj.store(have_st ? st.object : 0, std::memory_order_relaxed);
    const bool act = game && g_hooked.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed);
    g_active.store(act, std::memory_order_relaxed);
    if (holster::minigame_fresh() && !(holster::minigame() & 8)) {  // (the review: not while the scripts' list is only stale: the pause menu)
        g_shoe.store(0, std::memory_order_relaxed);
        g_inst.store(0, std::memory_order_relaxed);
        g_thrown.store(0, std::memory_order_relaxed);
        g_thrown_inst.store(0, std::memory_order_relaxed);
        inst_for = 0;
    }
    if (!act) {
        if (was) {
            clear_throw();
            log::info("[horseshoe] the game of horseshoes over: %llu throws by the hand (launched %llu, given up %llu, let go slowly %llu)",
                      static_cast<unsigned long long>(g_throws.load()), static_cast<unsigned long long>(g_launched.load()),
                      static_cast<unsigned long long>(g_given_up.load()), static_cast<unsigned long long>(g_slow.load()));
        }
        was = grip_was = false;
        return;
    }
    if (!was) log::info("[horseshoe] the game of horseshoes: the shoe is your right hand's (hold the grip, swing, let go to throw)");
    was = true;
    const int32_t shoe = g_shoe.load(std::memory_order_relaxed);
    // the shoe's physinst, asked once a shoe (its handle: the path's velocity after the launch)
    if (inst_req) {
        RdrvrNativeResult r{};
        if (api::wait_native(inst_req, &r, 0)) {
            inst_req = 0;
            if (inst_for == shoe) g_inst.store(static_cast<int32_t>(r.value & 0xffffffffull), std::memory_order_relaxed);
        } else if (now - inst_ms > 1000.0) {
            api::cancel_op(inst_req);
            inst_req = 0;
            inst_for = 0;
        }
    }
    static int32_t said_shoe = 0;
    if (shoe && inst_for != shoe && !inst_req) {
        const uint64_t a[1] = {static_cast<uint64_t>(static_cast<uint32_t>(shoe))};
        inst_req = api::queue_native(kGetPhysinstFromObject, a, 1);
        inst_for = shoe;
        inst_ms = now;
        if (said_shoe != shoe) log::info("[horseshoe] a shoe on John's right wrist (object %d): held while the right grip is", shoe);  // (the review: once a shoe)
        said_shoe = shoe;
    }
    const hands::Hand h = hands::get(1);
    const bool grip = h.connected && h.grip > (grip_was ? 0.3f : 0.6f);
    Pending p;
    {
        std::lock_guard lock(g_mu);
        p = g_pend;
    }
    if (!p.on) {
        int sw = 0;
        float v[3];
        if (shoe && grip && have_st && gestures::hand_velocity(1, 80.0, v)) {  // the hand's move along John's facing
            const float hr = st.heading_deg * 0.0174532925f, fx = -std::sin(hr), fz = -std::cos(hr);
            const float s = v[0] * fx + v[2] * fz;
            sw = s > 0.6f ? 1 : s < -0.4f ? -1 : 0;
        }
        g_swing.store(sw, std::memory_order_relaxed);
        if (shoe && grip_was && !grip) {  // let go
            float vel[3], org[3], speed = 0.0f;
            const bool got = gestures::hand_throw(1, 300.0, vel, org, &speed);
            if (got && speed >= kMinSpeed && std::isfinite(vel[0] + vel[1] + vel[2] + org[0] + org[1] + org[2])) {
                const float g = g_gain.load(std::memory_order_relaxed);
                Pending n;
                n.on = true;
                n.t0 = now;
                n.speed = speed;
                for (int k = 0; k < 3; ++k) n.vel[k] = vel[k] * g, n.org[k] = org[k];
                {
                    std::lock_guard lock(g_mu);
                    g_pend = n;
                }
                g_launch_ms.store(-1.0, std::memory_order_relaxed);
                g_swing.store(1, std::memory_order_relaxed);
                g_press.store(true, std::memory_order_relaxed);
                g_throws.fetch_add(1, std::memory_order_relaxed);
                log::info("[horseshoe] let go at %.2f m/s: thrown from (%.2f %.2f %.2f) at (%.2f %.2f %.2f) m/s; the script's release asked", speed, org[0],
                          org[1], org[2], n.vel[0], n.vel[1], n.vel[2]);
            } else if (g_slow.fetch_add(1, std::memory_order_relaxed) < 10) {
                log::info("[horseshoe] let go slowly (%.2f m/s, a throw from %.1f): the shoe stays on the wrist (hold the grip again)", speed, kMinSpeed);
            }
        }
    } else {
        const double dt = now - p.t0;
        const double lm = g_launch_ms.load(std::memory_order_relaxed);
        if (lm >= 0.0) {  // the script launched it (its velocity already the hand's): the shoe on the throw's own path
            // (the review) the shoe as the script took it off his wrist for this launch, else the one still on it
            const int32_t off = g_thrown.load(std::memory_order_relaxed);
            const int32_t put = off ? off : shoe;
            const int32_t inst = off ? g_thrown_inst.load(std::memory_order_relaxed) : g_inst.load(std::memory_order_relaxed);
            const float t = static_cast<float>(dt * 0.001) + 0.011f;  // (the puts run on the next script tick)
            const float pos[3] = {p.org[0] + p.vel[0] * t, p.org[1] + p.vel[1] * t - 0.5f * kGravity * t * t, p.org[2] + p.vel[2] * t};
            const float vel[3] = {p.vel[0], p.vel[1] - kGravity * t, p.vel[2]};
            clear_throw();  // (first: the velocity put below is not a launch to answer)
            if (put) {
                // (the review) SET_OBJECT_POSITION takes its vector by value (x and y in one argument, z in the next: held_prop's)
                auto fb = [](float f) {
                    uint32_t u;
                    std::memcpy(&u, &f, sizeof(u));
                    return static_cast<uint64_t>(u);
                };
                const uint64_t a[3] = {static_cast<uint64_t>(static_cast<uint32_t>(put)), fb(pos[0]) | (fb(pos[1]) << 32), fb(pos[2])};
                api::queue_native(kSetObjectPosition, a, 3);
                if (inst) {
                    const uint64_t b[2] = {static_cast<uint64_t>(static_cast<uint32_t>(inst)), 0};
                    api::queue_native(kSetPropVelocity, b, 2, 2, vel);
                }
            }
            std::lock_guard lock(g_mu);
            std::snprintf(g_last, sizeof(g_last), "launched %.0f ms after the let-go (the script's), put at (%.2f %.2f %.2f) with (%.2f %.2f %.2f) m/s%s",
                          lm - p.t0, pos[0], pos[1], pos[2], vel[0], vel[1], vel[2], put && inst ? "" : put ? " (no physinst: the place only)" : " (no shoe known)");
            log::info("[horseshoe] %s", g_last);
            g_thrown.store(0, std::memory_order_relaxed);
            g_thrown_inst.store(0, std::memory_order_relaxed);
        } else if (dt > kGiveUpMs) {
            clear_throw();
            g_given_up.fetch_add(1, std::memory_order_relaxed);
            log::warn("[horseshoe] the script took no release in %.1f s (the swing answered forward, then back, in turn): the throw given up", kGiveUpMs / 1000.0);
        } else {  // forward first; the other way after 0.3 s should the model's forward be the other sign; so on
            const int phase = static_cast<int>(dt / 300.0);
            const int sw = phase % 2 == 0 ? 1 : -1;
            if (g_swing.exchange(sw, std::memory_order_relaxed) != sw && phase == 1)
                log::info("[horseshoe] no launch 0.3 s after the release with the swing forward: the swing answered backward");
            g_press.store(true, std::memory_order_relaxed);
        }
    }
    grip_was = grip;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, a, v;
    in >> c;
    while (in >> a) {
        if (a == "on" || a == "off") set_enabled(a == "on", false);
        else if (a == "gain" && in >> v) g_gain = std::strtof(v.c_str(), nullptr);
        else if (a == "shoe" && in >> v) g_shoe = std::atoi(v.c_str());  // (the tests': a shoe's object named by hand)
    }
    char b[420];
    std::lock_guard lock(g_mu);
    std::snprintf(b, sizeof(b), "horseshoe %d (hooks %d, gain %.2f), the game %d, active %d, the shoe %d (physinst %d), swing %d, release asked %d | attached "
                  "%llu, throws %llu, launched %llu, given up %llu, let go slowly %llu, answers %llu | last: %s",
                  g_on.load() ? 1 : 0, g_hooked.load() ? 1 : 0, g_gain.load(), g_game.load() ? 1 : 0, g_active.load() ? 1 : 0, g_shoe.load(), g_inst.load(),
                  g_swing.load(), g_press.load() ? 1 : 0, static_cast<unsigned long long>(g_attached.load()), static_cast<unsigned long long>(g_throws.load()),
                  static_cast<unsigned long long>(g_launched.load()), static_cast<unsigned long long>(g_given_up.load()),
                  static_cast<unsigned long long>(g_slow.load()), static_cast<unsigned long long>(g_answers.load()), g_last);
    return b;
}

}  // namespace rdrvr::horseshoe
