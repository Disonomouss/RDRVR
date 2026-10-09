#include "core/controls.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "common/rdrvr_api.h"
#include "core/api.h"
#include "core/body.h"
#include "core/actions.h"
#include "core/aim.h"
#include "core/config.h"
#include "core/dual.h"
#include "core/hands.h"
#include "core/holster.h"
#include "core/physics.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/pose.h"
#include "core/reload.h"
#include "core/wheel.h"
#include "core/whistle.h"
#include "core/vr_mode.h"
#include "core/xr.h"

namespace rdrvr::controls {
namespace {

float g_grip_on = 0.6f, g_grip_off = 0.4f;
double g_menu_hold_ms = 600;
bool g_menu_chord_on = true;  // [Controls] MenuChord: both B buttons act as the menu button

// The remap: each VR source drives one pad target.
enum Target : uint8_t {
    kNone, kPadA, kPadB, kPadX, kPadY, kPadLB, kPadRB, kPadLT, kPadRT, kPadL3, kPadR3, kPadBack, kPadStart,
    kPadUp, kPadDown, kPadLeft, kPadRight
};
const char* const kTargetNames[kTargets] = {"none", "A",  "B",    "X",     "Y",      "LB",       "RB",       "LT",       "RT",
                                            "L3",   "R3", "Back", "Start", "DpadUp", "DpadDown", "DpadLeft", "DpadRight"};
const uint16_t kTargetButton[kTargets] = {0,
                                          XINPUT_GAMEPAD_A,
                                          XINPUT_GAMEPAD_B,
                                          XINPUT_GAMEPAD_X,
                                          XINPUT_GAMEPAD_Y,
                                          XINPUT_GAMEPAD_LEFT_SHOULDER,
                                          XINPUT_GAMEPAD_RIGHT_SHOULDER,
                                          0,
                                          0,
                                          XINPUT_GAMEPAD_LEFT_THUMB,
                                          XINPUT_GAMEPAD_RIGHT_THUMB,
                                          XINPUT_GAMEPAD_BACK,
                                          XINPUT_GAMEPAD_START,
                                          XINPUT_GAMEPAD_DPAD_UP,
                                          XINPUT_GAMEPAD_DPAD_DOWN,
                                          XINPUT_GAMEPAD_DPAD_LEFT,
                                          XINPUT_GAMEPAD_DPAD_RIGHT};
const char* const kSourceKeys[kSources] = {"RightA",         "RightB",          "LeftX",    "LeftY",     "LeftStickClick",
                                           "RightStickClick", "LeftGrip",        "RightGrip", "LeftTrigger", "RightTrigger"};
const char* const kSourceLabels[kSources] = {"Right A",          "Right B",           "Left X",    "Left Y",     "Left stick click",
                                             "Right stick click", "Left grip",         "Right grip", "Left trigger", "Right trigger"};
const uint8_t kDefault[kSources] = {kPadA, kPadB, kPadX, kPadY, kPadL3, kPadR3, kPadLB, kPadRB, kPadLT, kPadRT};
std::atomic<uint8_t> g_map[kSources];
std::atomic<bool> g_left_handed{false};
constexpr int kSrcLeftStickClick = 4;  // kSourceKeys[4]
// [Horse] StickClickBrake: on a horse or a wagon the left stick click is the game's brake (@GENERIC.BRAKE, the pad's
// RB, input_horse.xml; nothing for riding is on L3): held, a soft brake; with the stick pulled back, a hard one. Decided
// at the press: 0 its own mapping, 1 the brake, 2 nothing until it is let go (dismounted, or the recentre chord)
std::atomic<bool> g_click_brake{true};
int g_click_route = 0;
bool g_lclk_was = false;
std::atomic<uint64_t> g_brakes{0};
// [Hands] TriggerAims: with a gun in hand in first person, the right trigger alone fires. The game shoots only from its
// aim pose (FUN_140d169a0 drops a fire request otherwise), so while the trigger is used LT is held for it, from the
// pull until TriggerAimTailMs after the last; the pull itself is held back until the aim pose is up (TriggerAimLeadMs
// at most), so the game sees its press then (a quick pull already let go is pressed for 50 ms then)
std::atomic<bool> g_trigger_aims{true}, g_aims_riding{true};
// [Reload] TwoHandedAims: while a long gun is held with both hands, LT is held for it the whole time (the game's aim
// stance: its support-hand grip, which the front hand snaps onto, and shots at once), whatever TriggerAims is; when the
// hold ends, TriggerAimTailMs follows (and in Dead Eye LT stays held until it ends)
std::atomic<bool> g_two_aims{true};
// [Hands] AimWhenRaised: LT held for you while the gun hand is raised (holster::gun_raised), so the gun stays up in the
// aim stance and every pull fires at once (round 8: "holding LT and shooting works much better"); the tail after
// lowering it as after a pull. [Hands] TriggerAimReady: a held-back pull goes through when the gun is ready to fire
// (holster::fire_ready), not merely when its aim pose starts (a press before the raise reaches 0.8 is dropped).
std::atomic<bool> g_aim_raised{true}, g_ready_gate{true};
std::atomic<bool> g_sprint_drops{true};  // [Hands] SprintDropsAim: on foot, the game's sprint button lets the raised gun's LT go
float g_sprint_hold_ms = 700.0f;         // [Hands] SprintHoldMs: how long after the last sprint press it stays let go
double g_sprint_until = 0;
std::atomic<bool> g_jump_drops{false};  // [Hands] JumpDropsAim (run 7 item 1f, off until the headset round)
float g_jump_hold_ms = 1500.0f;         // [Hands] JumpHoldMs: LT let go that long from the jump's press (a vault's length)
double g_jump_until = 0, g_jump_since = 0, g_jump_pulse_until = 0;
bool g_jump_prev = false, g_jump_pending = false;
std::atomic<uint64_t> g_jump_presses{0}, g_jump_waits{0};
std::atomic<float> g_jump_wait_ms{0.0f};
// [Gestures] Whistle (run 9 item 1, the user: "Add ability to whistle with off hand when weapon equipped in the
// other"): the game takes no whistle from its aim stance (the simulator: the Schofield and the Winchester raised, LT held
// by AimWhenRaised: the press sent, IS_ACTOR_WHISTLING 0, the horse stayed at 20 m; lowered, it came). A whistle pressed
// while LT is held: LT let go for kWhistleDropMs from the press, the whistle held back until the aim pose is down (at most
// kWhistleWaitMs), then pressed for kWhistlePulseMs
constexpr double kWhistleDropMs = 900.0, kWhistleWaitMs = 300.0, kWhistlePulseMs = 200.0;
double g_wh_drop_until = 0, g_wh_since = 0, g_wh_pulse_until = 0;
bool g_wh_prev = false, g_wh_pending = false;
std::atomic<uint64_t> g_wh_drops{0};
std::atomic<float> g_wh_wait_ms{0.0f};
std::atomic<uint64_t> g_sprint_frames{0};
std::atomic<bool> g_draw_any{true};     // [Hands] DrawToGrabbingHand
std::atomic<int> g_draw_hand{-1};       // the controller that drew the gun in hand (-1: the layout's)
std::atomic<int> g_pend_hand{-1}, g_pend_weapon{-1};  // a draw queued while another gun was in hand: its hand, once it is in
std::atomic<double> g_pend_ms{0};
bool g_trig_swapped = false;            // the triggers' roles swapped (the gun in the layout's other hand)
float g_aim_lead_ms = 250, g_aim_tail_ms = 500;
bool g_aim_inj = false;
double g_aim_since = 0, g_aim_until = 0;
bool g_rt_pending = false;     // a pull made while the gun was coming up (held back): it still fires
double g_rt_pulse_until = 0;   // that pull's press, sent once the aim pose is up
std::atomic<uint64_t> g_aim_injects{0}, g_rt_held{0}, g_aim_tail_frames{0}, g_raised_frames{0};

int target_by_name(const std::string& n, int fallback) {
    for (int t = 0; t < kTargets; ++t)
        if (_stricmp(n.c_str(), kTargetNames[t]) == 0) return t;
    return fallback;
}
// presenting thread
std::mutex g_inject_mutex;
uint16_t g_inj_buttons = 0;
uint8_t g_inj_rt = 0, g_inj_lt = 0;
double g_inj_rt_from = 0;  // RT only from then (an LT lead)
bool g_inj_lt_shown = false;  // a pad state with the lead's LT went out: RT may follow (review 1: a frame longer than the
                              // lead put LT and RT into the same pad state, the zero lead that never punched)
int16_t g_inj_ly = 0;
double g_inj_until = 0;
std::atomic<uint64_t> g_injects{0};
bool g_grip_down[2] = {false, false};
bool g_grip_raw[2] = {false, false}, g_grip_held_back[2] = {false, false};  // the grip itself; held back for a holster
bool g_menu_was = false, g_menu_chord = false, g_menu_fired = false;
double g_menu_since = 0, g_start_until = 0;
// [Controls] WristMenu: the off hand's Y (its upper button) held with the wrist HUD in view toggles the menu
std::atomic<bool> g_wrist_menu{true};
bool g_wy_held = false, g_wy_fired = false;
double g_wy_since = 0, g_wy_tap_until = 0;
// [Controls] MenuControls (2026-10-09): while a game menu is open (the satchel, the pause menu, the shop) RT presses RB
// and LT presses LB (their tabs), the left stick is the D-pad's steps (the game's own stick scrolled two to three items a
// flick and the whole list in a second)
std::atomic<bool> g_menu_ctl{true};
int g_w_shop = -1;  // UI_ISFOCUSED("ShopMenu"), every script tick (the shop does not stop the scripts)
const char kShopLayer[] = "ShopMenu";
std::atomic<uint64_t> g_menu_tabs{0}, g_menu_steps{0}, g_menu_frames{0};
bool g_menu_rt = false, g_menu_lt = false;  // the triggers' state (the pad's thread)
double g_menu_tab_until = 0.0;
uint16_t g_menu_tab_button = 0;
int g_menu_dir = -1;  // the stick's step direction held: 0 up, 1 down, 2 left, 3 right
double g_menu_dir_since = 0.0, g_menu_next_step = 0.0, g_menu_step_until = 0.0;
constexpr double kStepPulse = 70.0, kStepFirstRepeat = 450.0, kStepRepeat = 180.0;
bool game_menu_now() {
    const int f = vr_mode::state_flags();
    if ((f & (2 | 4)) && !(f & 1)) return true;  // paused or the scripts stopped, the scene still drawn (not a load)
    uint64_t v = 0;
    return g_w_shop >= 0 && api::watched(g_w_shop, &v, nullptr) && (v & 0xff) == 1;
}
// [Controls] WristSatchel (2026-10-09, the user's choice): that short press is the pad's Back (the satchel), not Y
std::atomic<bool> g_wrist_satchel{false};
std::atomic<uint64_t> g_satchels{0}, g_wy_single{0}, g_y_frames{0};
double g_back_until = 0;
// run 9 item 2 (the user: "too easy to accidentally trigger with one press"): two taps within SatchelTapMs open the
// satchel. The first tap is held back for that window (from its release): a second press inside it makes the pair (no Y
// at all); none, and the game gets its Y at the window's end (a single Y SatchelTapMs late)
float g_satchel_tap_ms = 350.0f;
bool g_wy_pending = false, g_wy_second = false;
double g_wy_pending_at = 0;
std::atomic<float> g_wy_single_delay{0.0f};  // the last single tap's Y, from its release (ms)
double g_combo_since = 0;
bool g_combo_fired = false;
uint64_t g_seen_updates[2] = {0, 0};
double g_seen_ms[2] = {0, 0};
bool g_was_in_use = false;
std::atomic<bool> g_menu_toggle{false}, g_recentre{false};
std::atomic<uint64_t> g_starts{0}, g_toggles{0}, g_frames_in_use{0};
xinput::PadState g_last{};

int16_t axis(float v) {
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    return static_cast<int16_t>(v * 32767.0f);
}

uint8_t trig(float v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

// A controller is in use while it reports input; the real source must also be fresh (the XR frame loop syncs it each
// frame; a stopped session leaves its last state behind). The synthetic source is sticky by design.
bool in_use(int h, const hands::Hand& d, double now) {
    if (!d.connected) return false;
    if (hands::synthetic()) return true;
    if (d.updates != g_seen_updates[h]) {
        g_seen_updates[h] = d.updates;
        g_seen_ms[h] = now;
    }
    return now - g_seen_ms[h] < 250;
}

}  // namespace

void init() {
    g_grip_on = config::get_float("Controls", "GripThreshold", 0.6f);
    g_grip_off = g_grip_on - 0.2f;
    g_menu_hold_ms = config::get_float("Controls", "MenuHoldMs", 600.0f);
    g_menu_chord_on = config::get_bool("Controls", "MenuChord", true);
    g_wrist_menu = config::get_bool("Controls", "WristMenu", true);
    g_wrist_satchel = config::get_bool("Controls", "WristSatchel", false);
    g_satchel_tap_ms = config::get_float("Controls", "SatchelTapMs", 350.0f);
    g_satchel_tap_ms = !(g_satchel_tap_ms >= 150.0f) ? 150.0f : g_satchel_tap_ms > 800.0f ? 800.0f : g_satchel_tap_ms;
    log::info("[controls] the satchel by a double tap of Y at the wrist HUD (WristSatchel) %d, within %.0f ms (a single tap: Y that late)",
              g_wrist_satchel.load() ? 1 : 0, g_satchel_tap_ms);
    g_menu_ctl = config::get_bool("Controls", "MenuControls", true);
    g_w_shop = api::watch_native(0x6F2509E8u, reinterpret_cast<uint64_t>(kShopLayer), 1);  // UI_ISFOCUSED
    g_left_handed = config::get_bool("Controls", "LeftHanded", false);
    g_click_brake = config::get_bool("Horse", "StickClickBrake", true);
    g_trigger_aims = config::get_bool("Hands", "TriggerAims", true);
    g_aims_riding = config::get_bool("Hands", "TriggerAimsRiding", true);
    g_aim_lead_ms = config::get_float("Hands", "TriggerAimLeadMs", 400.0f);
    g_aim_raised = config::get_bool("Hands", "AimWhenRaised", true);
    g_sprint_drops = config::get_bool("Hands", "SprintDropsAim", true);
    g_sprint_hold_ms = config::get_float("Hands", "SprintHoldMs", 700.0f);
    g_sprint_hold_ms = g_sprint_hold_ms < 0 ? 0 : g_sprint_hold_ms > 5000 ? 5000 : g_sprint_hold_ms;
    g_jump_drops = config::get_bool("Hands", "JumpDropsAim", false);
    g_jump_hold_ms = config::get_float("Hands", "JumpHoldMs", 1500.0f);
    g_jump_hold_ms = !(g_jump_hold_ms >= 0) ? 0 : g_jump_hold_ms > 5000 ? 5000 : g_jump_hold_ms;
    log::info("[controls] a jump with a gun in hand lets the aim go first (JumpDropsAim) %d, for %.0f ms", g_jump_drops.load() ? 1 : 0, g_jump_hold_ms);
    g_draw_any = config::get_bool("Hands", "DrawToGrabbingHand", true);
    g_ready_gate = config::get_bool("Hands", "TriggerAimReady", true);
    g_aim_tail_ms = config::get_float("Hands", "TriggerAimTailMs", 500.0f);
    g_aim_lead_ms = g_aim_lead_ms < 0 ? 0 : g_aim_lead_ms > 2000 ? 2000 : g_aim_lead_ms;
    g_aim_tail_ms = g_aim_tail_ms < 0 ? 0 : g_aim_tail_ms > 10000 ? 10000 : g_aim_tail_ms;
    g_two_aims = config::get_bool("Reload", "TwoHandedAims", true);
    log::info("[controls] the right trigger alone fires %d (riding %d, lead %.0f ms, tail %.0f ms, aim while raised %d, at fire-ready %d, "
              "sprint lets it go %d (%.0f ms)); the left stick click brakes a horse %d",
              g_trigger_aims.load() ? 1 : 0, g_aims_riding.load() ? 1 : 0, g_aim_lead_ms, g_aim_tail_ms, g_aim_raised.load() ? 1 : 0,
              g_ready_gate.load() ? 1 : 0, g_sprint_drops.load() ? 1 : 0, g_sprint_hold_ms, g_click_brake.load() ? 1 : 0);
    std::string remapped;
    for (int s = 0; s < kSources; ++s) {
        int t = target_by_name(config::get_string("Controls", kSourceKeys[s], kTargetNames[kDefault[s]]), kDefault[s]);
        g_map[s] = static_cast<uint8_t>(t);
        if (t != kDefault[s]) remapped += std::string(" ") + kSourceKeys[s] + "=" + kTargetNames[t];
    }
    log::info("[controls] mapping: %s%s%s", remapped.empty() ? "the game's scheme" : "remapped:", remapped.c_str(),
              g_left_handed.load() ? " (left-handed: the controllers swapped)" : "");
}

bool pad(xinput::PadState* out) {
    double now = log::now_ms();
    hands::Hand l = hands::get(0), r = hands::get(1);
    bool use_l = in_use(0, l, now), use_r = in_use(1, r, now);
    bool use = use_l || use_r;
    if (use != g_was_in_use) {
        g_was_in_use = use;
        log::info("[controls] controllers %s", use ? "drive the game's pad" : "not in use (the gamepad alone)");
    }
    if (!use) {
        g_menu_was = false;
        g_combo_since = 0;
        wheel::cancel();
        return false;
    }
    if (!use_l) l = hands::Hand{};
    if (!use_r) r = hands::Hand{};
    bool whistle_btn = false;
    {  // [Gestures] Whistle (run 8 item 4): a hand at the mouth, its trigger: the whistle (D-pad up); that trigger kept
        float trig[2] = {l.trigger, r.trigger};
        whistle_btn = whistle::poll(now, trig);
        l.trigger = trig[0];
        r.trigger = trig[1];
    }
    if (const dual::State ds = dual::state(); ds.on) (ds.ctrl ? r : l).trigger = 0.0f;  // [Hands] DualWield: the second gun's own
    if (g_left_handed.load(std::memory_order_relaxed)) std::swap(l, r);  // the mirror: every role swapped
    {  // the gun in the layout's other hand (drawn by it): its trigger fires, the other aims; changed only with both let go
        const bool want = gun_hand() != layout_gun_hand();
        if (want != g_trig_swapped && l.trigger < 0.15f && r.trigger < 0.15f) g_trig_swapped = want;
        if (g_trig_swapped) std::swap(l.trigger, r.trigger);
    }
    xinput::PadState p;
    // the chord for controllers without a menu button: both B buttons are the menu button (and not B / Y)
    const bool chord = g_menu_chord_on && (l.buttons & hands::kB) && (r.buttons & hands::kB);
    if (chord) {
        l.buttons &= ~static_cast<uint32_t>(hands::kB);
        r.buttons &= ~static_cast<uint32_t>(hands::kB);
    }
    // menu button: tap, hold, or a modifier for the right stick (D-pad) and A (Back)
    bool menu_down = chord || ((l.buttons | r.buttons) & hands::kMenu) != 0;
    bool right_a = (r.buttons & hands::kA) != 0;
    float rx = r.stick[0], ry = r.stick[1];
    if (menu_down) {
        if (!g_menu_was) {
            g_menu_since = now;
            g_menu_chord = false;
            g_menu_fired = false;
        }
        if (std::fabs(rx) > 0.6f || std::fabs(ry) > 0.6f) {
            if (std::fabs(rx) > std::fabs(ry)) p.buttons |= rx > 0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
            else p.buttons |= ry > 0 ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN;
            g_menu_chord = true;
        }
        if (right_a) {
            p.buttons |= XINPUT_GAMEPAD_BACK;
            g_menu_chord = true;
        }
        rx = ry = 0;
        right_a = false;
        if (!g_menu_chord && !g_menu_fired && now - g_menu_since >= g_menu_hold_ms) {
            g_menu_fired = true;
            g_menu_toggle = true;
            g_toggles.fetch_add(1);
            log::info("[controls] menu button held: the RDRVR menu");
        }
    } else if (g_menu_was && !g_menu_chord && !g_menu_fired) {
        g_start_until = now + 120;  // a tap: Start
        g_starts.fetch_add(1);
    }
    g_menu_was = menu_down;
    if (now < g_start_until) p.buttons |= XINPUT_GAMEPAD_START;
    // the game's GENERIC.WHISTLE (D-pad up): pressed below, after the aim stance's check
    // the wrist menu: with the wrist HUD in view (the palm flat, looked at), the off hand's Y held MenuHoldMs toggles
    // the menu. Y is the mod's from its press while the HUD shows (the hold keeps it if the HUD goes); a shorter press
    // reaches the game as a tap at the release
    {
        const bool y = (l.buttons & hands::kB) != 0;
        const bool sat = g_wrist_satchel.load(std::memory_order_relaxed);
        // a press while the first tap waits is the pair's second, the HUD in view or not
        if (y && (g_wy_held || g_wy_pending || (g_wrist_menu.load(std::memory_order_relaxed) && xr::wrist_hud_shown()))) {
            if (!g_wy_held) {
                g_wy_held = true;
                g_wy_fired = false;
                g_wy_since = now;
                g_wy_second = g_wy_pending;
                g_wy_pending = false;  // the first tap's Y is never sent
            }
            if (!g_wy_fired && now - g_wy_since >= g_menu_hold_ms) {
                g_wy_fired = true;
                g_menu_toggle = true;
                g_toggles.fetch_add(1);
                log::info("[controls] Y held with the wrist HUD in view: the RDRVR menu");
            }
            l.buttons &= ~static_cast<uint32_t>(hands::kB);
        } else if (g_wy_held) {
            if (!g_wy_fired) {
                if (!sat) {
                    g_wy_tap_until = now + 120;  // a tap: Y for the game
                } else if (g_wy_second) {  // [Controls] WristSatchel: the second tap opens the satchel
                    g_back_until = now + 120;
                    g_satchels.fetch_add(1, std::memory_order_relaxed);
                    log::info("[controls] Y tapped twice with the wrist HUD in view: the satchel (Back)");
                } else {
                    g_wy_pending = true;  // the first: held back for the window
                    g_wy_pending_at = now;
                }
            }
            g_wy_held = false;
            g_wy_second = false;
        }
        if (g_wy_pending && (now - g_wy_pending_at >= g_satchel_tap_ms || !sat)) {  // no second tap: the game's Y now
            g_wy_pending = false;
            g_wy_tap_until = now + 120;
            g_wy_single.fetch_add(1, std::memory_order_relaxed);
            g_wy_single_delay.store(static_cast<float>(now - g_wy_pending_at), std::memory_order_relaxed);
        }
        if (now < g_wy_tap_until) l.buttons |= hands::kB;
        if (now < g_back_until) p.buttons |= XINPUT_GAMEPAD_BACK;  // the game's GENERIC.INVENTORY
    }
    // both stick clicks held a second: recentre
    if ((l.buttons & hands::kStick) && (r.buttons & hands::kStick)) {
        if (!g_combo_since) g_combo_since = now;
        if (!g_combo_fired && now - g_combo_since >= 1000) {
            g_combo_fired = true;
            g_recentre = true;
            log::info("[controls] both stick clicks held 1 s: recentre");
        }
    } else {
        g_combo_since = 0;
        g_combo_fired = false;
    }
    actions::filter_stick(&rx, &ry);  // [Reload] ManualActions: a revolver opens by a flick down (that cone consumed)
    if (menu::visible()) {  // the menu has the controllers
        wheel::cancel();
        return false;
    }
    RdrvrActorState st{};
    const bool have_st = api::actor_state(&st);
    {  // the horse brake: the route decided at the press
        const bool seated = have_st && (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING));
        const bool lclk = (l.buttons & hands::kStick) != 0, rclk = (r.buttons & hands::kStick) != 0;
        if (lclk && !g_lclk_was) {
            g_click_route = g_click_brake.load(std::memory_order_relaxed) && seated && !rclk && !(st.weapon_flags & RDRVR_WEAPON_DEADEYE) ? 1 : 0;
            if (g_click_route == 1) g_brakes.fetch_add(1, std::memory_order_relaxed);
        } else if (!lclk) {
            g_click_route = 0;
        } else if (g_click_route == 1 && (!seated || rclk)) {
            g_click_route = 2;  // dismounted, or the recentre chord: neither the brake nor a crouch
        }
        g_lclk_was = lclk;
    }
    const float grips[2] = {l.grip, r.grip};
    const bool swapped = g_left_handed.load(std::memory_order_relaxed);
    for (int h = 0; h < 2; ++h) {
        // a grip at a body holster (a draw, putting the gun away, a round from the chest, the foregrip) is the
        // holster's from its press until it is let go. The press is judged by where the hand was at the last frame
        // end (holster.cpp decides there, possibly after this poll), so not even its first frame reaches the game.
        const int ph = swapped ? 1 - h : h;  // the controller
        const bool was = g_grip_raw[h];
        if (grips[h] > g_grip_on) g_grip_raw[h] = true;
        else if (grips[h] < g_grip_off) g_grip_raw[h] = false;
        if (g_grip_raw[h] && !was && (holster::grip_wanted(ph) || actions::grip_wanted(ph) || aim::grip_throw_wanted(ph))) g_grip_held_back[h] = true;
        // [Physics] Grab: an empty hand's press held back until the grab is known (kept if a prop is held, the game's
        // if none was there, or after 200 ms)
        static bool grab_wait[2] = {false, false};
        static uint32_t grab_miss0[2] = {0, 0};
        static double grab_t0[2] = {0, 0};
        if (g_grip_raw[h] && !was && !g_grip_held_back[h] && physics::grip_wanted(ph)) {
            g_grip_held_back[h] = true;
            grab_wait[h] = true;
            grab_miss0[h] = physics::grab_misses(ph);
            grab_t0[h] = log::now_ms();
        }
        if (grab_wait[h] && g_grip_raw[h]) {
            if (physics::grab_held(ph)) {
                grab_wait[h] = false;  // the mod's until let go
            } else if (physics::grab_misses(ph) != grab_miss0[h] || log::now_ms() - grab_t0[h] > 200.0) {
                grab_wait[h] = false;
                g_grip_held_back[h] = false;  // nothing grabbed: the game's from now on
            }
        }
        if (!g_grip_raw[h]) {
            g_grip_held_back[h] = false;
            grab_wait[h] = false;
        }
        g_grip_down[h] = g_grip_raw[h] && !g_grip_held_back[h] && !holster::grip_consumed(ph);
    }
    {  // [Holsters] Mode=wheel (run 8 item 5): a free grip holds the game's weapon wheel, steered by the hand
        bool free[2] = {false, false}, taken[2] = {false, false};
        for (int h = 0; h < 2; ++h) free[swapped ? 1 - h : h] = g_grip_down[h];
        wheel::poll(now, free, taken);
        for (int h = 0; h < 2; ++h)
            if (taken[swapped ? 1 - h : h]) g_grip_down[h] = false;
    }
    // each source's value (0..1: a button is 0 or 1, a trigger analog), then onto its target: a button target is
    // pressed above half, an LT/RT target takes the largest value mapped to it
    const float value[kSources] = {right_a ? 1.0f : 0.0f,
                                   (r.buttons & hands::kB) ? 1.0f : 0.0f,
                                   (l.buttons & hands::kA) ? 1.0f : 0.0f,
                                   (l.buttons & hands::kB) ? 1.0f : 0.0f,
                                   (l.buttons & hands::kStick) ? 1.0f : 0.0f,
                                   (r.buttons & hands::kStick) ? 1.0f : 0.0f,
                                   g_grip_down[0] ? 1.0f : 0.0f,
                                   g_grip_down[1] ? 1.0f : 0.0f,
                                   l.trigger,
                                   r.trigger};
    float lt = 0, rt = 0;
    for (int s = 0; s < kSources; ++s) {
        int t = g_map[s].load(std::memory_order_relaxed);
        if (s == kSrcLeftStickClick && g_click_route) t = g_click_route == 1 ? kPadRB : kNone;
        if (t == kPadLT) lt = value[s] > lt ? value[s] : lt;
        else if (t == kPadRT) rt = value[s] > rt ? value[s] : rt;
        else if (t > kNone && t < kTargets && value[s] > 0.5f) p.buttons |= kTargetButton[t];
    }
    if (actions::fire_blocked()) rt = 0.0f;  // [Reload] ManualActions: the gun's action not worked (open, or not cycled)
    const bool menu_mode = g_menu_ctl.load(std::memory_order_relaxed) && game_menu_now();
    if (menu_mode) {  // [Controls] MenuControls: the triggers are the tabs (RB, LB), a press of 120 ms on each pull
        g_menu_frames.fetch_add(1, std::memory_order_relaxed);
        const bool rt_on = rt > (g_menu_rt ? 0.35f : 0.6f), lt_on = lt > (g_menu_lt ? 0.35f : 0.6f);
        if ((rt_on && !g_menu_rt) || (lt_on && !g_menu_lt)) {
            g_menu_tab_button = rt_on && !g_menu_rt ? XINPUT_GAMEPAD_RIGHT_SHOULDER : XINPUT_GAMEPAD_LEFT_SHOULDER;
            g_menu_tab_until = now + 120.0;
            g_menu_tabs.fetch_add(1, std::memory_order_relaxed);
        }
        g_menu_rt = rt_on;
        g_menu_lt = lt_on;
        if (now < g_menu_tab_until) p.buttons |= g_menu_tab_button;
        lt = rt = 0.0f;
    } else {
        g_menu_rt = g_menu_lt = false;
        g_menu_dir = -1;
    }
    aim::grip_throw_input(&lt, &rt);  // [Gestures] ThrowByGrip: the trigger with the grip holding a throwable readies it
    {  // the right trigger alone fires: LT held for it
        const bool gun = have_st && reload::is_gun(st.weapon);
        const bool de = have_st && (st.weapon_flags & RDRVR_WEAPON_DEADEYE);
        const bool riding = have_st && (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING));
        const bool base = pose::anchor_active() && gun;
        const bool allow = base && g_trigger_aims.load(std::memory_order_relaxed) && (g_aims_riding.load(std::memory_order_relaxed) || !riding);
        // two-handed ([Reload] TwoHandedAims, its own switch): the aim stance the whole time it is held
        const bool two = base && g_two_aims.load(std::memory_order_relaxed) && reload::is_long_gun(st.weapon) && holster::two_hand_blend() > 0.0f;
        // sprinting on foot (the game's A, from whichever button drives it): the aim stance would stop the sprint, so a
        // raised gun's LT is let go from the press until SprintHoldMs after the last (RDR's sprint is tapped). A pull
        // still aims and fires; riding, A spurs and the stance stays.
        if (!riding && (p.buttons & XINPUT_GAMEPAD_A)) g_sprint_until = now + g_sprint_hold_ms;
        const bool sprint = !riding && g_sprint_drops.load(std::memory_order_relaxed) && now < g_sprint_until;
        // [Hands] JumpDropsAim (run 7 item 1f): with a gun in hand the game takes no jump from its aim stance (the
        // simulator: the press reached the game, the aim pose dropped, no jump; with the fists the same press jumped).
        // The game's jump (X, from whichever button drives it) on foot with a gun: LT let go from the press for
        // JumpHoldMs, and the press itself held back until the aim pose is down (at most 250 ms), then sent as a pulse
        // of 150 ms (a tap shorter than the wait is not lost).
        {
            const bool x = (p.buttons & XINPUT_GAMEPAD_X) != 0;
            const bool on = !riding && gun && g_jump_drops.load(std::memory_order_relaxed);
            if (on && x && !g_jump_prev && !g_jump_pending && now >= g_jump_pulse_until) {
                g_jump_until = now + g_jump_hold_ms;
                g_jump_pending = true;
                g_jump_since = now;
                g_jump_presses.fetch_add(1, std::memory_order_relaxed);
            }
            g_jump_prev = x;
            if (g_jump_pending) {
                if (!holster::aim_pose() || now - g_jump_since >= 250.0) {
                    g_jump_pending = false;
                    g_jump_pulse_until = now + 150.0;
                    g_jump_waits.fetch_add(1, std::memory_order_relaxed);
                    g_jump_wait_ms.store(static_cast<float>(now - g_jump_since), std::memory_order_relaxed);
                } else {
                    p.buttons &= static_cast<WORD>(~XINPUT_GAMEPAD_X);  // held back while the aim pose is up
                }
            }
            if (now < g_jump_pulse_until) p.buttons |= XINPUT_GAMEPAD_X;
        }
        const bool jump = !riding && g_jump_drops.load(std::memory_order_relaxed) && now < g_jump_until;
        {  // [Gestures] Whistle from the aim stance: LT let go first (above: kWhistleDropMs)
            const bool press = whistle_btn && !g_wh_prev;
            g_wh_prev = whistle_btn;
            if (press && gun && (g_aim_inj || lt >= 0.3f)) {
                g_wh_drop_until = now + kWhistleDropMs;
                g_wh_pending = true;
                g_wh_since = now;
                g_wh_drops.fetch_add(1, std::memory_order_relaxed);
            }
            if (g_wh_pending && (!holster::aim_pose() || now - g_wh_since >= kWhistleWaitMs)) {
                g_wh_pending = false;
                g_wh_pulse_until = now + kWhistlePulseMs;
                g_wh_wait_ms.store(static_cast<float>(now - g_wh_since), std::memory_order_relaxed);
                log::info("[whistle] from the aim stance: LT let go, the whistle sent after %.0f ms", now - g_wh_since);
            }
            if ((whistle_btn && !g_wh_pending) || now < g_wh_pulse_until) p.buttons |= XINPUT_GAMEPAD_DPAD_UP;
        }
        if (!g_wh_pending && now >= g_wh_pulse_until && rt > 0.15f && g_wh_drop_until > now) g_wh_drop_until = now;  // a pull: the aim back
        const bool whist = now < g_wh_drop_until;
        if (whist) lt = rt = 0.0f;  // the real LT too, and no pull fires meanwhile (RT alone would shoot from the hip)
        const bool raised = base && g_aim_raised.load(std::memory_order_relaxed) && (g_aims_riding.load(std::memory_order_relaxed) || !riding) &&
                            holster::gun_raised() && !sprint && !jump && !whist;
        if (sprint && base && holster::gun_raised()) g_sprint_frames.fetch_add(1, std::memory_order_relaxed);
        if (raised) g_raised_frames.fetch_add(1, std::memory_order_relaxed);
        auto stop = [&] {
            g_aim_inj = false;
            g_rt_pending = false;
            g_rt_pulse_until = 0;
        };
        if (!menu_mode && lt < 0.3f && (allow || two || raised || (g_aim_inj && de))) {  // the real LT is not held
            const bool pulled = rt > 0.15f && !whist;
            if (two || raised || (allow && pulled)) {
                if (!g_aim_inj) {
                    g_aim_inj = true;
                    g_aim_since = now;
                    g_aim_injects.fetch_add(1, std::memory_order_relaxed);
                }
                g_aim_until = now + g_aim_tail_ms;  // two-handed or raised: the tail runs from when it ends
            }
            if (!pulled && (holster::gun_at_zone() || sprint || jump || whist) && g_aim_until > now) g_aim_until = now;  // into a holster, sprinting, jumping, whistling: no tail
            // held while pulled, two-handed, the tail, and never let go in Dead Eye (that fires the marks)
            if (g_aim_inj && (two || raised || pulled || now < g_aim_until || de)) {
                lt = 1.0f;
                if (!two && !pulled && !de) g_aim_tail_frames.fetch_add(1, std::memory_order_relaxed);
                const bool up = g_ready_gate.load(std::memory_order_relaxed) ? holster::fire_ready() : holster::aim_pose();
                if (!de && !up && now - g_aim_since < g_aim_lead_ms) {
                    if (pulled) g_rt_pending = true;
                    rt = 0.0f;  // the pull waits for the aim pose, so the game sees its press there
                    g_rt_held.fetch_add(1, std::memory_order_relaxed);
                } else if (g_rt_pending) {
                    if (pulled)
                        g_rt_pending = false;  // still held: its press reaches the game now
                    else if (g_rt_pulse_until == 0) {
                        g_rt_pulse_until = now + 50;  // let go while the gun came up: a short press now, so it fires
                        if (g_aim_until < now + 100) g_aim_until = now + 100;  // LT outlasts it
                    }
                }
                if (g_rt_pulse_until > 0) {
                    if (now < g_rt_pulse_until) {
                        rt = 1.0f;
                    } else {
                        g_rt_pulse_until = 0;
                        g_rt_pending = false;
                    }
                }
            } else {
                stop();
            }
        } else {
            stop();
        }
    }
    p.lt = trig(lt);
    p.rt = trig(rt);
    p.lx = axis(l.stick[0]);
    p.ly = axis(l.stick[1]);
    if (menu_mode) {  // [Controls] MenuControls: the left stick as the D-pad's steps, its axes held back
        const float sx = l.stick[0], sy = l.stick[1];
        const float ax = std::fabs(sx), ay = std::fabs(sy);
        int dir = -1;
        const float on = g_menu_dir >= 0 ? 0.35f : 0.55f;  // let go below 0.35 once held
        if (ay >= ax && ay > on) dir = sy > 0.0f ? 0 : 1;
        else if (ax > ay && ax > on) dir = sx > 0.0f ? 3 : 2;
        if (dir != g_menu_dir) {
            g_menu_dir = dir;
            g_menu_dir_since = now;
            g_menu_next_step = now;  // a new push: a step at once
        }
        if (dir >= 0 && now >= g_menu_next_step) {
            g_menu_step_until = now + kStepPulse;
            g_menu_next_step = now + (now - g_menu_dir_since < 1.0 ? kStepFirstRepeat : kStepRepeat);
            g_menu_steps.fetch_add(1, std::memory_order_relaxed);
        }
        static const uint16_t kPadDir[4] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
        if (dir >= 0 && now < g_menu_step_until) p.buttons |= kPadDir[dir];
        p.lx = p.ly = 0;
    }
    p.rx = axis(rx);
    p.ry = axis(ry);
    {
        std::lock_guard lock(g_inject_mutex);
        if (now < g_inj_until) {
            p.buttons |= g_inj_buttons;
            if (now >= g_inj_rt_from && (g_inj_lt_shown || !g_inj_lt) && g_inj_rt > p.rt) p.rt = g_inj_rt;
            if (g_inj_lt > p.lt) p.lt = g_inj_lt;
            if (g_inj_lt) g_inj_lt_shown = true;
            if (g_inj_ly) p.ly = g_inj_ly;
        }
    }
    g_frames_in_use.fetch_add(1, std::memory_order_relaxed);
    if (p.buttons & XINPUT_GAMEPAD_Y) g_y_frames.fetch_add(1, std::memory_order_relaxed);  // run 9: the Ys the game got (the tests)
    g_last = p;
    *out = p;
    return true;
}

