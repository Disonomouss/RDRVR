#include "core/deadeye.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/aim.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/pose.h"
#include "core/ui_layer.h"
#include "core/xinput.h"

namespace rdrvr::deadeye {
namespace {

// Game memory read and written with SEH as the backstop (no system calls: the hook runs every frame)
bool raw(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool wraw(uintptr_t a, const void* in, size_t n) {
    __try {
        std::memcpy(reinterpret_cast<void*>(a), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<int> g_level{0};       // [DeadEye] Level
std::atomic<int> g_latched{0};     // the level used: the setting as it was while Dead Eye was last off
std::atomic<int> g_unlocked{0};    // the story's, as last read
std::atomic<bool> g_sepia{true};   // [DeadEye] Sepia
std::atomic<uint64_t> g_calls{0}, g_lowered{0}, g_sepia_writes{0}, g_sepia_restores{0};

// the player info's level fields (research run 28: the natives' handlers 0x140ad8ee0, 0x140ad8f10)
constexpr uintptr_t kMultilock = 0x3bd;  // byte: multilock on (level 2 or more)
constexpr uintptr_t kPaint = 0x3be;      // bit 1: the automatic paint (level 2); clear: the paint button (level 3)

// Dead Eye's preset (FUN_140350ba0): PostFx +0x8a0 the names (char*[]), +0x8a8 their count (u16), +0x8b0 the presets
// (0x270 bytes each); a preset's +0x10 colour correction (4 floats), +0x98 saturation. *PppDirtyPtr: [0] = 1 makes the
// sky update copy the active preset again
constexpr uintptr_t kNames = 0x8a0, kCount = 0x8a8, kPresets = 0x8b0, kPresetSize = 0x270, kColour = 0x10, kSat = 0x98;
const float kNeutralColour[4] = {0.5f, 0.5f, 0.5f, 1.0f}, kNeutralSat = 1.0f;  // the default preset's (default.ppp)
bool g_saved = false;  // the originals below, from Dead Eye's own preset
float g_orig_colour[4] = {}, g_orig_sat = 0.0f;
std::atomic<int> g_preset_idx{-1};  // (the hook and the test channel)

uintptr_t postfx() {
    uintptr_t p = 0;
    return raw(anchors::addr(anchors::Id::PostFxSingleton), &p, sizeof(p)) ? p : 0;
}
// FX_Deadeye.ppp's preset (its address), found by name (the index kept while its name still matches)
uintptr_t deadeye_preset(uintptr_t pf) {
    uint16_t n = 0;
    uintptr_t names = 0, presets = 0;
    if (!pf || !raw(pf + kCount, &n, sizeof(n)) || !raw(pf + kNames, &names, sizeof(names)) || !raw(pf + kPresets, &presets, sizeof(presets)) ||
        !names || !presets || n == 0 || n > 512)
        return 0;
    auto is_it = [&](int i) {
        uintptr_t s = 0;
        char b[16] = {};
        return raw(names + static_cast<uintptr_t>(i) * 8, &s, sizeof(s)) && s && raw(s, b, 15) && _strnicmp(b, "FX_Deadeye.ppp", 15) == 0;
    };
    int idx = g_preset_idx.load(std::memory_order_relaxed);
    if (idx < 0 || idx >= n || !is_it(idx)) {
        idx = -1;
        for (int i = 0; i < n && idx < 0; ++i)
            if (is_it(i)) idx = i;
        g_preset_idx.store(idx, std::memory_order_relaxed);
    }
    return idx >= 0 ? presets + static_cast<uintptr_t>(idx) * kPresetSize : 0;
}
void mark_dirty() {
    uintptr_t d = 0;
    const uint8_t one = 1;
    if (raw(anchors::addr(anchors::Id::PppDirtyPtr), &d, sizeof(d)) && d) wraw(d, &one, 1);
}
// The game thread (the hook): the preset's colour as the setting wants it
void sepia_apply() {
    const uintptr_t pr = deadeye_preset(postfx());
    if (!pr) return;
    float c[4], s;
    if (!raw(pr + kColour, c, sizeof(c)) || !raw(pr + kSat, &s, sizeof(s))) return;
    const bool neutral = std::memcmp(c, kNeutralColour, sizeof(c)) == 0 && s == kNeutralSat;
    if (!g_sepia.load(std::memory_order_relaxed)) {
        if (neutral) return;
        std::memcpy(g_orig_colour, c, sizeof(c));  // the preset's own (again after a reload from disk)
        g_orig_sat = s;
        g_saved = true;
        if (wraw(pr + kColour, kNeutralColour, sizeof(kNeutralColour)) && wraw(pr + kSat, &kNeutralSat, sizeof(kNeutralSat))) {
            mark_dirty();
            g_sepia_writes.fetch_add(1, std::memory_order_relaxed);
            log::info("[deadeye] the sepia off: FX_Deadeye's colour (%.3f %.3f %.3f, saturation %.2f) set to the default's", c[0], c[1], c[2], s);
        }
    } else if (g_saved && !neutral) {
        g_saved = false;  // reloaded from disk meanwhile: its own colour is back already
    } else if (g_saved && neutral) {
        g_saved = false;
        if (wraw(pr + kColour, g_orig_colour, sizeof(g_orig_colour)) && wraw(pr + kSat, &g_orig_sat, sizeof(g_orig_sat))) {
            mark_dirty();
            g_sepia_restores.fetch_add(1, std::memory_order_relaxed);
            log::info("[deadeye] the sepia on: FX_Deadeye's own colour back");
        }
    }
}

using Update_t = uintptr_t (*)(uintptr_t P, uintptr_t a2);
Update_t o_update = nullptr;
// (GOAL-after-0.8.13.2 item 6; the user: "Duels need to be changed too, so that up on the joystick places the Xs and
// right trigger fires", the right stick) [DeadEye] DuelTriggerFires: in a duel's Dead Eye (player info +0x3be bit 2, set
// by the duel's FUN_1403526d0) while it paints (R +0x20 == 2), the right trigger fires the marks placed instead of
// placing one: on its press with a mark placed, the duel's time ends now, and the trigger's paint (on its press: +0x3b8,
// last frame's trigger, at or under the game's threshold) is held off for the call (+0x3b8 0xff, put back after).
// (round 116, the user: the X's and the trigger worked, "but both I and the opponent died") The first version sent the
// game's own fire (request 3) at the press: the marks went while the duel still ran, and at its time-out the score
// (counted from the marks, now gone) lost: the game had the opponent shoot John. The duel (FUN_140d078a0, the player's
// duel state D = [[actor +0x38] +0xaa8] +0x20): D +0x30 the state (3 painting, 4 judged), D +0x10c the time left,
// counted down in state 3; at its end the score D +0x108 (the marks') against D +0xd4 decides, state 4, and in duel
// mode FUN_1403480a0 sends request 3 at state 4 itself: the marks fire as the game's own. So the press sets the time
// left just under 0: the game judges the marks as placed, then fires them (a duel won as the game counts it). Only with
// the marks' score at the bar (D +0x108 >= D +0xd4) and the outcome open (D +0x54 0): an early press would only lose (the
// review); then the time runs on for more marks. The judged outcome logged after (or the time-out not held). The marks come from the paint button,
// RB, which [Controls] FlickUpRB presses with the right stick flicked up: without it, nothing changes. In VR only.
// Research: research/after-08132/notes.md, item 6 (static; no duel in the simulator). Off by default
constexpr uintptr_t kTrigLast = 0x3b8;  // byte: the fire trigger as FUN_1403480a0 last read it
constexpr uint8_t kDuelRtOn = 0x80;     // the trigger's press (half way)
std::atomic<bool> g_duel_rt{false};
std::atomic<uint64_t> g_duel_calls{0}, g_duel_fires{0}, g_duel_nomark{0}, g_duel_short{0}, g_duel_other{0};
// (round 116's review) a press that ended the time: the duel's outcome logged once judged, or the end not held
uintptr_t g_duel_ended_D = 0;
double g_duel_ended_ms = 0.0;
uint8_t g_duel_rt_prev = 0xff;  // (the game thread; 0xff: a held trigger at the start is no press)
// (GOAL-doorway-and-poker item 4.5, the review: the edge was used up by whichever call saw it first, a request-2 call's
// too: a press in the frame LT let go did nothing) the press kept 150 ms for a request-0 call in the duel to take
double g_duel_press_ms = 0.0;
// the player's duel state D (the comment above), 0 unread
uintptr_t duel_state(uintptr_t P) {
    uint16_t idx = 0;
    uintptr_t pool = 0, actor = 0, a = 0, b = 0, D = 0;
    if (!raw(P + 0x5ec, &idx, 2) || !raw(anchors::addr(anchors::Id::ActorPool), &pool, 8) || !pool || !raw(pool + idx * 0x10u, &actor, 8) ||
        !actor || !raw(actor + 0x38, &a, 8) || !a || !raw(a + 0xaa8, &b, 8) || !b || !raw(b + 0x20, &D, 8))
        return 0;
    return D;
}
int marks_placed(uintptr_t P) {  // T +0x6060 (T: the player actor +0xb8), -1 unread
    uint16_t idx = 0;
    uintptr_t pool = 0, actor = 0, T = 0;
    int32_t n = 0;
    if (!raw(P + 0x5ec, &idx, 2) || !raw(anchors::addr(anchors::Id::ActorPool), &pool, 8) || !pool || !raw(pool + idx * 0x10u, &actor, 8) ||
        !actor || !raw(actor + 0xb8, &T, 8) || !T || !raw(T + 0x6060, &n, 4))
        return -1;
    return n;
}
// the request for the call (a2 as it came, or 3: fire); *held: +0x3b8 held at 0xff for it, *keep its value to put back
// (the review: the request compared as the 32 bits the callers set; held only while the trigger is pressed at all)
uintptr_t duel_pre(uintptr_t P, uintptr_t a2, bool* held, uint8_t* keep) {
    *held = false;
    if (!g_duel_rt.load(std::memory_order_relaxed) || !P) return a2;
    const uint8_t rt = xinput::pad_rt();
    const double now = log::now_ms();
    if (rt >= kDuelRtOn && g_duel_rt_prev < kDuelRtOn) g_duel_press_ms = now;
    g_duel_rt_prev = rt;
    if (g_duel_ended_D) {  // the press's result: judged (state 4 on, the outcome D +0x54), or the time-out undone
        // (item 4.5, the review: before the gate; a loss ends Dead Eye's phase in the same update, so behind it a loss was
        // never logged and the mark lasted into the next duel)
        int32_t st = 0, out = 0;
        float t = 0.0f;
        const double age = now - g_duel_ended_ms;
        if (!raw(g_duel_ended_D + 0x30, &st, 4) || st != 3) {
            raw(g_duel_ended_D + 0x54, &out, 4);
            log::info("[deadeye] the duel judged %.0f ms after the press: state %d, outcome %d (1 won, 2 won by a disarm, 3 lost)", age, st, out);
            g_duel_ended_D = 0;
        } else if (age > 500.0) {
            raw(g_duel_ended_D + 0x10c, &t, 4);
            log::warn("[deadeye] the duel still painting 500 ms after the press (%.2f s left): the time-out did not hold", t);
            g_duel_ended_D = 0;
        }
    }
    uint8_t fl = 0;
    uintptr_t R = 0;
    int32_t phase = 0;
    if (!pose::anchor_active() || !controls::flick_up_rb_on() || !raw(P + kPaint, &fl, 1) || !(fl & 4) || !raw(P + 0x410, &R, 8) || !R ||
        !raw(R + 0x20, &phase, 4) || phase != 2)
        return a2;
    g_duel_calls.fetch_add(1, std::memory_order_relaxed);
    const uint8_t hold = 0xff;
    if (rt > 0 && raw(P + kTrigLast, keep, 1) && wraw(P + kTrigLast, &hold, 1)) *held = true;
    if (g_duel_press_ms > 0.0 && static_cast<uint32_t>(a2) == 0 && now - g_duel_press_ms < 150.0) {  // (item 4.5) the kept press
        g_duel_press_ms = 0.0;
        const int n = marks_placed(P);
        const uintptr_t D = n > 0 ? duel_state(P) : 0;
        int32_t state = 0, need = 0, out = -1;
        float left = 0.0f, score = 0.0f;
        const bool read = D && raw(D + 0x30, &state, 4) && raw(D + 0x10c, &left, 4) && raw(D + 0x108, &score, 4) && raw(D + 0xd4, &need, 4) &&
                          raw(D + 0x54, &out, 4);
        if (n <= 0) {
            g_duel_nomark.fetch_add(1, std::memory_order_relaxed);
        } else if (!read || state != 3 || left <= 0.0f || out != 0) {  // not painting, judged already, or a press after ours
            g_duel_other.fetch_add(1, std::memory_order_relaxed);
            log::info("[deadeye] the duel: the right trigger with %d marks, nothing done (%s: state %d, %.2f s left, outcome %d)", n,
                      !read ? "the duel not read" : state != 3 ? "not painting" : left <= 0.0f ? "the time out" : "judged", state, left, out);
        } else if (score < static_cast<float>(need)) {
            // (the review) the marks' score under the duel's bar: ended now it loses (the opponent shoots John); the time runs
            // on for more marks (a disarm's win is not in the score: the time-out still judges it)
            g_duel_short.fetch_add(1, std::memory_order_relaxed);
            log::info("[deadeye] the duel: the right trigger with %d marks, score %.0f of %.0f needed: not enough yet, the time runs on (%.2f s left)",
                      n, score, static_cast<float>(need), left);
        } else {
            const float end = -0.001f;  // (the game's test: under 0, not 0)
            if (wraw(D + 0x10c, &end, 4)) {
                g_duel_fires.fetch_add(1, std::memory_order_relaxed);
                g_duel_ended_D = D;
                g_duel_ended_ms = now;
                log::info("[deadeye] the duel: the right trigger ends the duel's time (%.2f s left): %d marks, score %.0f of %d, judged now", left, n,
                          score, need);
            }
        }
    }
    return a2;
}
uintptr_t update_level(uintptr_t P, uintptr_t a2);
// FUN_140348980(player info, flag): rcx and edx only (r8, r9 and xmm1-3 written before any read)
uintptr_t hk_update(uintptr_t P, uintptr_t a2) {
    uint8_t keep = 0;
    bool held = false;
    const uintptr_t req = duel_pre(P, a2, &held, &keep);
    const uintptr_t r = update_level(P, req);
    uint8_t now = 0;
    if (held && raw(P + kTrigLast, &now, 1) && now == 0xff) wraw(P + kTrigLast, &keep, 1);  // (the review: only our 0xff put back)
    return r;
}
uintptr_t update_level(uintptr_t P, uintptr_t a2) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    {
        static uint32_t n = 0;
        if ((++n & 15) == 0 || !g_sepia.load(std::memory_order_relaxed) || g_saved) sepia_apply();  // (cheap: one name compare)
    }
    uint8_t ml = 0, tp = 0;
    if (!P || !raw(P + kMultilock, &ml, 1) || !raw(P + kPaint, &tp, 1)) return o_update(P, a2);
    const int unl = ml == 0 ? 1 : (tp & 2) ? 2 : 3;
    g_unlocked.store(unl, std::memory_order_relaxed);
    const int want = g_latched.load(std::memory_order_relaxed);
    if (want <= 0 || want >= unl) return o_update(P, a2);  // the game's (as unlocked), or above it: the unlocked level
    // lowered for this call: level 1 multilock off; level 2 (from 3) the automatic paint
    const uint8_t ml2 = want >= 2 ? 1 : 0, tp2 = want == 2 ? static_cast<uint8_t>(tp | 2) : tp;
    wraw(P + kMultilock, &ml2, 1);
    wraw(P + kPaint, &tp2, 1);
    g_lowered.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t r = o_update(P, a2);
    // the game's back: the multilock byte, and only bit 1 of the flags (bits 0 and 2 are the call's own)
    uint8_t tp_now = 0;
    wraw(P + kMultilock, &ml, 1);
    if (raw(P + kPaint, &tp_now, 1)) {
        tp_now = static_cast<uint8_t>((tp_now & ~2) | (tp & 2));
        wraw(P + kPaint, &tp_now, 1);
    }
    return r;
}

void frame() {  // the level latched while Dead Eye is off (a change applies at the next Dead Eye)
    RdrvrActorState st{};
    const bool de = api::actor_state(&st) && (st.weapon_flags & RDRVR_WEAPON_DEADEYE);
    if (!de) g_latched.store(g_level.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// ---- run 28 item 1c: the Dead Eye X in the headset and the marks by the barrel (after optimumbox's fork, v0.8.11.9,
// commit ac942427, MIT: his deadeye.cpp's mechanism, ported with our safety rules; research\run28\deadeye.md)
template <class T>
bool rd(uintptr_t a, T* out) {
    return a && raw(a, out, sizeof(T));
}
template <class T>
bool wr(uintptr_t a, T v) {
    return a && wraw(a, &v, sizeof(T));
}

constexpr double kFreshMs = 200.0;   // marks drawn only while updated this recently (marks(), showing())
constexpr double kWindowMs = 500.0;  // the swap and the barrel paint run on this long after Dead Eye's mode was last on
constexpr int kAnchorRun = 30;       // calls with the plugin saying its camera is current before its pointer is taken
constexpr double kStateFreshMs = 250.0;  // the plugin's actor state trusted only while its tick advanced this recently

std::atomic<bool> g_marks_on{false};   // [Hands] DeadEyeMarks (its hooks installed only when on at the start)
std::atomic<bool> g_barrel{true};      // [Hands] DeadEyeBarrelPaint (its hook installed only when on at the start)
std::atomic<float> g_deg{3.0f};        // [Hands] DeadEyeMarkDegrees: an X's crop across in the eyes
std::atomic<int> g_pitch_set{0};       // the slots' spacing (UI pixels; 0: 200 at a 1080-line UI, scaled), "deadeye pitch"
std::atomic<int> g_off[2] = {0, 0};    // the X's centre from the element's point (UI pixels), "deadeye shift"
std::atomic<bool> g_marks_hooked{false}, g_paint_hooked{false};
std::atomic<uint64_t> g_hud_calls{0}, g_hud_swaps{0}, g_hud_kept{0}, g_hud_not_ours{0}, g_ui_runs{0}, g_ui_own{0}, g_ui_busy{0}, g_placed{0},
    g_paints{0}, g_paints_cleared{0}, g_layers{0}, g_hide_passes{0};
std::atomic<double> g_de_ms{0.0};      // log::now_ms when Dead Eye's mode was last seen on (only that renews the window)
// the anchor's camera object, learnt on the game thread: channel 0's camera after kAnchorRun calls in a row with the
// plugin saying its scripted camera is current (RDRVR_ACTOR_CAM_AWAY clear) and the same pointer there
uintptr_t g_anchor_cand = 0, g_anchor_cam = 0;
int g_anchor_run = 0;
uint64_t g_st_tick = 0;  // the plugin's last actor-state tick seen, and when it advanced
double g_st_ms = 0.0;
// the swapped call's purpose (game thread: set by hk_reticle_hud around its call, read by hk_marks_ui inside it):
// 0 none, 1 show the X (in Dead Eye's window with the X on), 2 hide them (a hole left pending outside that)
int g_swap_mode = 0;
std::atomic<int> g_hole_live{0};  // g_hole_n's copy for the cheap tests (g_hole_n itself under g_mutex)
std::atomic<uintptr_t> g_anchor_cam_seen{0};  // (the status)

std::mutex g_mutex;  // the marks below (the game thread writes, the presenting thread and the test channel read)
// the slots' grid in the UI target: per_row slots of p pixels across the middle, row 0 centred on the screen's height
// and further rows above it (the whole grid inside the target); x0 the grid's left, yb row 0's bottom
struct Grid {
    int p = 0, per_row = 1, x0 = 0, yb = 0;
};
Mark g_marks[kMax];
int g_n = 0;
double g_ms = 0;  // log::now_ms of the last marks' update (the drawing's freshness only)
// the hole: the slots the last redirected update placed elements in (with its grid and shift), kept however old until
// the game places or hides them itself (an update of its own, or a redirected one with fewer marks: the game hides the
// elements past the marks, 0x1403948a0), so a stale X left there shown never shows on the HUD
int g_hole_n = 0;
Grid g_hole_grid;
int g_hole_off[2] = {0, 0};
struct Drawn {
    float px[2], py[2];  // where the X was drawn, per eye (that eye's half of the layer, pixels; -1 not drawn)
    float u[2], v[2];    // the same as fractions of the eye's view (0..1, left to right, top to bottom)
};
Drawn g_drawn[kMax];
uint32_t g_layer_w = 0, g_layer_h = 0;  // one eye's half of the X layer

int pitch_px(uint32_t h) {
    const int p = g_pitch_set.load(std::memory_order_relaxed);
    if (p > 0) return p;
    const int a = static_cast<int>(std::lround(200.0 * static_cast<double>(h) / 1080.0));
    return a < 64 ? 64 : a > 400 ? 400 : a;
}
Grid grid() {
    uint32_t w = 0, h = 0;
    ui_layer::size(&w, &h);
    Grid g;
    g.p = pitch_px(h);
    g.per_row = w > static_cast<uint32_t>(g.p) ? static_cast<int>(w) / g.p : 1;
    if (g.per_row > kMax) g.per_row = kMax;
    g.x0 = (static_cast<int>(w) - g.per_row * g.p) / 2;
    if (g.x0 < 0) g.x0 = 0;
    const int rows = (kMax + g.per_row - 1) / g.per_row;
    g.yb = static_cast<int>(h) / 2 + g.p / 2;
    if (g.yb < rows * g.p) g.yb = rows * g.p < static_cast<int>(h) ? rows * g.p : static_cast<int>(h);
    return g;
}
void slot_of(const Grid& g, int k, float* out) {
    out[0] = static_cast<float>(g.x0 + g.p * (k % g.per_row)) + 0.5f * static_cast<float>(g.p);
    out[1] = static_cast<float>(g.yb - g.p * (k / g.per_row)) - 0.5f * static_cast<float>(g.p);
}

// Dead Eye's mode (player info +0x410 -> R +0x1c: 1 slow motion, 2 marks) on now, or on within kWindowMs. Only the
// mode renews the window (not marks still listed or shown)
bool dead_eye_window() {
    uintptr_t info = 0, R = 0;
    int32_t mode = 0;
    const double now = log::now_ms();
    if (rd(anchors::addr(anchors::Id::PlayerInfo), &info) && info && rd(info + 0x410, &R) && R && rd(R + 0x1c, &mode) && mode != 0) {
        g_de_ms.store(now, std::memory_order_relaxed);
        return true;
    }
    const double t = g_de_ms.load(std::memory_order_relaxed);
    return t > 0.0 && now - t < kWindowMs;
}

// Channel 0's camera `cur` is the anchor's own (not a script's: a duel's, a mission's, a cutscene's, a shop's). The
// plugin's flag (IS_CAMERA_ACTIVE_ON_CHANNEL of its camera, once a script tick) and the pointer learnt from it must
// both agree; a pointer change while the flag lags a tick is refused until it has held kAnchorRun calls.
bool anchor_camera_current(uintptr_t cur) {
    RdrvrActorState st{};
    const bool have = api::actor_state(&st);
    const double now = log::now_ms();
    if (have && st.tick != g_st_tick) {
        g_st_tick = st.tick;
        g_st_ms = now;
    }
    const bool fresh = have && g_st_ms > 0.0 && now - g_st_ms < kStateFreshMs;  // (a stalled script: its flag not trusted)
    if (!fresh || !st.camera || (st.flags & RDRVR_ACTOR_CAM_AWAY) || !cur) {
        g_anchor_run = 0;
        return false;
    }
    if (cur == g_anchor_cand) {
        if (g_anchor_run < kAnchorRun) ++g_anchor_run;
    } else {
        g_anchor_cand = cur;
        g_anchor_run = 1;
    }
    if (g_anchor_run >= kAnchorRun && g_anchor_cam != cur) {
        g_anchor_cam = cur;
        g_anchor_cam_seen.store(cur, std::memory_order_relaxed);
    }
    return g_anchor_cam && cur == g_anchor_cam;
}

// FUN_14034bc00 (ReticleHudUpdate), each tick: the marks' UI (DeadeyeMarksUi) runs only while channel 0's camera is the
// gameplay camera (0x14034bd09); in first person it is the anchor's (pose.cpp), so no X was ever placed. In Dead Eye's
// window, with the anchor's own camera on channel 0, the gameplay camera is put there for this one call, as hk_fire does
// for the trigger (aim.cpp), and the anchor's put back after it (also if the call unwinds) only if channel 0 is still
// the same channel holding what was written (a camera change the game made inside the call is kept). The game's
// reticle also runs under it then; the scripts hide it ([Hands] HideReticle, on by default) [I].
using ReticleHud_t = void (*)(uintptr_t info, uintptr_t ped, uintptr_t T, uintptr_t a4);
ReticleHud_t o_reticle_hud = nullptr;
struct Swap {
    uintptr_t cm, ch, cur, gp;
};
void restore_channel(const Swap& s) {
    g_swap_mode = 0;
    uintptr_t chs2 = 0, ch2 = 0, now = 0;
    if (rd(s.cm + 8, &chs2) && rd(chs2 + 0x18, &ch2) && ch2 == s.ch && rd(s.ch + 0x10, &now) && now == s.gp && wr<uintptr_t>(s.ch + 0x10, s.cur))
        g_hud_swaps.fetch_add(1, std::memory_order_relaxed);
    else
        g_hud_kept.fetch_add(1, std::memory_order_relaxed);  // the game changed channel 0 inside the call: its change kept
    aim::camera_swap_flag(false);
}
void call_swapped(const Swap& s, uintptr_t info, uintptr_t ped, uintptr_t T, uintptr_t a4) {  // (no C++ objects: __try/__finally)
    __try {
        o_reticle_hud(info, ped, T, a4);
    } __finally {
        restore_channel(s);
    }
}
void hk_reticle_hud(uintptr_t info, uintptr_t ped, uintptr_t T, uintptr_t a4) {
    g_hud_calls.fetch_add(1, std::memory_order_relaxed);
    Swap s{};
    const bool on = g_marks_on.load(std::memory_order_relaxed), pending = g_hole_live.load(std::memory_order_relaxed) > 0;
    if (!(on || pending) || !pose::anchor_active() || !rd(anchors::addr(anchors::Id::CamManager), &s.cm) || !s.cm) {
        o_reticle_hud(info, ped, T, a4);
        return;
    }
    uintptr_t chs = 0;
    if (!rd(s.cm + 8, &chs) || !chs || !rd(chs + 0x18, &s.ch) || !s.ch || !rd(s.ch + 0x10, &s.cur) || !rd(s.cm + 0x28, &s.gp) || !s.gp ||
        s.cur == s.gp) {
        o_reticle_hud(info, ped, T, a4);
        return;
    }
    const bool ours = anchor_camera_current(s.cur);  // (learnt every call, Dead Eye or not)
    // show: the X on, in Dead Eye's window; else hide: X left in their slots (the X turned off, the window over with
    // marks still listed) run once more with every tested element hidden, so the X and the hole go together
    const int mode = on && dead_eye_window() ? 1 : pending ? 2 : 0;
    if (!mode) {
        o_reticle_hud(info, ped, T, a4);
        return;
    }
    if (!ours) {  // a script's camera holds channel 0: left to it
        g_hud_not_ours.fetch_add(1, std::memory_order_relaxed);
        o_reticle_hud(info, ped, T, a4);
        return;
    }
    aim::camera_swap_flag(true);
    if (!wr<uintptr_t>(s.ch + 0x10, s.gp)) {
        aim::camera_swap_flag(false);
        o_reticle_hud(info, ped, T, a4);
        return;
    }
    g_swap_mode = mode;  // (cleared by restore_channel, also on an unwind)
    if (mode == 2) g_hide_passes.fetch_add(1, std::memory_order_relaxed);
    call_swapped(s, info, ped, T, a4);
}

}  // namespace

// deadeye_thunks.asm's gate and the originals (written before each hook is enabled: hooks::install sets the original
// first). rdrvr_de_tid: the thread inside the marks' UI with the X redirected (0: none)
extern "C" {
volatile long rdrvr_de_tid = 0;
uintptr_t rdrvr_de_vis_ret = 0, rdrvr_de_vis_orig = 0;
volatile uint64_t rdrvr_de_vis_hits = 0;
volatile uint8_t rdrvr_de_vis_answer = 1;  // what the let-in leaf call answers: 1 shown, 0 hidden (a hide pass)
uintptr_t rdrvr_de_pos_ret[2] = {0, 0}, rdrvr_de_pos_orig = 0;
uintptr_t rdrvr_de_ui_ret[2] = {0, 0}, rdrvr_de_ui_orig = 0;
void rdrvr_de_vis_thunk();
void rdrvr_de_pos_thunk();
void rdrvr_de_ui_thunk();
}

namespace {
// Only the thread in rdrvr_de_tid reaches the two parts below (the thunks' gate), between hk_marks_ui's start and end
int g_k = 0;               // the mark being placed: one MarkWorldPos, then one ScreenToUi, per mark (both paths)
float g_pos[kMax][3] = {};
Grid g_grid;               // this update's slots
int g_grid_off[2] = {0, 0};  // and the shift it placed them with

using WorldPos_t = void (*)(uintptr_t mark, float* out);
using ToUi_t = float* (*)(float* out, const float* in);
}  // namespace

// MarkWorldPos from DeadeyeMarksUi (the two sites): the mark's world point recorded for the k-th X
extern "C" void rdrvr_de_pos_impl(uintptr_t mark, float* out) {
    reinterpret_cast<WorldPos_t>(rdrvr_de_pos_orig)(mark, out);
    const int k = g_k < kMax ? g_k : kMax - 1;
    if (!raw(reinterpret_cast<uintptr_t>(out), g_pos[k], sizeof(g_pos[k]))) g_pos[k][0] = g_pos[k][1] = g_pos[k][2] = 0.0f;
}
// ScreenToUi from the marks (the two sites): the k-th X placed at slot k (the game still draws and animates it there)
extern "C" float* rdrvr_de_ui_impl(float* out, const float* in) {
    (void)in;
    float s[2];
    slot_of(g_grid, g_k < kMax ? g_k : kMax - 1, s);
    s[0] += static_cast<float>(g_grid_off[0]);
    s[1] += static_cast<float>(g_grid_off[1]);
    ++g_k;
    g_placed.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<ToUi_t>(rdrvr_de_ui_orig)(out, s);
}

namespace {
// FUN_140394300 (DeadeyeMarksUi): with the X wanted, the thunks let this thread in for the call
using MarksUi_t = void (*)(uintptr_t T, uintptr_t vp);
MarksUi_t o_marks_ui = nullptr;
void run_marks_ui(uintptr_t T, uintptr_t vp) {  // (no C++ objects: __try/__finally)
    __try {
        o_marks_ui(T, vp);
    } __finally {
        InterlockedExchange(&rdrvr_de_tid, 0);
    }
}
void publish(int n) {
    std::lock_guard lock(g_mutex);
    for (int k = 0; k < n; ++k) {
        std::memcpy(g_marks[k].world, g_pos[k], sizeof(g_marks[k].world));
        slot_of(g_grid, k, g_marks[k].slot);
        g_marks[k].slot[0] += static_cast<float>(g_grid_off[0]);
        g_marks[k].slot[1] += static_cast<float>(g_grid_off[1]);
    }
    g_n = n;
    g_ms = log::now_ms();
    g_hole_n = n;  // the elements past n hidden by the game in this same update
    g_hole_grid = g_grid;
    g_hole_off[0] = g_grid_off[0];
    g_hole_off[1] = g_grid_off[1];
    g_hole_live.store(n, std::memory_order_relaxed);
}
// a hide pass's end: no X drawn; the hole clears when every element placed was also tested (and so hidden): the duel's
// path shows its elements untested (FUN_140d07680), so they keep the hole until the game's list empties
void publish_hidden(int placed, uint64_t tested) {
    std::lock_guard lock(g_mutex);
    g_n = 0;
    g_ms = log::now_ms();
    if (tested >= static_cast<uint64_t>(placed)) g_hole_n = 0;
    g_hole_live.store(g_hole_n, std::memory_order_relaxed);
}
void game_placed() {  // an update of the game's own: every element where the game puts it (none left in a slot)
    g_ui_own.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    g_hole_n = 0;
    g_hole_live.store(0, std::memory_order_relaxed);
}
void hk_marks_ui(uintptr_t T, uintptr_t vp) {
    const long me = static_cast<long>(GetCurrentThreadId());
    // inside our swapped call its mode; a call of the game's own (the duel's, in first person) shows with the X on and
    // hides a pending hole with it off
    const bool on = g_marks_on.load(std::memory_order_relaxed);
    const int mode = g_swap_mode ? g_swap_mode : on ? 1 : g_hole_live.load(std::memory_order_relaxed) > 0 ? 2 : 0;
    if (!mode || !g_marks_hooked.load(std::memory_order_relaxed) || !pose::anchor_active()) {
        o_marks_ui(T, vp);
        game_placed();
        return;
    }
    if (InterlockedCompareExchange(&rdrvr_de_tid, me, 0) != 0) {  // another thread inside (never seen): the game's own
        g_ui_busy.fetch_add(1, std::memory_order_relaxed);
        o_marks_ui(T, vp);
        game_placed();
        return;
    }
    g_k = 0;
    g_grid = grid();
    g_grid_off[0] = g_off[0].load(std::memory_order_relaxed);
    g_grid_off[1] = g_off[1].load(std::memory_order_relaxed);
    rdrvr_de_vis_answer = mode == 1 ? 1 : 0;
    const uint64_t vis0 = rdrvr_de_vis_hits;
    run_marks_ui(T, vp);
    rdrvr_de_vis_answer = 1;
    g_ui_runs.fetch_add(1, std::memory_order_relaxed);
    if (mode == 1)
        publish(g_k < kMax ? g_k : kMax);
    else
        publish_hidden(g_k, rdrvr_de_vis_hits - vis0);
}

// Painting a mark: the trigger asks with T +0x6084, and the next TargetingUpdate (FUN_14039bec0) adds the mark
// (FUN_14039a3f0 at 0x14039cae7). With +0x6085 set (the paint button's request: research\gc-weapons.md, "may snap to
// the soft-locked target") the update adds it only once its own assisted pick from the flat camera's view is found
// (r12b, 0x14039caaf), so in first person a mark often never came where the barrel pointed. Cleared, the update marks
// the probe's hit: the actor on the barrel's ray (aim.cpp's AimRay override). A byte write on the game thread, before
// the update reads it, for the player's T only, in Dead Eye's window, in first person.
using Targeting_t = void (*)(uintptr_t T, uintptr_t a2, uintptr_t a3, uintptr_t a4);
Targeting_t o_targeting = nullptr;
uintptr_t player_T() {  // [ActorPool + (player info +0x5ec) * 0x10] +0xb8
    uintptr_t info = 0, pool = 0, actor = 0, T = 0;
    uint16_t idx = 0;
    return rd(anchors::addr(anchors::Id::PlayerInfo), &info) && info && rd(info + 0x5ec, &idx) && rd(anchors::addr(anchors::Id::ActorPool), &pool) &&
                   pool && rd(pool + static_cast<uintptr_t>(idx) * 0x10, &actor) && actor && rd(actor + 0xb8, &T)
               ? T
               : 0;
}
void hk_targeting(uintptr_t T, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    uint8_t req = 0, assisted = 0;
    if (T && g_barrel.load(std::memory_order_relaxed) && rd(T + 0x6084, &req) && req && rd(T + 0x6085, &assisted) && assisted && T == player_T() &&
        pose::anchor_active() && dead_eye_window()) {
        g_paints.fetch_add(1, std::memory_order_relaxed);
        if (wr<uint8_t>(T + 0x6085, 0)) g_paints_cleared.fetch_add(1, std::memory_order_relaxed);
    }
    o_targeting(T, a2, a3, a4);
}

const anchors::Id kMarkIds[] = {anchors::Id::ReticleHudUpdate, anchors::Id::DeadeyeMarksUi, anchors::Id::MarkWorldPos,
                                anchors::Id::ViewportPointVisible, anchors::Id::ScreenToUi, anchors::Id::DeadeyeVisRet,
                                anchors::Id::DeadeyePosRet, anchors::Id::DeadeyePosRet2, anchors::Id::DeadeyeUiRet,
                                anchors::Id::DeadeyeUiRet2, anchors::Id::CamManager, anchors::Id::PlayerInfo};
const char* marks_missing() {  // the first anchor the X needs that this build lacks, nullptr none
    for (anchors::Id id : kMarkIds)
        if (!anchors::present(id)) return anchors::name(id);
    return nullptr;
}
bool paint_supported() {
    return anchors::present(anchors::Id::TargetingUpdate) && anchors::present(anchors::Id::PlayerInfo) && anchors::present(anchors::Id::ActorPool);
}

void install_marks() {
    using anchors::Id;
    if (const char* miss = marks_missing()) {
        log::warn("[deadeye] the X in the headset: not installed, this build of RDR.exe has no %s", miss);
        return;
    }
    // the gate's call sites first (the thunks compare against them as soon as their hooks are on)
    rdrvr_de_vis_ret = anchors::addr(Id::DeadeyeVisRet);
    rdrvr_de_pos_ret[0] = anchors::addr(Id::DeadeyePosRet);
    rdrvr_de_pos_ret[1] = anchors::addr(Id::DeadeyePosRet2);
    rdrvr_de_ui_ret[0] = anchors::addr(Id::DeadeyeUiRet);
    rdrvr_de_ui_ret[1] = anchors::addr(Id::DeadeyeUiRet2);
    // the thunks before the hooks that let a thread in (each inert until rdrvr_de_tid names a thread)
    bool ok = hooks::install("RDR viewport point test (deadeye thunk)", reinterpret_cast<void*>(anchors::addr(Id::ViewportPointVisible)),
                             reinterpret_cast<void*>(&rdrvr_de_vis_thunk), reinterpret_cast<void**>(&rdrvr_de_vis_orig));
    ok = ok && hooks::install("RDR mark world point (deadeye thunk)", reinterpret_cast<void*>(anchors::addr(Id::MarkWorldPos)),
                              reinterpret_cast<void*>(&rdrvr_de_pos_thunk), reinterpret_cast<void**>(&rdrvr_de_pos_orig));
    ok = ok && hooks::install("RDR screen to UI (deadeye thunk)", reinterpret_cast<void*>(anchors::addr(Id::ScreenToUi)),
                              reinterpret_cast<void*>(&rdrvr_de_ui_thunk), reinterpret_cast<void**>(&rdrvr_de_ui_orig));
    ok = ok && hooks::install("RDR Dead Eye marks' UI (deadeye)", reinterpret_cast<void*>(anchors::addr(Id::DeadeyeMarksUi)), hk_marks_ui, &o_marks_ui);
    ok = ok && hooks::install("RDR reticle UI update (deadeye)", reinterpret_cast<void*>(anchors::addr(Id::ReticleHudUpdate)), hk_reticle_hud,
                              &o_reticle_hud);
    // A failure part way leaves the installed ones inert: the thunks let in only the thread hk_marks_ui names, and
    // hk_marks_ui only redirects the game's own calls of the marks' UI (none in first person without the reticle
    // hook's swap: the duel's alone, drawn then by the layer).
    g_marks_hooked = ok;
    if (!ok) log::error("[deadeye] the X in the headset: a hook failed, off");
}

}  // namespace

void init() {
    const int l = config::get_int("DeadEye", "Level", 0);
    g_level = l < 0 || l > 3 ? 0 : l;
    g_latched = g_level.load();
    g_sepia = config::get_bool("DeadEye", "Sepia", true);
    g_duel_rt = config::get_bool("DeadEye", "DuelTriggerFires", false);
    log::info("[deadeye] level %d (0: the game's), sepia %d, the duel's right trigger fires %d", g_level.load(), g_sepia.load() ? 1 : 0,
              g_duel_rt.load() ? 1 : 0);
    // his key names, so his users' inis carry over
    g_marks_on = config::get_bool("Hands", "DeadEyeMarks", false);
    g_barrel = config::get_bool("Hands", "DeadEyeBarrelPaint", true);
    const float d = config::get_float("Hands", "DeadEyeMarkDegrees", 3.0f);
    g_deg = !(d >= 0.5f) ? 0.5f : d > 15.0f ? 15.0f : d;
    log::info("[deadeye] the X in the headset %d (%.1f degrees), the marks by the barrel %d", g_marks_on.load() ? 1 : 0, g_deg.load(),
              g_barrel.load() ? 1 : 0);
    d3d::add_frame_end_listener([](uint64_t) { frame(); });
}

bool install() {
    using anchors::Id;
    // Off at the start: nothing installed (bootstrap's "off: nothing"); turned on in the menu, at the next start. On, a
    // later off in the menu or the test channel is the hooks' cheap off path.
    if (!g_barrel.load()) {
        log::info("[deadeye] the marks by the barrel: off ([Hands] DeadEyeBarrelPaint=0): not installed");
    } else if (paint_supported()) {
        g_paint_hooked = hooks::install("RDR targeting update (deadeye barrel paint)", reinterpret_cast<void*>(anchors::addr(Id::TargetingUpdate)),
                                        hk_targeting, &o_targeting);
    } else {
        log::warn("[deadeye] the marks by the barrel: not installed, this build of RDR.exe has no TargetingUpdate");
    }
    if (g_marks_on.load())
        install_marks();
    else
        log::info("[deadeye] the X in the headset: off ([Hands] DeadEyeMarks=0): not installed");
    log::info("[deadeye] the marks by the barrel %s; the X in the headset %s", g_paint_hooked.load() ? "installed" : "not installed",
              g_marks_hooked.load() ? "installed" : "not installed");
    if (!anchors::present(Id::DeadEyeUpdate)) {
        log::warn("[deadeye] the level choice: not installed, this build of RDR.exe has no DeadEyeUpdate");
        return false;
    }
    return hooks::install("RDR Dead Eye state (the level)", reinterpret_cast<void*>(anchors::addr(Id::DeadEyeUpdate)), hk_update, &o_update);
}

int level() { return g_level.load(std::memory_order_relaxed); }
void set_level(int l) {
    if (l < 0 || l > 3) return;
    if (g_level.exchange(l) != l) log::info("[deadeye] level %d (0: the game's; the unlocked %d)", l, g_unlocked.load());
    config::set("DeadEye", "Level", std::to_string(l).c_str());
}
int unlocked() { return g_unlocked.load(std::memory_order_relaxed); }
bool sepia() { return g_sepia.load(std::memory_order_relaxed); }
bool duel_trigger_fires() { return g_duel_rt.load(std::memory_order_relaxed); }
void set_duel_trigger_fires(bool on, bool save) {
    if (g_duel_rt.exchange(on) != on) log::info("[deadeye] the duel's right trigger fires the marks: %s", on ? "on" : "off");
    if (save) config::set("DeadEye", "DuelTriggerFires", on ? "1" : "0");
}
void set_sepia(bool on) {
    if (g_sepia.exchange(on) != on) log::info("[deadeye] the sepia filter %s", on ? "on" : "off");
    config::set("DeadEye", "Sepia", on ? "1" : "0");
}

bool marks_on() { return g_marks_on.load(std::memory_order_relaxed); }
bool marks_available() { return g_marks_hooked.load(std::memory_order_relaxed); }
bool marks_supported() { return marks_missing() == nullptr; }
void set_marks_on(bool on, bool save) {
    if (g_marks_on.exchange(on) != on)
        log::info("[deadeye] the X in the headset %s%s", on ? "on" : "off", g_marks_hooked.load() ? "" : " (its hooks not installed: from the next start)");
    if (save) config::set("Hands", "DeadEyeMarks", on ? "1" : "0");
}
float mark_degrees() { return g_deg.load(std::memory_order_relaxed); }
void set_mark_degrees(float d, bool save) {
    if (!(d >= 0.5f && d <= 15.0f)) return;
    g_deg = d;
    if (save) {
        char b[16];
        std::snprintf(b, sizeof(b), "%.1f", d);
        config::set("Hands", "DeadEyeMarkDegrees", b);
    }
}
bool barrel_paint() { return g_barrel.load(std::memory_order_relaxed); }
bool barrel_paint_available() { return g_paint_hooked.load(std::memory_order_relaxed); }
bool barrel_paint_supported() { return paint_supported(); }
void set_barrel_paint(bool on, bool save) {
    if (g_barrel.exchange(on) != on)
        log::info("[deadeye] the marks by the barrel %s%s", on ? "on" : "off (the game's own pick)",
                  g_paint_hooked.load() ? "" : " (its hook not installed: from the next start)");
    if (save) config::set("Hands", "DeadEyeBarrelPaint", on ? "1" : "0");
}

bool showing() {
    if (!g_marks_on.load(std::memory_order_relaxed) || !g_marks_hooked.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_mutex);
    return g_n > 0 && log::now_ms() - g_ms <= kFreshMs;
}

int marks(Mark* out, int max, float* crop_px, float* deg) {
    *deg = g_deg.load(std::memory_order_relaxed);
    if (!g_marks_on.load(std::memory_order_relaxed) || !g_marks_hooked.load(std::memory_order_relaxed)) return 0;
    std::lock_guard lock(g_mutex);
    if (log::now_ms() - g_ms > kFreshMs) return 0;
    *crop_px = static_cast<float>(g_hole_grid.p);  // the published update's grid (the slots' pitch)
    const int n = g_n < max ? g_n : max;
    std::memcpy(out, g_marks, sizeof(Mark) * static_cast<size_t>(n));
    return n;
}

bool band(int out[4]) {
    if (!g_marks_hooked.load(std::memory_order_relaxed)) return false;  // (on or off: a stale X there is kept out too)
    std::lock_guard lock(g_mutex);
    const int n = g_hole_n;
    const Grid& g = g_hole_grid;
    if (n <= 0 || g.p <= 0) return false;
    // only the slots in use, with the grid and the shift the elements were placed with: row 0 from the grid's left,
    // further rows above (the HUD keeps the rest)
    const int cols = n < g.per_row ? n : g.per_row, rows = (n + g.per_row - 1) / g.per_row;
    out[0] = g.x0 + g_hole_off[0];
    out[1] = g.yb - rows * g.p + g_hole_off[1];
    out[2] = g.x0 + cols * g.p + g_hole_off[0];
    out[3] = g.yb + g_hole_off[1];
    return true;
}

void note_layer() { g_layers.fetch_add(1, std::memory_order_relaxed); }
void note_drawn(int k, int eye, float px, float py, float u, float v, uint32_t layer_w, uint32_t layer_h) {
    if (k < 0 || k >= kMax || eye < 0 || eye > 1) return;
    std::lock_guard lock(g_mutex);
    g_drawn[k].px[eye] = px;
    g_drawn[k].py[eye] = py;
    g_drawn[k].u[eye] = u;
    g_drawn[k].v[eye] = v;
    g_layer_w = layer_w;
    g_layer_h = layer_h;
}

std::string command(const std::string& line) {
    // deadeye [level 0-3] [sepia on|off] [marks on|off] [barrel on|off] [deg <d>] [pitch <px>] [shift <x> <y>]: the
    // session only (the ini untouched), then the state
    std::istringstream in(line);
    std::string c, w, v;
    in >> c;
    while (in >> w) {
        if (w == "level" && (in >> v)) {
            const int l = std::atoi(v.c_str());
            if (l >= 0 && l <= 3) g_level = l;
        } else if (w == "sepia" && (in >> v)) {
            g_sepia = v == "on";
        } else if (w == "duelrt" && (in >> v)) {  // (item 6) [DeadEye] DuelTriggerFires
            set_duel_trigger_fires(v == "on", false);
        } else if (w == "marks" && (in >> v)) {
            set_marks_on(v == "on", false);
        } else if (w == "barrel" && (in >> v)) {
            set_barrel_paint(v == "on", false);
        } else if (w == "deg" && (in >> v)) {
            set_mark_degrees(std::strtof(v.c_str(), nullptr), false);
        } else if (w == "pitch" && (in >> v)) {
            const int p = std::atoi(v.c_str());
            if (p == 0 || (p >= 32 && p <= 600)) g_pitch_set = p;
        } else if (w == "shift") {
            int x = 0, y = 0;
            if (in >> x >> y) {
                g_off[0] = x;
                g_off[1] = y;
            }
        }
    }
    const uintptr_t pr = deadeye_preset(postfx());
    float col[4] = {}, sat = 0.0f;
    const bool have = pr && raw(pr + kColour, col, sizeof(col)) && raw(pr + kSat, &sat, sizeof(sat));
    int bd[4] = {0, 0, 0, 0};
    const bool has_band = band(bd);
    uint32_t uw = 0, uh = 0;
    ui_layer::size(&uw, &uh);
    // read only (this thread; SEH): the reticle probe's hit physinst (T +0x5790, the hit result's +0 as the probe
    // stores it at 0x14039c4ea) and its level index (+0x10), and the marks' list (T +0x6050; node +0 the mark, +8
    // next): each mark's physinst index (+0x38, which the add copies from the hit's +0x10 at 0x14039a92a), its
    // generation (+0x3a), bone (+0x3c) and flags (+0x3e); at most 32 nodes
    const uintptr_t T = player_T();
    uintptr_t hit = 0, node = 0;
    uint16_t hit_idx = 0xffff;
    int32_t target = 0;
    const bool have_hit = T && rd(T + 0x5790, &hit) && hit && rd(hit + 0x10, &hit_idx);
    const bool have_target = T && rd(T + 0x3900, &target);  // the probe's target actor's handle (aim.cpp reads it so)
    // the paint request (T +0x6084 paint, +0x6085 assisted, +0x6086 no rate limit, +0x6087 same bone), the gun
    // controller's state (G +0x3bc: 8 aiming, the painting phase needs 8 or 12, 0x140349861) and its arm-block reason
    // (G +0x410), Dead Eye's phase (R +0x20: 0 off, 2 painting, 3 firing)
    uint8_t req[4] = {0, 0, 0, 0}, block = 0;
    int32_t gun_state = -1, phase = -1;
    const bool have_req = T && raw(T + 0x6084, req, sizeof(req));
    const uintptr_t G = aim::player_gun_ctl();
    if (G) {
        rd(G + 0x3bc, &gun_state);
        rd(G + 0x410, &block);
    }
    {
        uintptr_t info = 0, R = 0;
        if (rd(anchors::addr(anchors::Id::PlayerInfo), &info) && info && rd(info + 0x410, &R) && R) rd(R + 0x20, &phase);
    }
    std::string list;
    int nl = 0;
    if (T && rd(T + 0x6050, &node))
        for (; node && nl < 32; ++nl) {
            uintptr_t mark = 0, next = 0;
            uint16_t idx = 0xffff, gen = 0;
            int16_t bone = -1;
            uint8_t fl = 0;
            if (!rd(node, &mark) || !rd(node + 8, &next)) break;
            if (mark) {
                rd(mark + 0x38, &idx);
                rd(mark + 0x3a, &gen);
                rd(mark + 0x3c, &bone);
                rd(mark + 0x3e, &fl);
            }
            char e[64];
            std::snprintf(e, sizeof(e), " %u/%u/%d/%#x", static_cast<unsigned>(idx), static_cast<unsigned>(gen), static_cast<int>(bone),
                          static_cast<unsigned>(fl));
            list += e;
            node = next;
        }
    std::string o;
    char b[900];
    std::snprintf(b, sizeof(b),
                  "deadeye level %d (latched %d, unlocked %d), sepia %d | calls %llu, lowered %llu | FX_Deadeye %s colour %.3f %.3f %.3f %.3f sat %.2f, "
                  "writes %llu, restores %llu | marks %s (%s) deg %.1f pitch %d shift (%d %d) | barrel %s (%s) paints %llu cleared %llu | "
                  "hud calls %llu swaps %llu kept %llu not ours %llu hide passes %llu (anchor camera %p) | ui runs %llu own %llu busy %llu placed %llu visible %llu | "
                  "probe physinst %s %d target %d | request %s %02x %02x %02x %02x | gun state %d block %u phase %d | list %d:",
                  g_level.load(), g_latched.load(), g_unlocked.load(), g_sepia.load() ? 1 : 0, static_cast<unsigned long long>(g_calls.load()),
                  static_cast<unsigned long long>(g_lowered.load()), have ? "found" : "not found", col[0], col[1], col[2], col[3], sat,
                  static_cast<unsigned long long>(g_sepia_writes.load()), static_cast<unsigned long long>(g_sepia_restores.load()),
                  g_marks_on.load() ? "on" : "off", g_marks_hooked.load() ? "installed" : marks_supported() ? "not installed" : "unavailable",
                  g_deg.load(), pitch_px(uh), g_off[0].load(), g_off[1].load(), g_barrel.load() ? "on" : "off",
                  g_paint_hooked.load() ? "installed" : paint_supported() ? "not installed" : "unavailable",
                  static_cast<unsigned long long>(g_paints.load()), static_cast<unsigned long long>(g_paints_cleared.load()),
                  static_cast<unsigned long long>(g_hud_calls.load()), static_cast<unsigned long long>(g_hud_swaps.load()),
                  static_cast<unsigned long long>(g_hud_kept.load()), static_cast<unsigned long long>(g_hud_not_ours.load()),
                  static_cast<unsigned long long>(g_hide_passes.load()), reinterpret_cast<void*>(g_anchor_cam_seen.load()), static_cast<unsigned long long>(g_ui_runs.load()),
                  static_cast<unsigned long long>(g_ui_own.load()), static_cast<unsigned long long>(g_ui_busy.load()),
                  static_cast<unsigned long long>(g_placed.load()), static_cast<unsigned long long>(rdrvr_de_vis_hits), have_hit ? "idx" : "none",
                  have_hit ? static_cast<int>(hit_idx) : -1, have_target ? static_cast<int>(target) : -1, have_req ? "read" : "unread", req[0], req[1], req[2],
                  req[3], static_cast<int>(gun_state), static_cast<unsigned>(block), static_cast<int>(phase), nl);
    o = b;
    o += list;
    o += " | ";
    std::lock_guard lock(g_mutex);
    std::snprintf(b, sizeof(b), "last %d marks %.0f ms ago | band %s %d %d %d %d (%d px tall of %u) | layer %ux%u frames %llu", g_n,
                  log::now_ms() - g_ms, has_band ? "on" : "off", bd[0], bd[1], bd[2], bd[3], has_band ? bd[3] - bd[1] : 0, uh, g_layer_w, g_layer_h,
                  static_cast<unsigned long long>(g_layers.load()));
    o += b;
    for (int k = 0; k < g_n && k < 8; ++k) {
        const Drawn& d = g_drawn[k];
        std::snprintf(b, sizeof(b), " | %d at (%.3f %.3f %.3f) slot (%.0f %.0f) eye0 (%.1f %.1f; %.4f %.4f) eye1 (%.1f %.1f; %.4f %.4f)", k,
                      g_marks[k].world[0], g_marks[k].world[1], g_marks[k].world[2], g_marks[k].slot[0], g_marks[k].slot[1], d.px[0], d.py[0],
                      d.u[0], d.v[0], d.px[1], d.py[1], d.u[1], d.v[1]);
        o += b;
    }
    std::snprintf(b, sizeof(b), " | the duel's right trigger fires %s: calls %llu, the time ended %llu, presses under the bar %llu, without a "
                  "mark %llu, otherwise refused %llu", g_duel_rt.load() ? "on" : "off", static_cast<unsigned long long>(g_duel_calls.load()),
                  static_cast<unsigned long long>(g_duel_fires.load()), static_cast<unsigned long long>(g_duel_short.load()),
                  static_cast<unsigned long long>(g_duel_nomark.load()), static_cast<unsigned long long>(g_duel_other.load()));
    o += b;
    return o;
}

}  // namespace rdrvr::deadeye