bool take_menu_toggle() { return g_menu_toggle.exchange(false); }

void inject(uint16_t buttons, uint8_t rt, int16_t ly, int ms, uint8_t lt, int rt_after_ms) {
    std::lock_guard lock(g_inject_mutex);
    const double now = log::now_ms();
    g_inj_buttons = buttons;
    g_inj_rt = rt;
    g_inj_lt = lt;
    g_inj_ly = ly;
    g_inj_rt_from = now + (rt_after_ms > 0 ? rt_after_ms : 0);
    g_inj_until = now + (rt_after_ms > 0 ? rt_after_ms : 0) + ms;
    g_inj_lt_shown = false;
    g_injects.fetch_add(1, std::memory_order_relaxed);
}

const char* source_label(int s) { return s >= 0 && s < kSources ? kSourceLabels[s] : "?"; }
const char* source_key(int s) { return s >= 0 && s < kSources ? kSourceKeys[s] : "?"; }
const char* target_name(int t) { return t >= 0 && t < kTargets ? kTargetNames[t] : "?"; }
int mapping(int s) { return s >= 0 && s < kSources ? g_map[s].load() : 0; }

void set_mapping(int s, int t) {
    if (s < 0 || s >= kSources || t < 0 || t >= kTargets) return;
    g_map[s] = static_cast<uint8_t>(t);
    config::set("Controls", kSourceKeys[s], kTargetNames[t]);
    log::info("[controls] %s -> %s", kSourceKeys[s], kTargetNames[t]);
}

void reset_mapping() {
    for (int s = 0; s < kSources; ++s) set_mapping(s, kDefault[s]);
}

bool left_handed() { return g_left_handed.load(); }

void set_left_handed(bool on) {
    g_left_handed = on;
    g_draw_hand = -1;  // the layout's gun hand again
    g_pend_hand = -1;
    config::set("Controls", "LeftHanded", on ? "1" : "0");
    log::info("[controls] left-handed %s", on ? "on: the controllers swapped" : "off");
}

bool click_brake() { return g_click_brake.load(); }
void set_click_brake(bool on) {
    g_click_brake = on;
    config::set("Horse", "StickClickBrake", on ? "1" : "0");
}
bool trigger_aims() { return g_trigger_aims.load(); }
void set_trigger_aims(bool on) {
    g_trigger_aims = on;
    config::set("Hands", "TriggerAims", on ? "1" : "0");
}
void set_trigger_aims_session(bool on) { g_trigger_aims = on; }
bool wrist_satchel() { return g_wrist_satchel.load(); }
void set_wrist_satchel(bool on, bool save) {
    if (g_wrist_satchel.exchange(on) != on) log::info("[controls] a double tap of Y at the wrist HUD: %s", on ? "the satchel" : "Y twice");
    if (save) config::set("Controls", "WristSatchel", on ? "1" : "0");
}
bool aim_when_raised() { return g_aim_raised.load(); }
void set_aim_when_raised(bool on, bool save) {
    g_aim_raised = on;
    if (save) config::set("Hands", "AimWhenRaised", on ? "1" : "0");
}
bool jump_drops_aim() { return g_jump_drops.load(); }
void set_jump_drops_aim(bool on, bool save) {
    if (g_jump_drops.exchange(on) != on) log::info("[controls] a jump with a gun in hand lets the aim go first: %d", on ? 1 : 0);
    if (save) config::set("Hands", "JumpDropsAim", on ? "1" : "0");
}
bool sprint_drops_aim() { return g_sprint_drops.load(); }
void set_sprint_drops_aim(bool on, bool save) {
    g_sprint_drops = on;
    if (save) config::set("Hands", "SprintDropsAim", on ? "1" : "0");
}
void set_click_brake_session(bool on) { g_click_brake = on; }

int layout_gun_hand() { return g_left_handed.load() ? 0 : 1; }
bool draw_any_active() { return g_draw_any.load(std::memory_order_relaxed) && (!g_left_handed.load() || body::gun_in_gun_hand()); }
int gun_hand() {
    const int d = g_draw_hand.load(std::memory_order_relaxed);
    return d >= 0 && draw_any_active() ? d : layout_gun_hand();
}
void set_draw_hand(int ctrl, int weapon, bool now) {
    if (ctrl < -1 || ctrl > 1) return;
    if (!now && ctrl >= 0) {  // the old gun stays in its hand until the new one is in hand
        g_pend_weapon = weapon;
        g_pend_ms = log::now_ms();
        g_pend_hand = ctrl;
        return;
    }
    g_pend_hand = -1;
    const int was = g_draw_hand.exchange(ctrl);
    if (was != ctrl && draw_any_active())
        log::info("[controls] gun hand: the %s controller%s", gun_hand() ? "right" : "left", ctrl < 0 ? " (the layout's)" : " (it drew the gun)");
}
void commit_draw_hand(int weapon_in_hand, bool in_hand) {
    const int p = g_pend_hand.load(std::memory_order_relaxed);
    if (p < 0) return;
    if (in_hand && weapon_in_hand == g_pend_weapon.load(std::memory_order_relaxed)) {
        set_draw_hand(p, -1, true);
    } else if (log::now_ms() - g_pend_ms.load(std::memory_order_relaxed) > 1500.0) {
        g_pend_hand = -1;  // the game did not carry the draw out: the gun stays in its hand
        log::info("[controls] gun hand: the draw by the %s controller did not happen", p ? "right" : "left");
    }
}
bool draw_to_grabbing_hand() { return g_draw_any.load(); }
void set_draw_to_grabbing_hand(bool on, bool save) {
    g_draw_any = on;
    if (save) config::set("Hands", "DrawToGrabbingHand", on ? "1" : "0");
}
bool take_recentre() { return g_recentre.exchange(false); }

void status_text(char* out, size_t len) {
    xinput::PadState p = g_last;
    std::snprintf(out, len,
                  "controls: %s, frames %llu, last pad buttons %#06x LT %u RT %u L (%d %d) R (%d %d), starts %llu, menu toggles %llu, "
                  "wrist satchel %d (opened %llu, window %.0f ms, single taps %llu: Y after %.0f ms; Y to the game %llu frames), game menu controls %d (in a menu %d: frames %llu, tab presses %llu, stick steps %llu), "
                  "left-handed %d, chord %d, click brake %d (route %d, brakes %llu), trigger aims %d (injects %llu, RT held back %llu frames, "
                  "aim pose %d, fire-ready %d, raised %d (frames %llu), LT tail frames %llu, let go for a sprint %llu frames) | jump drops aim %d: "
                  "presses %llu, sent %llu (the last after %.0f ms) | the whistle from the aim stance %llu (the last sent after %.0f ms)",
                  g_was_in_use ? "in use" : "idle", static_cast<unsigned long long>(g_frames_in_use.load()), p.buttons, p.lt, p.rt, p.lx,
                  p.ly, p.rx, p.ry, static_cast<unsigned long long>(g_starts.load()), static_cast<unsigned long long>(g_toggles.load()),
                  g_wrist_satchel.load() ? 1 : 0, static_cast<unsigned long long>(g_satchels.load()), g_satchel_tap_ms,
                  static_cast<unsigned long long>(g_wy_single.load()), g_wy_single_delay.load(), static_cast<unsigned long long>(g_y_frames.load()),
                  g_menu_ctl.load() ? 1 : 0,
                  game_menu_now() ? 1 : 0, static_cast<unsigned long long>(g_menu_frames.load()), static_cast<unsigned long long>(g_menu_tabs.load()),
                  static_cast<unsigned long long>(g_menu_steps.load()),
                  g_left_handed.load() ? 1 : 0, g_menu_chord_on ? 1 : 0, g_click_brake.load() ? 1 : 0, g_click_route,
                  static_cast<unsigned long long>(g_brakes.load()), g_trigger_aims.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_aim_injects.load()), static_cast<unsigned long long>(g_rt_held.load()),
                  holster::aim_pose() ? 1 : 0, holster::fire_ready() ? 1 : 0, holster::gun_raised() ? 1 : 0,
                  static_cast<unsigned long long>(g_raised_frames.load()), static_cast<unsigned long long>(g_aim_tail_frames.load()),
                  static_cast<unsigned long long>(g_sprint_frames.load()), g_jump_drops.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_jump_presses.load()), static_cast<unsigned long long>(g_jump_waits.load()), g_jump_wait_ms.load(),
                  static_cast<unsigned long long>(g_wh_drops.load()), g_wh_wait_ms.load());
}

}  // namespace rdrvr::controls
