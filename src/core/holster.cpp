#include "core/holster.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "core/actions.h"
#include "core/aim.h"
#include "core/xinput.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/dual.h"
#include "core/gestures.h"
#include "core/hands.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/horseshoe.h"
#include "core/offhand.h"
#include "core/pose.h"
#include "core/pso.h"
#include "core/reload.h"
#include "core/round_draw.h"
#include "core/vr_mode.h"
#include "core/held_prop.h"
#include "core/wheel.h"
#include "core/whistle.h"
#include "core/zone_rings.h"

namespace rdrvr::holster {
bool knife_swap();
void set_knife_swap(bool on);
namespace {

std::atomic<bool> g_keep{true};          // [Hands] KeepGunDrawn (on with the body holsters)
std::atomic<uintptr_t> g_player_ped{0};  // the player's ped (actor +0x38), refreshed every frame
std::atomic<bool> g_aim_pose{false};      // the gun controller's +0x5d6 bit 0x40, at the last frame end
std::atomic<bool> g_fire_ready{false};
std::atomic<float> g_fire_phase{-1.0f};  // the gun controller's fire clip (G +0x24): its phase, below 0 when none plays    // FUN_140d16400's fire test, at the last frame end
std::atomic<bool> g_raised{false}, g_gun_at_zone{false};
// run 8 item 2: the foregrip ring in the drawn gun's frame (R^T (ring - o)), each holster frame with a long gun
struct ForeTrace {
    int weapon = -1;
    uint64_t frames = 0, jumps = 0;  // jumps: over 5 mm from the frame before
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, last[3] = {}, max_step = 0.0f;
    bool have = false;
};
std::mutex g_fore_mutex;
ForeTrace g_fore_trace;
float g_log_ref[3] = {};
int g_log_weapon = -1;
double g_log_ms = -1e12;
void fore_trace(int weapon, const float* R, const float* o, const float* ring, double now) {
    float d[3], g[3];
    for (int k = 0; k < 3; ++k) d[k] = ring[k] - o[k];
    for (int i = 0; i < 3; ++i) g[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2];  // the columns: the gun's axes
    {
        std::lock_guard lock(g_fore_mutex);
        ForeTrace& t = g_fore_trace;
        if (weapon != t.weapon) t = ForeTrace{}, t.weapon = weapon;
        if (t.have) {
            const float s = std::sqrt((g[0] - t.last[0]) * (g[0] - t.last[0]) + (g[1] - t.last[1]) * (g[1] - t.last[1]) + (g[2] - t.last[2]) * (g[2] - t.last[2]));
            if (s > 0.005f) ++t.jumps;
            if (s > t.max_step) t.max_step = s;
        }
        for (int k = 0; k < 3; ++k) {
            t.lo[k] = std::fmin(t.lo[k], g[k]);
            t.hi[k] = std::fmax(t.hi[k], g[k]);
            t.last[k] = g[k];
        }
        t.have = true;
        ++t.frames;
    }
    // the log: a move over 1 cm in the gun's frame since the last line (once a second at most), the weapon kept
    if (weapon != g_log_weapon) {
        g_log_weapon = weapon;
        std::memcpy(g_log_ref, g, sizeof(g));
        return;
    }
    const float m = std::sqrt((g[0] - g_log_ref[0]) * (g[0] - g_log_ref[0]) + (g[1] - g_log_ref[1]) * (g[1] - g_log_ref[1]) +
                              (g[2] - g_log_ref[2]) * (g[2] - g_log_ref[2]));
    if (m > 0.01f && now - g_log_ms > 1000.0) {
        g_log_ms = now;
        log::info("[holster] the foregrip ring moved %.0f mm in the gun's frame (weapon %d): now (%.3f %.3f %.3f), the grip from %s", m * 1000.0f,
                  weapon, g[0], g[1], g[2], body::grip_source_name());
        std::memcpy(g_log_ref, g, sizeof(g));
    }
}
std::atomic<uint64_t> g_raises{0};
float g_raise_pmin = -35.0f, g_raise_pmax = 60.0f, g_raise_reach = 0.25f;  // [Hands] RaisedPitchMin/Max, RaisedReach
std::atomic<float> g_raise_dbg[4] = {};  // the last frame's pitch, reach, ahead, and the conditions (bits) for "holster"
std::atomic<bool> g_unarmed{true};        // [Holsters] UnarmedAfterHolster
// [Holsters] HoldToKeep (2026-10-30, off; HEADSET round 52, the user: "I'd like to change the controls so that to draw a
// weapon you have to hold grip, letting go puts it back in your holster unless you are holding it by the foregrip in
// which case it remains held by the foregrip"). Off, everything is as before. On (with Mode=holsters; the wheel's mode
// keeps its own grips, and the option rests there):
// 1. A gun drawn at a holster stays in the hand whose grip drew it only while that grip stays held (the draw itself is
//    the press's, as before: at once with InstantDraw). The gun only (reload::is_gun: the sidearms and the long guns):
//    the knife, the lasso, the throwables and the torch keep their own grips (ThrowByGrip's throw is a let-go).
// 2. That grip let go, anywhere, puts the gun away: the game's own holster (ACTOR_PUT_ITEM_AWAY, the put-away at a
//    holster's path, with UnarmedAfterHolster's fists), shown at the holster it came from (put_zone_of). A second gun out
//    becomes the gun in hand, as at a holster.
// 3. A long gun held two-handed (the front hand on its foregrip, two_hold) and the gun hand let go: the front hand
//    carries it ([Hands] CarryByForegrip's hand-over, at once: no hold time, the grip was held since the draw). While a
//    carry cannot start (Dead Eye, cover, the menu, the game's reload, an action not worked, not yet fully two-handed,
//    the left-handed transplant) the let-go waits and the gun stays in hand as long as the front hand holds it; the
//    front hand let go first: the holster (2). With CarryByForegrip off there is no carry: the holster (2), as a pistol.
// 4. The carrying hand let go puts the carried gun away (as before), after the same debounce and the same waits for the
//    game's own states (Dead Eye, a game menu, a scene, paused or faded, mounting).
// 5. The gun hand's grip at the carried gun's take-back point takes it back (as before), held by that grip from then:
//    let go, carried again if the front hand still holds the foregrip, else the holster.
// 6. DualWield's second gun and DualWieldCopy's copy: taken at a holster by the grip held, back to its holster (dual::end:
//    the game hangs it there again) when that grip is let go. The front hand on a long gun's foregrip holds no gun of its own.
// 7. The other grips are as they were: the chest's rounds, the magazines, OffHandItems, the parts' grips, the fore-end.
//    The hand that holds a gun by its grip is the hold's (g_wanted: never the game's LB/RB meanwhile).
// 8. A let-go must last kHoldLetGoMs (100 ms: a loose grip dipping under GripThreshold's hysteresis or a tracking blip
//    lasts a frame or three, a hand opened on purpose far longer, and 100 ms more before the holster is not felt); a
//    shorter one is a blip (counted, logged) and keeps the gun. The put-away (and the second gun's put-back, the hand-over)
//    waits: through Dead Eye, the game's reload, a game menu (the pause menu, the satchel, a shop's, the map:
//    controls::game_menu_open), a script's camera or a cutscene (RDRVR_ACTOR_CAM_AWAY, vr_mode's cutscene bit; wheel.cpp's
//    scripted()), the game paused or faded, then kHoldBusyGraceMs more (none of the game's own states is cut by a
//    put-away; a let-go there is not one made for it); a second after mounting or dismounting (the game's own clip); the
//    game's draw clip (InstantDraw off, or a game-drawn gun taken: kHoldDrawSettleMs after the gun is in the hand, as
//    GunMelee's draw window, so the put-away never lands mid-draw); a carried gun's take-back still under way; the hand at
//    the chest's ammo zone where its grip would take rounds (ChestSqueeze, DualWieldCrossReload: the grip opened and
//    closed there; leaving it open, the holster); a let-go that released rounds or a magazine (into a gun or dropped: the
//    rounds' let-go, not the gun's; the grip again within kHoldRoundsMs keeps the gun, else it is a let-go after all); the
//    other hand's second gun let go in the same moment (its debounce first: then both go back, the second gun not made the
//    gun in hand only to be put away). After a hold's put-away a press on the gun on its way out does not take it again
//    (kHoldPutMs). In Dead Eye the gun hand's grip is the game's again (RB: Dead Eye's paint): the gun stays (its let-go
//    waits), the grip passes (and a paint's press takes no loose gun). With DrawToGrabbingHand off (or the left-handed layout's transplant), a gun drawn by the
//    other hand lands in the gun hand: that hand's grip holds it if held, else it is loose (the gun hand's press takes
//    it). The menu
//    open (or placing by hand), the option turned off or the frame end without the body for over half a second (a
//    cutscene, a load, third person): the held guns become loose, as is a gun the game draws itself (LT, a script, a
//    cutscene's end, a mission's): it stays drawn until the grip of the hand holding it is pressed (taking it) and let go.
//    The game taking a held gun away (a cutscene, its own holster, death) ends the hold. Riding and cover keep the rules
//    (the game's put-away works mounted, as the holsters' at a holster did); a carry ends mounted, as before.
std::atomic<bool> g_hold{false};
// [Holsters] HoldItems (2026-10-31, the off-hand build step 2, R1; the user: "All guns, melee and throwables should need to
// be held with grip. Throwing should be as simple as letting go with making a throwing motion"): with HoldToKeep, the
// knife, the torch, the lasso and the throwables are held by the grip as the guns are (1.-2. above). Let go: put away;
// a throwable let go fast is thrown unlit (the off-hand copy's launch: offhand::throw_in_hand; the user's answer 5: "Throw
// unlit"); the knife and the torch never thrown (answer 11), their let-go waiting out a swing; the lasso's while its rope
// is held (LT or a catch). Armed by the trigger (ThrowByGrip) a throwable stays the game's own throw
std::atomic<bool> g_hold_items{true};
std::atomic<uint64_t> g_hold_item_throws{0}, g_hold_item_game_throws{0};
bool hold_item_kind(int w) { return w == 21 || w == 22 || w == 33 || offhand::kind_of(w) == offhand::kThrown; }
bool keepable(int w) { return reload::is_gun(w) || (g_hold_items.load(std::memory_order_relaxed) && hold_item_kind(w)); }
// (2026-10-31, the off-hand build step 3, P1; the user: "Two long guns should be impossible, sawn off is the exception")
// a long gun for the pair rule: the Sawed-off pairs as a sidearm does. Refused (r77 h6 found all three today): the
// second gun's take beside a long gun in hand, the gun hand's draw beside a long second gun, and beside a carried one
std::atomic<uint64_t> g_two_long_refused{0};
std::atomic<bool> g_carry_rounds{true};  // [Reload] CarriedRounds (step 3, R17)
std::atomic<bool> g_rounds_for_sec{false};  // (step 4, S3) a hand holds rounds for the second gun or the copy
// [Hands] TorchSwap (2026-10-31, step 5; answer 9): the off hand's torch with a gun in hand is the game's weapon, the gun
// the second gun (DualWield and DualWieldBesideTorch on), not the off-hand copy
std::atomic<bool> g_torch_swap{true};
std::atomic<uint64_t> g_torch_swaps{0};
// [Hands] KnifeSwap (2026-11-01, GOAL-offhand-changes C2; round 63, the user: the off-hand copy knife does not hit a human
// enemy; the knife as the torch): the off hand's knife with a gun in hand is the game's weapon, the gun the second gun
// (DualWield and DualWieldBesideItems on), as TorchSwap
std::atomic<bool> g_knife_swap{true};
bool pair_long(int w) { return reload::is_long_gun(w) && w != 15; }  // (15: the Sawed-off)
constexpr double kHoldLetGoMs = 100.0;
constexpr double kHoldBusyGraceMs = 150.0;   // after Dead Eye, a game menu, a scene: the let-go waits this much more
constexpr double kHoldSeatMs = 1000.0;       // after mounting or dismounting
constexpr double kHoldDrawSettleMs = 700.0;  // an animated draw's clip (gestures.cpp's kDrawMs, GunMelee's draw window)
constexpr double kHoldRoundsMs = 1500.0;     // a let-go that released rounds: the grip again within this keeps the gun
constexpr double kHoldPutMs = 700.0;         // after a hold's put-away: no press takes the gun on its way out
std::atomic<double> g_handover_letgo{40.0};  // [Holsters] HandOverLetGoMs (step 3, R10)
std::atomic<double> g_hold_letgo{kHoldLetGoMs};  // the session's ("holster hold letgo <ms>", a test's aid); kHoldLetGoMs otherwise
std::atomic<bool> g_keeping[2] = {false, false};  // grip_keeps(): as of the last frame end
std::atomic<int> g_keep_pub[2] = {0, 0}, g_keep_w_pub[2] = {-1, -1};  // "holster hold": each controller's kept gun (0 none, 1 gun, 2 second)
std::atomic<float> g_keep_up_pub[2] = {-1.0f, -1.0f};                 // ... and how long its grip has been let go (ms, -1 held)
std::atomic<uint64_t> g_hold_takes{0}, g_hold_adopts{0}, g_hold_puts{0}, g_hold_carries{0}, g_hold_sec_puts{0}, g_hold_blips{0},
    g_hold_waits{0}, g_hold_loose{0}, g_hold_gone{0};
std::atomic<float> g_hold_blip_ms{-1.0f}, g_hold_put_len{-1.0f};  // the last blip's length, the last put-away's let-go (ms)
std::atomic<int> g_hold_put_zone{-1};                             // the holster the last let-go's gun was shown back at (-1 none)
std::atomic<bool> g_show_zones{false};    // [Holsters] ShowZones: the holster rings (run 8 item 5b: those only)
std::atomic<bool> g_zones_near{false};    // [Holsters] ZonesNear (2026-10-09): a ring only with a hand near it
std::atomic<float> g_near_dist{0.25f};    // [Holsters] ZonesNearDistance: how near (m, past the ring's radius)
std::atomic<bool> g_steady_zones{true};  // [Holsters] SteadyZones (2026-10-09): the zones' edges with hysteresis
constexpr float kZoneExit = 0.03f, kZoneSticky = 0.03f;  // SteadyZones: past the radius to leave; nearer to change
std::atomic<bool> g_place{false};  // placing the holsters by hand (the menu; the session only)
std::atomic<bool> g_piece_place{false};  // run 12 item 3: placing John's belt's pieces by hand (the menu; the session only)
std::atomic<uint64_t> g_pieces_placed{0};
std::atomic<int> g_piece_held[2] = {-1, -1};  // the piece each hand holds while placing (-1 none)
std::atomic<uint64_t> g_placed{0};  // holsters moved by hand and saved
void save_zone(int i);              // (below) a zone's offset and radius into the user ini
// the shown guns of the last update (kShowZones order: the weapon, -1 none) and when ("Place them by hand" takes them)
int g_shown_last[4] = {-1, -1, -1, -1};
double g_shown_ms = 0.0;
// the frame the zones' offsets are moved in by hand (the update's: right and forward, and the left-handed mirror),
// for a world move put on an offset (under g_zone_mutex)
float g_link_rgt[3] = {1.0f, 0.0f, 0.0f}, g_link_fwd[3] = {0.0f, 0.0f, -1.0f};
bool g_link_lefty = false;
// (2026-11-05, the user: "Left handed mode switches the left and right holsters, doesn't need to do that", then "remove
// mirror holsters option, not needed") left-handed, the holsters stay where they are; mirrored only by the test
// channel's "holster mirror on" (the session: the old layout, a test's control)
std::atomic<bool> g_mirror_lefty{false};
std::atomic<bool> g_ring_gun_axes{true};
std::atomic<bool> g_steady_ring{true};  // [Reload] SteadyRing (run 8 item 2): the ring's place on the gun locked after the draw
std::atomic<uint64_t> g_ring_locks{0};  // [Reload] RingInGunAxes (run 8 item 2): the ring's offset along the drawn gun's axes
std::atomic<bool> g_show_dots{false};     // [Holsters] ShowHandDots (run 8 item 5b; absent: ShowZones')
std::atomic<bool> g_show_points{false};   // [Holsters] ShowWeaponPoints: the foregrip ring, the load point, the action hints
// [Holsters] ReleaseMargin: a grip pressed in a zone counts as let go there within this times its radius (run 4: with a
// gun in hand the drawn wrist sits near the hip zone's edge, and a release drifted just outside it)
std::atomic<float> g_release_margin{1.3f};
float g_fore_off[3] = {0, 0, 0};          // [Reload] ForegripOffset (right, up, forward), under g_zone_mutex
std::atomic<float> g_grab_off[3] = {};     // [Hands] InteractOffset (right, up, forward)
struct GunAdjSet {  // round 13 item 8: one gun's own adjustments (kAdjForegrip, kAdjForeRing, kAdjLoadRing, kAdjMag), under g_zone_mutex
    bool own[kAdjCount] = {};
    float off[kAdjCount][3] = {};
    float radius[kAdjCount] = {};
};
constexpr int kAdjGuns = kWeapons;  // (C35: every weapon; were the guns 0-20)
// (2026-11-01, GOAL-offhand-changes C8; the user: every gun its own loading ring and foregrip for each hand) [1] the gun
// held in John's right hand (the keys as before), [0] in his left ([Weapon.<gun>] ...Left keys; without them: the right's)
GunAdjSet g_gun_adj[kAdjGuns][2];
std::atomic<int> g_gun_in_hand{-1};
const char* const kAdjKey[kAdjCount][2] = {{"ForegripOffset", nullptr}, {"ForegripRingOffset", "ForegripRingRadius"}, {"LoadPointOffset", "LoadPointRadius"},
                                           {"MagazineWellOffset", nullptr}, {"PartOffset", nullptr}};
const char* const kAdjName[kAdjCount] = {"foregrip", "foregrip ring", "loading ring", "magazine", "action's spot"};
const float kAdjNone[3] = {0, 0, 0};  // (kAdjMag: none of every gun's)
float g_fore_radius = 0.20f;              // [Reload] ForegripRadius (since run 6 only the zone radius's default)
// run 6 item 4: the foregrip ring apart from the grip: where the front hand engages is the grip point (the game's grip
// moved by ForegripOffset, where John's hand sits) moved again by [Reload] ForegripZoneOffset (right, up, forward in the
// gun's axes), within [Reload] ForegripZoneRadius (ForegripRadius when the key is absent). Under g_zone_mutex.
float g_fzone_off[3] = {0, 0, 0};
float g_fzone_radius = 0.20f;
// run 6 item 3: where a round goes into the gun in hand: the loading point (0.10 m out along the barrel from the gun
// hand's wrist) moved by [Reload] LoadPointOffset (right, up, forward in the gun's axes, m), the ring's radius
// [Reload] LoadPointRadius (a round held within it goes in; let go within it + 0.04 m), and [Reload] InsertOnTouch: the
// drawn round itself (its pose, not the wrist) within the radius puts it in, the grip still held (on by default, round
// 13 item 11). Under g_zone_mutex.
float g_load_off[3] = {0, 0, 0};
float g_load_radius = 0.12f;
bool g_insert_touch = true;
struct LoadDiag {  // "holster load": the last frame's point and the held round's distances (-1: none)
    float pt[3] = {}, round[3] = {}, axis[3] = {};  // the round's position and its axis (base to tip)
    float wrist_d = -1.0f, round_d = -1.0f;
    bool round_ok = false;
    uint64_t inserts_touch = 0, inserts_wrist = 0, inserts_let_go = 0;
    // (cycle r174, 2026-10-27: the pool's "shells in" saw no insert at all) the closest the held round and the wrist came
    // to the point since the grab (-1: none measured), the radius that gun's ring uses, the gun's can_load then
    float min_round_d = -1.0f, min_wrist_d = -1.0f, ring_r = 0.0f;
    int can_load = -1;
    // (2026-10-27, [Hands] DualWieldCrossReload; "holster cross") the second gun's loading point and ring this frame
    float sec_pt[3] = {}, sec_r = 0.0f;
    bool sec_ok = false;
} g_load_diag;
std::atomic<bool> g_fore_snap{true};      // [Reload] TwoHandedSnap
Markers g_markers{};                      // under g_hdiag_mutex
bool g_markers_valid = false;
double g_markers_ms = 0;
// [Holsters] InteractionHighlights (2026-10-29, GOAL-weapons-handling.md item 5): 0 rings, 1 highlights, 2 both, 3 parts
// (run 29 item 2: the gun's places glow on the gun's own surface, the rest as highlights); the frame end's highlights and,
// per place, why it is lit or not ("holster highlights")
std::atomic<int> g_hl_mode{0};
// (round 52) the places on the carried gun this update (bits by HighlightId: its part, its pump), and a carry with no
// gun in hand (its part and pump then the carried gun's whatever this update put: a fade-out stays on it), for highlight_gun
std::atomic<uint32_t> g_hl_carried{0};
std::atomic<bool> g_hl_carry_free{false};
const char* const kHlModes[4] = {"rings", "highlights", "both", "parts"};
// [Holsters] PartGlowColor (sRGB 0-1, round 45's warm gold by default) and PartGlowStrength (0-8, 1 the default): the parts
// mode's glow on the gun, set by the menu or (the session) "holster glow color|strength"
std::mutex g_glow_mx;
float g_glow_rgb[3] = {1.0f, 0.84f, 0.52f};  // under g_glow_mx
std::atomic<float> g_glow_strength{1.0f};
static_assert(kHlFore == kZones, "the highlights' ids follow the zones");
Highlights g_hl{};  // under g_hdiag_mutex
bool g_hl_valid = false;
std::atomic<bool> g_row_cross{false};  // (step 7, R12) the chest's row is the cross-reload target's (the status: "holster load")
// (r313 B10) the highlights' last hint, drawn gun's frame and shut-it branch (0 none, 1 the part, 2 no part, 3 the loading point)
std::atomic<int> g_hl_dbg_hint{0}, g_hl_dbg_frame{0}, g_hl_dbg_shut{0};
double g_hl_ms = 0;
struct HlDiag {
    bool exists = false, usable = false, sights = false;
    float d = -1.0f, reach = 0.0f, target = 0.0f;
    float pos[3] = {};
    int hand = -1;         // the controller it was judged by
    const char* on = "";   // a part's glow: what it sits on ("lever", "breech", "hammer", "fore", "zone", "point")
};
HlDiag g_hl_diag[kHlCount];  // under g_hdiag_mutex
std::atomic<uint64_t> g_unarmed_sets{0};
std::atomic<uint64_t> g_player_triggers{0};

// Game memory the action trees themselves work from, read and written with SEH as the backstop (no system calls:
// every actor's transition ops run through the hooked update every frame).
bool raw(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool rd(uintptr_t a, T* out) {
    return a && raw(a, out, sizeof(T));
}

template <class T>
bool wr(uintptr_t a, T v) {
    __try {
        *reinterpret_cast<T*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The player's actor (the object's +0xb0 handle in the actor pool), as body.cpp finds it.
uintptr_t player_actor();
uintptr_t holster_player_actor() { return player_actor(); }
std::atomic<double> g_turret_ready_ms{-1e12};  // the mounted gun's matrices last read (pose keeps first person on it only then)
bool turret_ready_now() { return log::now_ms() - g_turret_ready_ms.load(std::memory_order_relaxed) < 500.0; }
// [Hands] MountedGunFirstPerson (2026-10-13, experimental; the user: "Can we attempt to make the gatling gun controllable
// in first person with motion controls?"): on a mounted gun with first person kept, the turret (the game's: the right
// stick turns it while the game's own camera holds the channel, round 137) is turned by the right stick put to the game
// (xinput's override): the stick's push from the yaw and pitch between the wanted direction and the barrel, full at 20
// degrees, none within 1. The barrel is one of the firing object's matrices' axes (W +0x80, the gun's, and +0xd0, the
// shoot-from's: for a turret its barrels bone): the gun's -c until a round shows which axis it leaves along. The stick's
// sense in each direction is learnt: pushed one way, the barrel turning the other way flips it.
// (round 139, the user on the mission's Gatling with the gun following the gun hand's ray: "It turns but it is
// unintuitive. Right hand should manually work the lever on the right to fire. Left hand should snap on to gun with
// grip and control its movement.") The hands on the gun:
//  - John's arms stay on the gun as the game has them (a free arm following its controller made the gun spin: its
//    matrix comes from his animated hand; cycle RU6). The LEFT grip takes the gun's handle from wherever the hand is:
//    the gun turns about its pivot as the hand moves, as a real one held by its rear handle: the pivot is taken kLever
//    ahead of the hand along the barrel at the grab (so nothing jumps), and the wanted direction is from the hand
//    through it. Let go, the gun stays where it is (no push);
//  - the RIGHT grip takes the crank: turning the hand in a circle (about the gun's
//    side axis; its middle the hand's own place low-passed) fires while it turns: the game's right trigger held for it.
//    The trigger itself fires as before.
constexpr float kLever = 0.5f;       // the handle behind the pivot (m): the hand's travel per radian of the gun's turn
constexpr float kCrankMin = 0.015f;  // the hand at least this far from its circle's middle to have an angle (m)
// (round 141, the user: "Crank should turn with the hands speed, if possible fire rate should be relative to hand speed.
// Default being the maximum speed.") The crank fires by its angle: once it has turned kCrankEngage one way (a push and
// pull turns both ways and never gets there), a round for every kCrankShot of turn (the trigger pressed kCrankPulseMs
// for each), and from kCrankFull on the trigger is held: the gun's own rate, the most there is
constexpr float kCrankEngage = 2.8f;    // rad one way before the first round (under half a turn; the review: one push
                                        // and pull stroke past the middle sweeps up to 2.4)
constexpr float kCrankShot = 0.6283f;   // rad a round: ten a turn
constexpr float kCrankFull = 5.6f;      // rad/s (0.9 of a turn a second): the trigger held
constexpr float kCrankStop = 0.8f;      // rad/s: under it for 0.3 s the crank has stopped
constexpr uint32_t kCrankPulseMs = 60;
struct TurretState {
    // the barrel's axis: (matrix * 3 + row) * 2 + negated. (Round 141) the shoot-from's c negated (cycle RU8: on the
    // Gatling the same direction as the gun matrix's -c within 0.2 degrees), and only the shoot-from's axes taken from a
    // round: the gun's matrix comes from John's hand, and his hands are free now
    int axis = (3 + 2) * 2 + 1;
    double cal_ms = -1.0;  // the round it was taken from
    float sx = 1.0f, sy = 1.0f;  // the stick's learnt sense (1: as guessed, -1: flipped)
    double win_ms = 0.0;  // the learning window's start, the barrel's yaw and pitch then, the push held through it
    float win_yaw = 0.0f, win_pitch = 0.0f, win_rx = 0.0f, win_ry = 0.0f;
    bool active = false;
    double on_ms = 0.0;  // (round 137) the drive's start: no sense learnt in its first 1.5 s (the gun swings into place then)
    double log_ms = 0.0;
    double flip_ms[2] = {0.0, 0.0};  // the last flip of each sense; three each and the learning stops (no steady answer)
    int flips[2] = {0, 0};
    // (round 139) the hands on the gun
    bool l_held = false, r_held = false, pushing = false;
    double l_ms = 0.0;         // the handle taken (the senses learnt only 0.5 s on)
    float pivot[3] = {};       // where the gun turns about, as taken at the grab (from the view's origin, the world's axes)
    float c_mid[3] = {};       // the crank hand's circle's middle (the same)
    float c_ang = 0.0f, c_rate = 0.0f;  // its last angle about the middle (rad), its SIGNED turn rate smoothed (rad/s)
    bool c_have = false;
    double fire_ms = -1e12, last_ms = 0.0;
    bool firing = false;
    uint64_t grabs = 0, cranks = 0;
    // (round 141) the crank by its angle
    float c_turn = 0.0f;   // the turn one way since it last stopped or turned back (rad, signed)
    float c_shot = 0.0f;   // the turn since the last round (rad)
    double c_slow_ms = -1.0;  // under kCrankStop since (ms; -1 not)
    float c_back = 0.0f;      // (round 142) the turn the other way since the crank last turned its own way (rad)
    bool c_full = false;      // (round 142) at the gun's own rate (left only under 0.6 of kCrankFull for 0.25 s)
    double c_dip_ms = -1.0;
    uint64_t pulses = 0, holds = 0;  // rounds asked one by one; frames with the trigger held
};
TurretState g_turret;
std::atomic<bool> g_turret_on{false};  // the drive on (any thread: the grips are the gun's then, grip_consumed)
std::atomic<bool> g_wheel_hol{false};    // (round 143) [Holsters] WheelAtHolster
std::atomic<int> g_wheel_zone[2] = {-1, -1};  // the holster each controller's empty hand is at (the trigger's wheel)
// (the review: none after an early return of the frame) its time: older than 100 ms, none. (Cycle HP29: cleared at the
// frame's top instead, the pad's poll inside the frame saw none there and let the trigger's first frame reach the game,
// which drew the last weapon)
std::atomic<double> g_wheel_zone_ms{-1e12};
std::atomic<uint64_t> g_wheel_assigned{0}, g_wheel_assign_lost{0}, g_wheel_lt_draws{0};
std::atomic<bool> g_turret_held[2] = {};  // (round 141) the hand's grip holds the gun (its arm the game's)
std::atomic<bool> g_turret_arms_free{true};  // ("holster turret arms free|game": the test's switch)
std::atomic<bool> g_turret_gun_axis{false};  // (the review) the barrel taken from the gun's own matrix (no shoot-from axis fits
                                             // this gun's rounds): that matrix comes from John's hand, so his arms stay the game's
void turret_off(TurretState& T, const char* why) {
    if (T.pushing) xinput::set_override(0, false, 0.0f, 0.0f, 0);
    if (T.firing) xinput::set_rt_override(0);
    T.pushing = T.firing = T.l_held = T.r_held = false;
    g_turret_on.store(false, std::memory_order_relaxed);
    g_turret_held[0].store(false, std::memory_order_relaxed);
    g_turret_held[1].store(false, std::memory_order_relaxed);
    T.c_turn = T.c_shot = T.c_back = 0.0f;
    T.c_slow_ms = T.c_dip_ms = -1.0;
    T.c_full = false;
    if (!T.active) return;
    T.active = false;
    T.axis = (3 + 2) * 2 + 1;  // (each gun from the shoot-from's -c again)
    g_turret_gun_axis.store(false, std::memory_order_relaxed);
    T.flips[0] = T.flips[1] = 0;
    log::info("[turret] the hands' control off (%s): the handle taken %llu times, the crank fired %llu times", why,
              static_cast<unsigned long long>(T.grabs), static_cast<unsigned long long>(T.cranks));
}
void turret_drive(const body::BodyPoints* bp, uintptr_t actor, double now) {
    TurretState& T = g_turret;
    // the gun read whether or not the anchor is on (pose waits on it: turret_ready); driven only with the anchor on and a
    // mounted gun in hand now (not a shot's 1.5 s after)
    const bool want = actor && pose::mounted_gun_held() && pose::mounted_first_person();
    uintptr_t wmgr = 0, item = 0, W = 0;
    float M[2][16];
    const bool readable = want && rd(actor + 0x70, &wmgr) && wmgr && rd(wmgr + 0x80, &item) && item && rd(item + 0xa0, &W) && W &&
                          rd(W + 0x80, &M[0]) && rd(W + 0xd0, &M[1]);
    if (readable) g_turret_ready_ms.store(now, std::memory_order_relaxed);
    if (!(readable && pose::anchor_active())) {
        turret_off(T, !want ? "the mounted gun left" : !readable ? "the gun not read" : "first person off");
        return;
    }
    if (!bp || !bp->cam_ok) return;  // (the review: a missed read of the body's points is a frame skipped, not a let-go)
    auto axis_of = [&](int ax, float out[3]) {
        const int m = ax / 6, r = (ax / 2) % 3;
        const float sg = (ax & 1) ? -1.0f : 1.0f;
        float l = 0.0f;
        for (int k = 0; k < 3; ++k) out[k] = sg * M[m][r * 4 + k], l += out[k] * out[k];
        l = std::sqrt(l);
        if (l < 1e-4f) return false;
        for (int k = 0; k < 3; ++k) out[k] /= l;
        return true;
    };
    {  // a new round: the axis it left along (the best of the twelve, within 15 degrees)
        float d[3];
        double ms = 0.0;
        if (aim::mounted_shot(d, &ms) && ms > T.cal_ms) {
            T.cal_ms = ms;
            int best = -1;
            float bd = 0.966f;
            for (int ax = 6; ax < 12; ++ax) {  // (round 141: the shoot-from's first)
                float a[3];
                if (!axis_of(ax, a)) continue;
                const float dt = a[0] * d[0] + a[1] * d[1] + a[2] * d[2];
                if (dt > bd) bd = dt, best = ax;
            }
            // (the review) a gun whose shoot-from matrix has no axis along its rounds (only the Gatling was measured): the
            // gun's own matrix as before round 141, and the arms the game's on it (turret_hand_free)
            if (best < 0)
                for (int ax = 0; ax < 6; ++ax) {
                    float a[3];
                    if (!axis_of(ax, a)) continue;
                    const float dt = a[0] * d[0] + a[1] * d[1] + a[2] * d[2];
                    if (dt > bd) bd = dt, best = ax;
                }
            if (best >= 0) g_turret_gun_axis.store(best < 6, std::memory_order_relaxed);
            if (best >= 0 && best != T.axis) {
                log::info("[turret] the barrel is the %s matrix's %c%c (a round along it within %.1f deg; was %d)", best / 6 ? "shoot-from" : "gun",
                          (best & 1) ? '-' : '+', "abc"[(best / 2) % 3], std::acos(std::fmin(1.0f, bd)) * 57.29578f, T.axis);
                T.axis = best;
            }
        }
    }
    float b[3];
    if (!axis_of(T.axis, b)) {  // (the review) a shoot-from matrix not read as one: the gun's -c, the arms the game's
        if (T.axis < 6 || !axis_of(2 * 2 + 1, b)) return;
        g_turret_gun_axis.store(true, std::memory_order_relaxed);
    }
    auto yaw_of = [](const float* v) { return std::atan2(-v[0], -v[2]) * 57.29578f; };  // (the heading's sense: left positive)
    auto pitch_of = [](const float* v) { return std::asin(std::fmax(-1.0f, std::fmin(1.0f, v[1]))) * 57.29578f; };
    const float by = yaw_of(b), bpitch = pitch_of(b);
    if (!T.active) {
        T.active = true;
        g_turret_on.store(true, std::memory_order_relaxed);
        T.on_ms = now;
        T.last_ms = now;
        T.sx = T.sy = 1.0f;  // (each gun from the guess, the Gatling's true senses: cycle RU6, a stand-in's flips carried over)
        T.grabs = T.cranks = 0;
        log::info("[turret] the hands' control on (the barrel the %s matrix's axis %d): the left grip takes the handle and moves the gun, the right grip "
                  "takes the crank and a turning hand fires", T.axis / 6 ? "shoot-from" : "gun", T.axis);
    }
    const float dt = static_cast<float>(std::fmax(0.001, std::fmin(0.1, (now - T.last_ms) / 1000.0)));
    T.last_ms = now;
    // the controllers' places in the world's axes (their own: the body's hand points are not published with the arms' IK
    // off, cycle RU6: taken from them, the grip let go and took again every frame), from the view's own origin (the
    // review: a pivot and a crank's middle kept as world points were left behind by a moving wagon, 0.5 m in a tenth of
    // a second: the same frame's camera carries the origin and the hands, so the mount's travel is in neither)
    const hands::Hand hc[2] = {hands::get(0), hands::get(1)};
    float hw[2][3] = {};
    bool hok[2] = {false, false};
    {
        const float zero[3] = {0.0f, 0.0f, 0.0f}, qid[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float o[3], wr[9];
        const bool o_ok = camera_lever::local_to_world(bp->cam, zero, qid, o, wr) && std::isfinite(o[0] + o[1] + o[2]);
        for (int h = 0; h < 2 && o_ok; ++h) {
            if (!(hc[h].valid || hc[h].grip_valid)) continue;
            hok[h] = camera_lever::local_to_world(bp->cam, hc[h].grip_valid ? hc[h].grip_pos : hc[h].pos, hc[h].grip_valid ? hc[h].grip_rot : hc[h].rot, hw[h], wr) &&
                     std::isfinite(hw[h][0] + hw[h][1] + hw[h][2]);
            for (int k = 0; k < 3; ++k) hw[h][k] -= o[k];
        }
    }
    const float gl = hc[0].grip, gr = hc[1].grip;
    // ---- the left hand on the handle
    const bool l_now = hok[0] && (T.l_held ? gl > 0.3f : gl > 0.6f);
    if (l_now && !T.l_held) {
        for (int k = 0; k < 3; ++k) T.pivot[k] = hw[0][k] + kLever * b[k];
        T.l_ms = now;
        ++T.grabs;
        if (T.grabs <= 20) log::info("[turret] the left hand took the handle (the barrel yaw %.1f pitch %.1f)", by, bpitch);
    }
    T.l_held = l_now;
    float u[3] = {b[0], b[1], b[2]}, ey = 0.0f, ep = 0.0f, rx = 0.0f, ry = 0.0f;
    // (round 137, cycle RU6 on the mission's Gatling: with the push e/20 the barrel stopped 4 to 5 degrees short, the stick
    // at 0.2: inside the pad's right-stick dead zone, 0.265) from 1 degree out the push starts above the dead zone
    auto push = [](float e) {
        const float m = std::fabs(e);
        if (m < 1.0f) return 0.0f;
        const float p = 0.30f + 0.70f * std::fmin(1.0f, (m - 1.0f) / 19.0f);
        return e < 0.0f ? -p : p;
    };
    if (T.l_held) {
        float d[3] = {T.pivot[0] - hw[0][0], T.pivot[1] - hw[0][1], T.pivot[2] - hw[0][2]};
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 0.15f)  // (the hand pushed up to the pivot has no direction: the barrel's kept)
            for (int k = 0; k < 3; ++k) u[k] = d[k] / l;
        ey = std::remainder(yaw_of(u) - by, 360.0f), ep = pitch_of(u) - bpitch;
        rx = -push(ey) * T.sx, ry = push(ep) * T.sy;  // (the guess: the stick right turns it right, up tilts it up)
        xinput::set_override(0, true, rx, ry, 150);
        T.pushing = true;
    } else if (T.pushing) {
        xinput::set_override(0, false, 0.0f, 0.0f, 0);
        T.pushing = false;
    }
    // learning the sense: a push held a third of a second; the barrel turned against it flips it (not while the gun is
    // taken, round 137, nor in the half second after the handle is: the gun settles)
    const bool held_x = std::fabs(T.win_rx) > 0.4f && std::fabs(rx) > 0.4f && (rx > 0) == (T.win_rx > 0);
    const bool held_y = std::fabs(T.win_ry) > 0.4f && std::fabs(ry) > 0.4f && (ry > 0) == (T.win_ry > 0);
    if (now - T.on_ms < 1500.0 || !T.l_held || now - T.l_ms < 500.0) {
        T.win_ms = now, T.win_yaw = by, T.win_pitch = bpitch, T.win_rx = rx, T.win_ry = ry;
    } else if (now - T.win_ms > 330.0) {
        const float dy = std::remainder(by - T.win_yaw, 360.0f), dp = bpitch - T.win_pitch;
        auto may_flip = [&](int i) {
            if (T.flips[i] >= 3 || now - T.flip_ms[i] < 1000.0) return false;
            T.flip_ms[i] = now;
            if (++T.flips[i] == 3) log::info("[turret] the %s sense flipped three times: no steady answer to the stick, its learning stopped", i ? "up-down" : "left-right");
            return true;
        };
        // (round 139: over 6 degrees against the push, not 2: a wagon's own turn is not the gun's answer)
        if (held_x && std::fabs(dy) > 6.0f && (dy > 0) != (-T.win_rx * T.sx > 0) && may_flip(0)) {  // wanted: the yaw's change along -rx
            T.sx = -T.sx;
            log::info("[turret] the stick's left-right sense flipped (the barrel turned %.1f deg against the push)", dy);
        }
        if (held_y && std::fabs(dp) > 6.0f && (dp > 0) != (T.win_ry * T.sy > 0) && may_flip(1)) {
            T.sy = -T.sy;
            log::info("[turret] the stick's up-down sense flipped (the barrel tilted %.1f deg against the push)", dp);
        }
        T.win_ms = now, T.win_yaw = by, T.win_pitch = bpitch, T.win_rx = rx, T.win_ry = ry;
    }
    // ---- the right hand on the crank
    const bool r_now = hok[1] && (T.r_held ? gr > 0.3f : gr > 0.6f);
    if (r_now) {
        const float* h = hw[1];
        if (!T.r_held) {
            std::memcpy(T.c_mid, h, sizeof(T.c_mid));
            T.c_have = false;
            T.c_rate = 0.0f;
        }
        const float am = 1.0f - std::exp(-dt / 0.5f);
        for (int k = 0; k < 3; ++k) T.c_mid[k] += (h[k] - T.c_mid[k]) * am;
        // the crank's plane: along the barrel (level) and up
        float fl = std::sqrt(b[0] * b[0] + b[2] * b[2]), fx = fl > 1e-3f ? b[0] / fl : 0.0f, fz = fl > 1e-3f ? b[2] / fl : 1.0f;
        const float vx = (h[0] - T.c_mid[0]) * fx + (h[2] - T.c_mid[2]) * fz, vy = h[1] - T.c_mid[1];
        float inst = 0.0f;  // (signed: a tremor's turns back and forth cancel in the smoothing, a crank's add up)
        if (vx * vx + vy * vy > kCrankMin * kCrankMin) {
            const float ang = std::atan2(vy, vx);
            if (T.c_have) inst = std::remainder(ang - T.c_ang, 6.2831853f) / dt;
            T.c_ang = ang;
            T.c_have = true;
        } else {
            T.c_have = false;
        }
        T.c_rate += (std::fmax(-40.0f, std::fmin(inst, 40.0f)) - T.c_rate) * (1.0f - std::exp(-dt / 0.15f));
    } else {
        T.c_rate = 0.0f;
        T.c_have = false;
    }
    T.r_held = r_now;
    // (round 141) the fire by the crank's angle
    bool firing = false;
    {
        const float w = r_now ? T.c_rate : 0.0f, aw = std::fabs(w);
        // (round 142, the user: "Cranking also felt difficult to keep consistent"; their log: at 1-3 turns a second the turn
        // read "turned back" every second or two (the hand's circle off the measured plane: its angle runs backwards a
        // moment), and every time the fire stopped and had to engage again) a turn back resets only past 0.9 rad, a stop
        // only after 0.5 s, and the full rate holds through a dip
        if (aw < kCrankStop) {
            if (T.c_slow_ms < 0.0) T.c_slow_ms = now;
            if (now - T.c_slow_ms > 500.0 || !r_now) T.c_turn = T.c_shot = T.c_back = 0.0f;  // stopped: the next turn engages anew
        } else {
            T.c_slow_ms = -1.0;
            if ((w > 0.0f) != (T.c_turn > 0.0f) && T.c_turn != 0.0f) {
                T.c_back += aw * dt;
                if (T.c_back > 0.9f) T.c_turn = T.c_shot = T.c_back = 0.0f;  // turned back for real
            } else {
                T.c_back = std::fmax(0.0f, T.c_back - aw * dt);
                T.c_turn += w * dt;
            }
        }
        const bool engaged = std::fabs(T.c_turn) >= kCrankEngage;
        if (!engaged) {
            T.c_full = false;
        } else if (aw >= kCrankFull) {
            T.c_full = true;
            T.c_dip_ms = -1.0;
        } else if (T.c_full) {
            if (aw >= 0.6f * kCrankFull) T.c_dip_ms = -1.0;
            else if (T.c_dip_ms < 0.0) T.c_dip_ms = now;
            else if (now - T.c_dip_ms > 250.0) T.c_full = false;
        }
        if (engaged && T.c_full) {  // the gun's own rate
            xinput::set_rt_override(150);
            T.c_shot = 0.0f;
            T.fire_ms = now;
            ++T.holds;
            firing = true;
        } else if (engaged) {
            T.c_shot += aw * dt;
            if (T.c_shot >= kCrankShot) {
                T.c_shot = std::fmin(T.c_shot - kCrankShot, kCrankShot);  // (no backlog of rounds)
                xinput::set_rt_override(kCrankPulseMs);
                T.fire_ms = now;
                ++T.pulses;
            }
            firing = now - T.fire_ms < 400.0;
        } else if (T.firing && now - T.fire_ms < 150.0) {
            firing = true;  // (a held trigger's last 150 ms run out by the override itself)
        }
        // (the review) the grip let go, or the crank stopped or turned back: the trigger let go at once
        if (!engaged && T.firing) {
            xinput::set_rt_override(0);
            firing = false;
        }
        if (firing && !T.firing) ++T.cranks;
        if (!firing && T.firing && now - T.fire_ms >= 150.0) xinput::set_rt_override(0);
    }
    T.firing = firing;
    g_turret_held[0].store(T.l_held, std::memory_order_relaxed);
    g_turret_held[1].store(T.r_held, std::memory_order_relaxed);
    if (now - T.log_ms > 1000.0 && (T.l_held || T.r_held)) {
        T.log_ms = now;
        log::info("[turret] the left hand %s: wanted yaw %.1f pitch %.1f, the barrel yaw %.1f pitch %.1f (off %.1f %.1f), the stick %.2f %.2f (senses "
                  "%+.0f %+.0f) | the right hand %s: the crank's turn %.1f rad/s (%.1f rad one way)%s; rounds asked one by one %llu, frames with the "
                  "trigger held %llu",
                  T.l_held ? "on the handle" : "free", yaw_of(u), pitch_of(u), by, bpitch, ey, ep, rx, ry, T.sx, T.sy, T.r_held ? "on the crank" : "free",
                  std::fabs(T.c_rate), T.c_turn, firing ? ", firing" : "", static_cast<unsigned long long>(T.pulses), static_cast<unsigned long long>(T.holds));
    }
}
uintptr_t player_actor() {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.object) return 0;
    uint32_t h = static_cast<uint32_t>(st.object);
    uintptr_t pool = 0, obj = 0, apool = 0, actor = 0;
    uint16_t gen = 0, idx = 0, agen = 0, sgen = 0;
    if (!rd(anchors::addr(anchors::Id::ObjectsPool), &pool) || !pool) return 0;
    uintptr_t slot = pool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16) || !rd(slot, &obj) || !obj) return 0;
    if (!rd(obj + 0xb0, &idx) || !rd(obj + 0xb2, &agen) || !rd(anchors::addr(anchors::Id::ActorPool), &apool) || !apool) return 0;
    if (!rd(apool + idx * 0x10u + 8, &sgen) || sgen != agen || !rd(apool + idx * 0x10u, &actor)) return 0;
    return actor;
}

// An op belongs to the player when its context (op +0x28) has the player's ped at +0x30; the context's +0x10 is the
// running node instance, whose +0x18 is the time in the node and +0xc8 the list of running ops {vtable, next, ...}.
bool player_op(uintptr_t op, uintptr_t* node, float* t) {
    uintptr_t ctx = 0, ped = 0;
    const uintptr_t pp = g_player_ped.load(std::memory_order_relaxed);
    if (!pp || !rd(op + 0x28, &ctx) || !rd(ctx + 0x30, &ped) || ped != pp) return false;
    *node = 0;
    *t = -1;
    if (rd(ctx + 0x10, node) && *node) rd(*node + 0x18, t);
    return true;
}

// The gun-ready node: the running node holds the aim-pose op (vtable 0x141d4c1b8; it sets gun +0x5d6 bit 0x40 on
// start and clears it on exit, and the gun state machine, FUN_140d198f0 state 8, holsters once that bit is clear).
bool has_aim_op(uintptr_t node) {
    const uintptr_t aim_vtbl = anchors::addr(anchors::Id::AimPoseOpVtbl);
    uintptr_t o = 0;
    if (!rd(node + 0xc8, &o)) return false;
    for (int g = 0; o && g < 24; ++g) {
        uintptr_t vt = 0, next = 0;
        if (!rd(o, &vt) || !rd(o + 8, &next)) return false;
        if (vt == aim_vtbl) return true;
        o = next;
    }
    return false;
}

int32_t gun_state(uintptr_t ped) {
    uintptr_t anim = 0, gun = 0;
    int32_t state = -1;
    if (ped && rd(ped + 0xaa8, &anim) && rd(anim + 8, &gun)) rd(gun + 0x3bc, &state);
    return state;
}

// ---- the block. The player's weapon controller (FUN_14034cae0, called each frame with the player info): in its
// state 1 (+0x504, a gun in hand) it counts +0x3e0 down by the frame time; aiming, firing and the game's "in combat"
// flags set it back to the draw time (DAT_142b24a58, set from the tuning at load); when it reaches 0 the gun is put
// away (FUN_140340080, the weapon manager's +0x80 hand slot cleared; ConditionWeaponDrawn then turns false and the
// action trees play the holster) and the state goes to 2. The game's own holster inputs go through FUN_14034ca10
// (state 4) and are untouched. With KeepGunDrawn the countdown is topped up before each update while a gun is drawn.
using PlayerWeapon_t = void (*)(uintptr_t info, int mode);
PlayerWeapon_t o_player_weapon = nullptr;
std::atomic<uint64_t> g_topped{0};
std::atomic<float> g_reset_seen{0.0f};

void hk_player_weapon(uintptr_t info, int mode) {
    if (g_keep.load(std::memory_order_relaxed) && mode != 6) {
        int32_t st = 0;
        float t = 0, reset = 0;
        if (rd(info + 0x504, &st) && st == 1 && rd(info + 0x3e0, &t) && rd(anchors::addr(anchors::Id::DrawTimeReset), &reset)) {
            g_reset_seen.store(reset, std::memory_order_relaxed);
            // only while it counts down (> 0): it is 0 while the aim is held, and Dead Eye starts only at 0
            const float want = reset > 0.5f ? reset : 3.0f;
            if (t > 0.0f && t < want && wr<float>(info + 0x3e0, want)) g_topped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    o_player_weapon(info, mode);
}


// ---- the body holsters ([Holsters], the menu's Holsters tab): draw a weapon by gripping at its holster on the drawn
// body, put the gun in hand away by gripping and letting go at its holster. A zone sits at one of the drawn body's
// holster bones (body::BodyPoints: pistol on the right hip, rifle on the back, thrower and melee on the pelvis) plus an
// offset in the body frame (right, up, forward, on the body heading), with a radius; the hand is the IK's wrist
// target (the controller). A grip used at a holster is the holster's: controls does not pass it on as LB/RB until it
// is let go. The draw is the game's own: GET_WEAPON_EQUIPPED names the slot's weapon, then ACTOR_PUT_WEAPON_IN_HAND
// (its equip task puts a different gun in hand away first; [Holsters] InstantDraw: at once, else with the draw clip).
// The holster too (ACTOR_PUT_ITEM_AWAY). The slot selection natives do not draw on the same tick (the current slot,
// wmgr +0x448, follows a tick later), so the grip at a holster drew nothing the first time.
struct Zone {
    const char* key;    // the ini prefix
    const char* label;  // the menu's
    int bone;           // body::BodyPoints::bone index
    int slots[3];       // the equip slots it draws, in order (-1 none)
    float off[3];       // right, up, forward (m)
    float radius;
    int weapon = -1;    // [Holsters] <Key>Weapon: the eWeapon it draws, -1 automatic (its slots in order)
    bool mirror = false;   // its bone mirrored across the body's middle (the left twins of the right hip and the back)
    int partner = -1;      // automatic: another owned weapon than this zone's (the twin's), when there is one
    bool enabled = true;   // [Holsters] <Key>Enabled
    // [Holsters] <Key>GunTurn, <Key>GunOffset (2026-10-12, the user: "change gun holster model angle so it better fits in
    // holster"): the shown gun (ShowGuns) turned about its grip at the holster, deg: its tilt (about the right axis, the
    // barrel forward or back), its turn (about up), its lean (about forward); then moved, m: right, up, forward. The
    // left twins mirror their partner's unless set
    float gun_turn[3] = {0.0f, 0.0f, 0.0f};
    float gun_off[3] = {0.0f, 0.0f, 0.0f};
    // [Holsters] LeftHipRingOffset (2026-10-13): the LeftHip's own offset from the mirrored left holster while it is drawn
    // (OwnBeltLeftHolster); LeftHipOffset stays the one without it
    float off_copy[3] = {0.0f, 0.0f, 0.0f};
};
// the LeftHip (the right hip's twin) on the mirrored left holster: its own offset then (Zone::off_copy)
// the gun holsters (the hips and the backs): AnyWeapon's
bool gun_zone(const Zone& z) { return z.bone == 0 || z.bone == 1; }
// A weapon fits a holster by its game equip slot; and (2026-10-26, the user: "Sawn off should be able to be put in both
// side arm and long gun holsters") the Sawed-off (15, a long-gun slot) fits the sidearm holsters too
constexpr int kSawedOff = 15;
constexpr double kCarryHoldMs = 400.0;  // [Hands] CarryByForegrip: the gun hand's grip held this long, then let go (RB's cover is a tap)
bool weapon_fits(const Zone& z, int w, int equip_slot) {
    // (2026-10-29, [Hands] OffHandItems; r258: the torch is not in the melee slot 0 of a save that owns it) the knife's
    // zone (the melee slot's) takes the torch as its choice too, whatever the torch's own slot: Holsters' Knife: Torch
    if (w == 33 && z.slots[0] == 0) return true;
    for (int s : z.slots)
        if (s >= 0 && (s == equip_slot || (w == kSawedOff && s == 1))) return true;
    return false;
}
bool zone_on_copy(const Zone& z) { return z.mirror && z.bone == 0 && body::left_holster(); }
float* zone_off(Zone& z) { return zone_on_copy(z) ? z.off_copy : z.off; }
// [Holsters] RingsFollowHolsters (2026-10-13): 1 a moved belt piece's ring goes with it and gripping the ring moves the
// piece (always at the same spot); 0 each moved on its own
std::atomic<bool> g_rings_follow{true};
Zone g_zones[kZones] = {
    // (2026-10-13, the user: "Right hip holster - primary side arm, Left hip holster - secondary side arm, Right back
    // holster - primary long gun, Left back holster - secondary long gun, Knife holster - knife, Satchel - throwables,
    // Lasso - Lasso. Ammo holster will just have to be wherevern the user decides": each holster its belt piece's)
    {"RightHip", "Right hip (primary sidearm)", 0, {1, -1, -1}, {0.0f, 0.0f, 0.0f}, 0.15f, -1},
    {"Back", "Right back (primary long gun)", 1, {5, 6, 4}, {0.0f, 0.05f, 0.0f}, 0.25f, -1},
    {"Belt", "Satchel (throwables)", 2, {2, -1, -1}, {0.0f, 0.0f, 0.0f}, 0.12f, -1},
    {"Knife", "Knife", 3, {0, -1, -1}, {0.0f, 0.0f, 0.0f}, 0.12f, -1},
    {"Ammo", "Ammo (a round to reload; the gun hand squeezed here reloads)", 4, {-1, -1, -1}, {0.0f, 0.0f, 0.15f}, 0.15f, -1},
    // round 8: "another long holster behind the left shoulder, another pistol holster on the left hip, move the melee
    // holster to the lower back". The twins mirror the right hip's and the back's bone and offset (unless set).
    {"LeftHip", "Left hip (secondary sidearm)", 0, {1, -1, -1}, {0.0f, 0.0f, 0.0f}, 0.15f, -1, true, 0, true},
    {"LeftShoulder", "Left back (secondary long gun)", 1, {5, 6, 4}, {0.0f, 0.05f, 0.0f}, 0.25f, -1, true, 1, true},
    // (2026-10-13: the lasso's own, at John's lasso (its rope bone); it replaced the lower back's knife and lasso holster:
    // the knife's sheath moves where it is wanted now)
    {"Lasso", "Lasso", 6, {3, -1, -1}, {0.0f, 0.0f, 0.0f}, 0.10f, -1},
};
// eWeapon order (WeaponModel 0..37): the ini's tokens and the menu's labels
const char* const kWeaponToken[kWeapons] = {"Volcanic", "SemiAutoPistol", "HighPower", "Mauser", "Cattleman", "Schofield", "DoubleAction", "LeMat", "Carbine", "Winchester", "Henry", "Evans", "Springfield", "BoltAction", "Buffalo", "SawedOff", "DoubleBarrel", "PumpAction", "SemiAutoShotgun", "RollingBlock", "Carcano", "Lasso", "Knife", "FireBottle", "Dynamite", "ThrowingKnife", "Gatling", "Browning", "Cannon", "Tomahawk", "Bow", "ExplosiveRifle", "ZombieSpit", "Torch", "Blunderbuss", "HolyWater", "ZombieBait", "BoomBait"};
const char* const kWeaponLabel[kWeapons] = {"Volcanic Pistol", "Semi-Automatic Pistol", "High Power Pistol", "Mauser Pistol", "Cattleman Revolver", "Schofield Revolver", "Double-Action Revolver", "LeMat Revolver", "Carbine Repeater", "Winchester Repeater", "Henry Repeater", "Evans Repeater", "Springfield Rifle", "Bolt Action Rifle", "Buffalo Rifle", "Sawed-off Shotgun", "Double-barreled Shotgun", "Pump-action Shotgun", "Semi-Auto Shotgun", "Rolling Block Rifle", "Carcano Rifle", "Lasso", "Knife", "Fire Bottle", "Dynamite", "Throwing Knife", "Gatling Gun", "Browning Gun", "Cannon", "Tomahawk", "Bow", "Explosive Rifle", "Zombie Spit", "Torch", "Blunderbuss", "Holy Water", "Zombie Bait", "Boom Bait"};
std::atomic<bool> g_weapon_choice{true}, g_any_weapon{false};
// [Holsters] ShowGuns (off; run 6 item 6a): each gun zone's gun shown at it (held_prop slots 2-5), hidden while in a hand
std::atomic<bool> g_show_guns{false};
// [Holsters] ShowBackGuns (on; round 13 item 14): off, ShowGuns leaves out the back and the left shoulder's long guns
std::atomic<bool> g_show_back_guns{true};
std::atomic<int> g_anchor{1};  // [Holsters] Anchor: 0 body, 1 headset (run 7 item 1d; the default)
std::atomic<bool> g_anchor_used{false};  // the last update moved the zones with the neck
// [Holsters] TurnWithHead / TurnDeadZone (2026-10-09, the user's request): with the headset anchor, the holsters turned
// with the headset's yaw since recentre (about the neutral head), once it is more than the dead zone from where they point
std::atomic<bool> g_turn_head{false};
std::atomic<float> g_turn_dead{0.0f}, g_zone_yaw_pub{0.0f};
// [Holsters] LeanSteady, TorsoLength, TurnByHands (2026-10-09, the holsters' polish)
std::atomic<bool> g_lean_steady{false}, g_turn_hands{false};
std::atomic<float> g_torso_len{0.55f}, g_lean_pub{0.0f}, g_hands_yaw_pub{-999.0f};
float g_anchor_shift[3] = {0, 0, 0};    // that move (world, m); under g_zone_mutex
float g_anchor_cam[3] = {0, 0, 0};      // the frame camera's position (the neutral head) then; under g_zone_mutex
// run 10 items 1 and 4: the places' takes (a recentre or the anchor newly chosen; never a dismount or a cutscene's end),
// the frames a take waited for John to settle, the frames the camera was not the anchored one; the right hip's place
std::atomic<uint32_t> g_place_takes{0}, g_place_waits{0}, g_place_offcam{0};
float g_place_hip[3] = {0, 0, 0};  // under g_zone_mutex
const int kShowZones[4] = {0, 1, 5, 6};  // the right hip, the back, the left hip, the left shoulder
// [Holsters] ShowModels (2026-10-09, the user's request): a holster model at the hips (held_prop kModelSlot + i), the
// game's own prop by its fragment name ([Holsters] Model), turned by ModelTurn (degrees about the holster's up, right,
// forward) and moved by ModelOffset (m: right, up, forward; the left hip's right mirrored, its turn about up reversed)
const int kModelZones[2] = {0, 5};  // the right hip, the left hip
std::atomic<bool> g_show_models{false};
char g_model_name[64] = "p_gen_gunbelt01x";
float g_model_off[3] = {0, 0, 0}, g_model_rot[3] = {90, 90, 0};  // under g_zone_mutex
float g_model_raw[3] = {0, 0, 0};  // "holster model raw a b c": the native's angles as given (the axes' test)
bool g_model_raw_on = false;       // under g_zone_mutex
void turn_about(float* v, const float* ax, float rad);  // (below)
// [Holsters] OwnBeltProps (GOAL-cattleman-belts item 2; the user: "All outfits except the default one were missing the
// belt", then "Add them, off by default"): on an outfit without its own knife sheath, satchel or lasso (the duster, outfit
// 11 and others: research in the goal sheet), the game's own props drawn where the custom belt has that piece: the bowie
// knife for the sheath (no sheath prop exists), the satchel, the lasso's coil. Each at its home (where the default outfit
// hangs it: right, up, forward from the pelvis in the holsters' frame), moved and turned as the piece's OwnBelt keys say,
// the prop's own base turn first ([Holsters] OwnBeltProp<Piece>Home and OwnBeltProp<Piece>Turn)
std::atomic<bool> g_belt_props{false};
std::atomic<bool> g_belt_props_force{false};  // ("holster beltprops force on|off": also on outfits with the piece, the test's)
constexpr int kBeltProps = 3;                 // pieces 1 (the sheath), 2 (the satchel), 3 (the lasso)
// (round 103; the user: "none of the belt items except for holsters are where they are on the default outfit. They don't
// line up with their interaction points", and "Get the exact default satchel") each as the default outfit has it, on this
// outfit's own bone as drawn (where its zone is): the knife the game's own model (melee_knife02x) and the lasso's coil, in
// their bones' frames as the game hangs them on the default outfit (cycle DM1, "skel binds"); the satchel prop at half size
// on the thrower bone, its box's middle at the default satchel's (r157's boxes). The default outfit's own satchel and
// sheath from its own model (player_default; body.cpp draws only those pieces of it): [Holsters] OwnBeltPropsDefault
// (GOAL-after-0.8.13.2 item 7, off by default; "holster beltprops model on|off" the session's). (2026-11-08, cycle IP3:
// the "bind-posed John" seen in third-person test views stayed with the model off: the headset's second John, not it;
// cycle SF8: on the duster the default outfit's own satchel at the left hip, nothing else of the model in view)
const char* const kBeltPropFrag[kBeltProps] = {"melee_knife02x", "p_gen_satchel01x", "melee_lasso01x"};  // (the knife: cycle EB2)
std::atomic<bool> g_bp_model{false};  // "holster beltprops model on|off": the satchel slot the default outfit's own model
// (round 117; the user: "We should try using the throwables models to represent the currently equipped throwable on the
// belt. For example, molotov model visible on belt when molotov equipped.") [Holsters] OwnBeltThrowable: the satchel's
// slot the thrown slot's item's model (offhand::item_fragment), at its own size, on the satchel's bone; hidden while that
// item is in a hand; none equipped, none shown. Each model's frame on the bone: kThrownOnBone (the cycles' pictures)
std::atomic<bool> g_bp_throwable{false};
// Each model on the satchel prop's frame and place (kBpOnBone[1]), at its own size, rolled about John's forward axis
// (degrees, as the session's "holster beltprops turn 1 0 0 <roll>") and dropped along his up (m). Cycle BT2 (r196b,
// six turns each, seen from his left side): with none each one stuck straight out sideways; the fire bottle neck up at
// 90, the throwing knife (melee_knife03x) blade down at -90 as in a sheath, the dynamite stick down the thigh at -90,
// the tomahawk's head up at 90 (its box: the handle 0.475 m along its y, the head at the far end), dropped 0.30 m so
// the head sits at the belt and the handle hangs
struct ThrownOnBone {
    int w;
    float roll, drop;
};
// (round 142, cycle UN4's pictures: at the knife's own pose the torch stands as the knife does, its handle up at the belt
// behind the holster and its head down along the back of the thigh; turned 90 or 180 degrees it stuck out behind or
// ran up the back. Placed by hand as the sheath is: OwnBeltSheathOffset/Turn move it with the knife's place)
#define TORCH_ROLL 0.0f
#define TORCH_DROP 0.0f
const ThrownOnBone kThrownOnBone[] = {
    {23, 90.0f, 0.0f},     // the fire bottle (throw_molotov01x)
    {24, -90.0f, 0.0f},    // dynamite (throw_dynamite01x)
    {25, -90.0f, 0.0f},    // the throwing knife (melee_knife03x)
    {29, 90.0f, -0.30f},   // the tomahawk (melee_tomahawk01x)
    // (round 132, the user: "Need models on belt for undead nightmare throwables") the Undead Nightmare's three (cycle UN1's
    // pictures; each one's own place by hand as the others')
    {35, 90.0f, 0.0f},     // holy water
    {36, 90.0f, 0.0f},     // undead bait
    {37, 90.0f, 0.0f},     // boom bait
    // (round 142, the user: "Add torch model to belt in undead nightmare") the torch at the knife's place (its pose on
    // the knife's: TORCH_ROLL, TORCH_DROP)
    {33, TORCH_ROLL, TORCH_DROP},
};
const ThrownOnBone& thrown_on_bone(int w) {
    for (const ThrownOnBone& t : kThrownOnBone)
        if (t.w == w) return t;
    return kThrownOnBone[0];  // (the Undead Nightmare's: as the fire bottle)
}
// the prop in its piece bone's frame (axes as columns, then the place; the bones' rest poses alike in every rig, r161)
const float kBpOnBone[kBeltProps][12] = {
    {-0.8798f, 0.3397f, 0.3325f, 0.0840f, -0.5774f, 0.8122f, 0.4678f, 0.7425f, 0.4794f, -0.0270f, 0.0463f, -0.0590f},
    {1, 0, 0, 0, 0, 1, 0, -1, 0, -0.123f, -0.034f, 0.077f},
    {0.9851f, 0.0372f, 0.1678f, -0.0271f, 0.9977f, -0.0621f, -0.1697f, 0.0566f, 0.9839f, 0.0000f, 0.0002f, 0.0003f}};
// the user's moves on top (OwnBeltProp<Piece>Home: right, up, forward m; Turn deg), none by default
float g_bp_home[kBeltProps][3] = {};
float g_bp_turn[kBeltProps][3] = {};
// (round 101, the user: "this big satchel clips through") each prop's size against the default outfit's own piece: its
// draws' axes scaled (held_prop moves each draw by the pose's axes, so a scaled pose draws it scaled). Measured (cycle
// BZ1, r157): the satchel prop's box 0.40 x 0.29 x 0.24 m against the outfit's satchel 0.13 x 0.19 x 0.18 (about half);
// the knife prop 0.45 m long against the sheath's 0.09 m of handle (the sheath holds the rest: 0.6, a 27 cm knife); the
// lasso's coil 0.33 m tall (the outfits' coil not measured: as made). [Holsters] OwnBeltProp<Piece>Scale sets each
// (round 103: the knife and the lasso the default outfit's own, at their own size; the satchel prop at half, r157)
float g_bp_scale[kBeltProps] = {1.0f, 0.5f, 1.0f};
std::mutex g_bp_mutex;  // g_bp_home, g_bp_turn, g_bp_scale, g_thr_adj
// (round 120; the user: OwnBeltThrowable "works, need to adjust model positions depending on what is equipped... I'd like
// to place the throwables myself. Using the move by hand option") each measured throwable's own place on the belt: a
// turn (3x3) and a move, both in the throwable's own axes as placed without it (so they ride with the belt: the review),
// put on its pose last; set by hand while placing John's belt's pieces (a grip within 12 cm of it), saved as
// [Holsters] OwnBeltThrowable<Name>Pose (the 9 numbers of the turn by rows, then the 3 of the move)
constexpr int kThrowables = 7;  // (round 132: with the Undead Nightmare's three)
const int kThrowableW[kThrowables] = {23, 24, 25, 29, 35, 36, 37};
const char* const kThrowableKey[kThrowables] = {"FireBottle", "Dynamite", "ThrowingKnives", "Tomahawk", "HolyWater", "ZombieBait", "BoomBait"};
// (round 132, cycle UN2's pictures: the baits are flat flasks, 4 cm thick and 9 cm wide (the boom bait 16 with its
// stick), hung as the fire bottle they stood edge-on to the hip) the two baits a quarter turn about their own long axis
// (y): the flat side to the leg. Each one's place before the user's own (and what "reset" goes back to)
#define RDRVR_THR_ID {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}
#define RDRVR_THR_FLAT {0, 0, 1, 0, 1, 0, -1, 0, 0, 0, 0, 0}
const float kThrAdjDefault[kThrowables][12] = {RDRVR_THR_ID, RDRVR_THR_ID, RDRVR_THR_ID, RDRVR_THR_ID, RDRVR_THR_ID, RDRVR_THR_FLAT, RDRVR_THR_FLAT};
float g_thr_adj[kThrowables][12] = {RDRVR_THR_ID, RDRVR_THR_ID, RDRVR_THR_ID, RDRVR_THR_ID, RDRVR_THR_ID, RDRVR_THR_FLAT, RDRVR_THR_FLAT};
int throwable_index(int w) {
    for (int i = 0; i < kThrowables; ++i)
        if (kThrowableW[i] == w) return i;
    return -1;
}
// (the review) an adjustment a turn (its rows unit and square to each other, not a mirror) and a move within 1 m
bool thr_adj_ok(const float* a) {
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(a[i])) return false;
    for (int r = 0; r < 3; ++r) {
        for (int q = r; q < 3; ++q) {
            const float d = a[r * 3] * a[q * 3] + a[r * 3 + 1] * a[q * 3 + 1] + a[r * 3 + 2] * a[q * 3 + 2];
            if (std::fabs(d - (r == q ? 1.0f : 0.0f)) > 0.02f) return false;
        }
    }
    const float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    return det > 0.0f && std::fabs(a[9]) <= 1.0f && std::fabs(a[10]) <= 1.0f && std::fabs(a[11]) <= 1.0f;
}
int g_thr_shown_w = -1;      // the throwable shown at the satchel's place by the last update (holster_frame's thread)
double g_thr_shown_ms = -1e12;
float g_thr_shown_pos[3] = {};  // its place then (world; "holster beltprops", g_bp_mutex)
std::atomic<uint64_t> g_thr_placed{0};  // throwables placed by hand and saved
std::atomic<uint64_t> g_thr_hand_hides{0};
std::atomic<uint64_t> g_none_left_refused{0};  // (round 123) the satchel holster's draws refused with none of the throwable left  // (round 122) updates the outfit's satchel hidden with the belt's throwable in a hand
float g_bp_frame[12] = {};  // the last frame's right, up, forward and the pelvis ("holster beltprops" measures by it)
bool g_bp_frame_ok = false;
bool belt_prop_pose(int k, const float* hr, const float* up, const float* hf, const float* origin, float* pz, float* anchor, bool model,
                    int thrown = -1, bool thrown_adj = true);
// The model's pose at a hip (12 floats: X, Y, Z columns, then the position): the holster's frame (X right, Y forward,
// Z up) turned by the model's turn, at the zone moved by its offset
void model_pose(int i, const float* at, const float* rgt, const float* up, const float* fwd, const float* off, const float* rot, float* pz) {
    const float side = i == 0 ? 1.0f : -1.0f;
    const float ya = side * rot[0] * 0.0174532925f, pa = rot[1] * 0.0174532925f, ra = side * rot[2] * 0.0174532925f;
    // R = Rz(yaw) * Rx(pitch) * Ry(roll) in the holster's (right, forward, up) basis: its columns, then to the world
    const float cy = std::cos(ya), sy = std::sin(ya), cp = std::cos(pa), sp = std::sin(pa), cr = std::cos(ra), sr = std::sin(ra);
    const float Rz[9] = {cy, -sy, 0, sy, cy, 0, 0, 0, 1}, Rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp}, Ry[9] = {cr, 0, sr, 0, 1, 0, -sr, 0, cr};
    float T[9], M[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) T[r * 3 + c] = Rx[r * 3 + 0] * Ry[0 * 3 + c] + Rx[r * 3 + 1] * Ry[1 * 3 + c] + Rx[r * 3 + 2] * Ry[2 * 3 + c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) M[r * 3 + c] = Rz[r * 3 + 0] * T[0 * 3 + c] + Rz[r * 3 + 1] * T[1 * 3 + c] + Rz[r * 3 + 2] * T[2 * 3 + c];
    const float* B[3] = {rgt, fwd, up};  // the holster's basis vectors (world)
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) pz[c * 3 + k] = B[0][k] * M[0 * 3 + c] + B[1][k] * M[1 * 3 + c] + B[2][k] * M[2 * 3 + c];
    for (int k = 0; k < 3; ++k) pz[9 + k] = at[k] + rgt[k] * off[0] * side + up[k] * off[1] + fwd[k] * off[2];
}
// the shown guns' error: the drawn position against the wanted one of the frame after (the walk's lag), the worst (m)
struct ShowDiag {
    int weapon[4] = {-1, -1, -1, -1};
    float err[4] = {};    // the last frame's: the drawn place on the body against the wanted one (m)
    float worst[4] = {};
    uint64_t stale[4] = {};  // frames shown with no draw of it in the last 100 ms (its draws not moved)
    uint64_t gated[4] = {};  // its model's draws gated out (held_prop)
    uint64_t over[4] = {};   // frames whose error passed 2 cm
    float rel[4][3] = {};    // the last drawn place against the pelvis (the anchor), m (2026-10-12: GunOffset's test)
    uint64_t frames = 0;
};
ShowDiag g_show_diag;  // under g_zone_mutex
// the gun models (fragments.rpf root/fragments, the 01x variants: the ones the clip and skeleton research read)
const char* gun_fragment(int w) {
    static const char* const kLong[13] = {"repeater_carbine01x", "repeater_winchester01x", "repeater_henry01x", "repeater_evans01x",
                                          "rifle_springfield01x", "rifle_boltaction01x", "rifle_buffalo01x", "shotgun_sawed01x",
                                          "shotgun_doublebarrel01x", "shotgun_pumpaction01x", "shotgun_semiauto01x", "rifle_rollingblock01x",
                                          "rifle_carcano01x"};
    if (w >= 0 && w < 8) return dual::sidearm_fragment(w);
    return w >= 8 && w <= 20 ? kLong[w - 8] : nullptr;
}
// A shown gun's pose at its zone (12 floats: the axes X, Y, Z, then the position; world): at a hip barrel down, its top
// forward (its grip at the zone); on the back diagonal, the barrel up and out over its shoulder, its right side facing
// back, its grip 30 cm down the barrel's line from the zone
void show_pose(int zi, const float* at, const float* rgt, const float* up, const float* fwd, const float* root, float* pz) {
    float X[3], Y[3], Z[3], P[3];
    if (zi == 0 || zi == 5) {
        for (int k = 0; k < 3; ++k) X[k] = rgt[k], Y[k] = fwd[k], Z[k] = up[k], P[k] = at[k];
    } else {
        const float side = (at[0] - root[0]) * rgt[0] + (at[2] - root[2]) * rgt[2] >= 0.0f ? 1.0f : -1.0f;
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = up[k] * 0.8f + rgt[k] * 0.5f * side;
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; ++k) {
            d[k] /= n;
            Z[k] = -d[k];
            X[k] = -fwd[k];
            P[k] = at[k] - d[k] * 0.30f;
        }
        Y[0] = Z[1] * X[2] - Z[2] * X[1];  // Y = Z x X: a right-handed frame
        Y[1] = Z[2] * X[0] - Z[0] * X[2];
        Y[2] = Z[0] * X[1] - Z[1] * X[0];
    }
    std::memcpy(pz, X, sizeof(X));
    std::memcpy(pz + 3, Y, sizeof(Y));
    std::memcpy(pz + 6, Z, sizeof(Z));
    std::memcpy(pz + 9, P, sizeof(P));
}
// a vector turned about a unit axis (Rodrigues)
void turn_about(float* v, const float* ax, float rad) {
    const float c = std::cos(rad), s = std::sin(rad), d = v[0] * ax[0] + v[1] * ax[1] + v[2] * ax[2];
    const float cr[3] = {ax[1] * v[2] - ax[2] * v[1], ax[2] * v[0] - ax[0] * v[2], ax[0] * v[1] - ax[1] * v[0]};
    for (int k = 0; k < 3; ++k) v[k] = v[k] * c + cr[k] * s + ax[k] * d * (1.0f - c);
}
// the zone's GunTurn and GunOffset applied to a shown gun's pose (12 floats) at its zone point `at`: turned about the
// grip at the zone in the holster's axes (the lean about forward, the tilt about right, the turn about up, in that
// order), then moved right, up, forward
void adjust_pose(const float turn[3], const float off[3], const float* at, const float* rgt, const float* up, const float* fwd,
                 float* pz) {
    if (turn[0] == 0.0f && turn[1] == 0.0f && turn[2] == 0.0f && off[0] == 0.0f && off[1] == 0.0f && off[2] == 0.0f) return;
    const float* axes[3] = {fwd, rgt, up};
    const float rad[3] = {turn[2] * 0.0174532925f, turn[0] * 0.0174532925f, turn[1] * 0.0174532925f};
    float rel[3] = {pz[9] - at[0], pz[10] - at[1], pz[11] - at[2]};
    for (int a = 0; a < 3; ++a) {
        if (rad[a] == 0.0f) continue;
        turn_about(pz, axes[a], rad[a]);
        turn_about(pz + 3, axes[a], rad[a]);
        turn_about(pz + 6, axes[a], rad[a]);
        turn_about(rel, axes[a], rad[a]);
    }
    for (int k = 0; k < 3; ++k) pz[9 + k] = at[k] + rel[k] + rgt[k] * off[0] + up[k] * off[1] + fwd[k] * off[2];
}
std::atomic<uint64_t> g_choice_draws{0};
const char* token_of(int w) { return w == kRounds ? "Rounds" : w >= 0 && w < kWeapons ? kWeaponToken[w] : "auto"; }
int weapon_from_token(const std::string& t) {
    if (t.empty() || _stricmp(t.c_str(), "auto") == 0) return -1;
    if (_stricmp(t.c_str(), "Rounds") == 0) return kRounds;  // run 10 item 10
    for (int w = 0; w < kWeapons; ++w)
        if (_stricmp(t.c_str(), kWeaponToken[w]) == 0) return w;
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (end && !*end && v >= 0 && v < kWeapons) return static_cast<int>(v);
    log::warn("[holster] unknown weapon \"%s\" (automatic)", t.c_str());
    return -1;
}
constexpr int kAmmoZone = 4;
// [Reload] PhysicalStock (run 14, after optimumbox's physical rifle stock, 0.8.10.4; off): for a stock that holds both
// controllers. The front hand's trigger at the foregrip takes hold of a long gun as its grip does (the trigger still
// goes to the game), the front hand no longer turns the gun (body.cpp, as [Reload] TwoHandedSteer=0), and every long
// gun is turned in the gun hand by [Reload] StockTurn (a rotation vector, degrees), learned by the menu's "Calibrate
// the stock": the barrel along the line from the gun hand's controller to the front hand's, its top up.
std::atomic<bool> g_stock{false};
// [Reload] TriggerTakesRound (run 14, after optimumbox's 0.8.10.4; off): the off hand's trigger pressed at the chest's
// ammo zone (the grip not held) takes a round as its grip does, held while the trigger is; the trigger still goes to the
// game (its aim)
std::atomic<bool> g_trig_round{false};
// trigger_for_round(): with TriggerTakesRound on, that hand's trigger is the round's (pressed at the ammo zone, or holding
// a round it took): the lever lock leaves it alone (the review: not a grip-taken round, so off is as before)
std::atomic<bool> g_trig_for_round[2] = {false, false};
// [Hands] DualWieldCrossReload (2026-10-27, the user: "When dual weilding, add option for both hands to be able to grab
// the other guns bullets and reload"): a hand holding a gun and the other gun's rounds fires nothing (cross_holds_fire)
std::atomic<bool> g_cross_fire_hold[2] = {false, false};
std::atomic<uint64_t> g_cross_takes{0}, g_cross_in{0};
// [Reload] RoundsPerGrab, RoundsGoIn, RoundInsertMs (2026-10-26, the user: "option to grab multiple bullets at once
// (amount configurable), with option between all going into gun at once or one at a time as you hold them in the reload
// spot"): a handful taken at the chest, drawn in the hand, put in all at once or one at a time
std::atomic<int> g_per_grab{1};
std::atomic<bool> g_all_in{false};
std::atomic<float> g_insert_gap_ms{300.0f};
// [Reload] RoundSource (2026-10-28, GOAL-weapons-handling.md item 2; the user: "Option to have bullets spawn in hand
// instead of on chest"; research/run28/inhand.md): 0 chest (the take at the chest's ammo zone only, as before); 1 gun
// (the user's choice, "Grip near the gun"): a hand that may take rounds (the free hand; DualWieldCrossReload: each
// dual-wielding hand, for the other gun) gripped within kNearShell of the target gun's loading ring (inside the ring
// too: they go straight in), out of the holsters' zones; 2 palm (the implementer's, kept as an option on the user's
// word): that hand turned palm up and gripped anywhere outside the zones and 5 cm past the ring. Either way the parts'
// presses keep priority (an action's part, the foregrip, the carried gun's grip); the rounds appear in the hand
// (RoundsPerGrab of them, the gun's own calibre) as the chest's take gives them, and load at the loading point as those
// do. The chest's take stays in every mode. Status (the last frame end): g_palm_y each controller's palm normal's world
// up (-2 not known); g_near_d its grab point's distance from the loading point it would load (-1 not a hand that may
// take); g_held_n / g_held_w the rounds each controller holds and the gun drawn for them
std::atomic<int> g_round_src{0};
constexpr float kPalmUp = 0.6f;     // the palm within about 53 degrees of straight up
constexpr float kNearShell = 0.15f;  // RoundSource=gun: the take reaches this far past the loading ring
std::atomic<float> g_palm_y[2] = {-2.0f, -2.0f}, g_near_d[2] = {-1.0f, -1.0f}, g_near_max[2] = {0.0f, 0.0f};
std::atomic<int> g_held_n[2] = {0, 0}, g_held_w[2] = {-1, -1};
std::atomic<uint64_t> g_hand_takes{0}, g_hand_refused{0};
// [Reload] PistolMagazines (2026-10-28): the magazines (clips) taken, refused, put in, let go away from the gun; each
// controller's held magazine's top from the well (-1 none) and the well (the "holster mag" status, the tests')
std::atomic<uint64_t> g_mag_takes{0}, g_mag_refused{0}, g_mag_ins{0}, g_mag_away{0};
std::atomic<float> g_mag_top_d[2] = {-1.0f, -1.0f};
std::atomic<int> g_mag_held[2] = {0, 0};
std::atomic<int> g_mag_rounds_in{0};  // the last magazine's (clip's) rounds put in
float g_mag_well_pub[3] = {};  // (under g_zone_mutex)
float g_mag_top_pub[2][3] = {};  // each controller's held magazine's top (the clip's foot), world (under g_zone_mutex)
float g_mag_mid_pub[2][3] = {};  // ... and its middle (the model's centre), world (under g_zone_mutex)
bool g_mag_well_ok = false;
std::mutex g_stock_mutex;
float g_stock_turn[3] = {};
std::atomic<double> g_stock_cal_ms{0.0};  // when the calibration takes its sample (0: none asked)
std::atomic<float> g_two_blend{0.0f};  // the two-handed aim's weight (body.cpp), 0..1 over a tenth of a second
std::atomic<uint64_t> g_load_on_model{0};  // (C37) the loading point placed from the drawn gun's frame (two-handed), "holster cross"
// (C37, C37c) each gun's loading point in the drawn gun's frame (its root: x right, y up, z back), as the gun in hand had it held
// in John's hand j alone (learned there each frame; the frame end's): the two-handed hold's and the carried gun's
float g_lg[kWeapons][2][3];
bool g_lg_ok[kWeapons][2] = {};
std::atomic<uint64_t> g_rounds_taken{0}, g_rounds_in{0}, g_clicks{0}, g_two_grips{0}, g_squeezes{0};
std::atomic<uint64_t> g_auto_chest{0};  // (round 55) the dual-wield chest takes by the entry, no press
std::mutex g_zone_mutex;  // the zone settings: the menu writes, the frame end reads
std::atomic<bool> g_holsters{true};
std::atomic<bool> g_instant{true};  // [Holsters] InstantDraw
std::atomic<bool> g_consumed[2] = {false, false};
std::atomic<bool> g_wanted[2] = {false, false};  // grip_wanted: as of the last frame end
// [Reload] CarriedBolt (actions.cpp): the carried gun's take-back point (world), the frame end's (carry_take_point)
std::mutex g_take_mx;
float g_take_pt[3] = {};
float g_take_ctl[3] = {};  // the same point in the carrying controller's frame (the fallback's; "skel carry trace")
bool g_take_ok = false, g_take_gun = false;
double g_take_ms = 0.0;
float g_grip_on = 0.6f;
std::atomic<uint64_t> g_draws{0}, g_puts{0}, g_entries{0};
std::atomic<uint64_t> g_twin_hidden{0};  // run 7 item 1b: frames a twin hip hid the gun its partner shows
// the frame end's last view, for the status line
struct HolsterDiag {
    bool valid = false;
    float zone[kZones][3] = {};
    bool zone_ok[kZones] = {};
    float hand[2][3] = {};
    bool hand_ok[2] = {};
    int in_zone[2] = {-1, -1};
    int cur_slot = -1;
    bool in_hand = false;
    float cam[3] = {};  // the frame camera's position in the same update (run 12: the tests' zone against the camera)
    bool cam_ok = false;
    float base[kZones][3] = {};  // each zone's base (its bone without a belt piece's move, a twin's mirrored) ("holster bases")
    float root[3] = {}, right[3] = {};  // the twins' mirror: the root and the body's right
};
HolsterDiag g_hdiag;
std::mutex g_hdiag_mutex;

const uint32_t kWeaponEquipped = 0x42C0FAAA;   // GET_WEAPON_EQUIPPED(actor, slot): the slot's eWeapon, -1 none
const uint32_t kPutWeaponInHand = 0x8F4B473D;  // ACTOR_PUT_WEAPON_IN_HAND(actor, eWeapon, animated)
const uint32_t kIsMinigameRunning = 0x117D7E71;     // IS_MINIGAME_RUNNING()
const uint32_t kIsPlayerControllable = 0x9613C2D0;  // IS_PLAYER_CONTROLLABLE(player)
const uint32_t kGetCurrentGringo = 0x5D9DB7A5;      // GET_CURRENT_GRINGO(actor): the gringo (a scripted use spot) he is in
const uint32_t kIsGringoValid = 0x7C858A47;         // IS_GRINGO_VALID(gringo)
const uint32_t kIsActorOnTrain = 0xD36E2D54;        // IS_ACTOR_ON_TRAIN(actor, 0)
// (round 114) the holsters closed while the game runs a minigame (bit 0), closed to draws into empty hands while it holds
// the player's control (bit 1) (holster_frame's poll; "holster gate")
// (round 128, the user: "look into other mini games. Liars dice and horseshoes for example. Also ... when I sat on the
// train, the camera was in front of johns body"; research/after-08132/minigames_research.md) only poker raises the
// minigame's flag (START_MINIGAME: poker01, and carcrank_gringo, tamewildhorse). Liar's dice, horseshoes, blackjack, five
// finger fillet and arm wrestling take the player's control and snap John to a gringo; the train's seat is a gringo too,
// John left controllable while he rides. So two more answers: bit 2 (4) he is in a gringo, bit 3 (8) he is on a train; and
// two made from them once they have held a while (a chest opened, a door: a second or less): bit 4 (16) a table game by
// the control held in a gringo for kTableHoldMs, bit 5 (32) seated on a train (in a gringo on one, or his head at a seat's
// height on one, for kSeatHoldMs)
constexpr double kTableHoldMs = 1500.0, kSeatHoldMs = 1000.0, kHoldOffMs = 600.0;
bool gate_table(int g) { return (g & 1) != 0 || (g & 16) != 0; }
bool gate_seated(int g) { return gate_table(g) || (g & 32) != 0; }
std::atomic<int> g_game_gate{0};
std::atomic<double> g_game_gate_ms{-1e9};  // (the re-review) the gate's last poll: older than 2 s, taken as open
std::atomic<uint64_t> g_gate_closes{0};
// (round 130, the user: blackjack's dealer "Same as before"; "Liars dice does not work [with John's own arms] ... Make it
// unaffected by it"; their log: the named-script test of round 129 never answered) the table games all read the same gate,
// so which one runs is read from the game's own list of scripts, as _IS_ANY_NAMED_SCRIPT_RUNNING walks it (RDR.exe
// 0x1ffa10: the list's head pointer by the mov at +0x23; a node: the next at +8, running at +0x30, its first child at
// +0x50, its name at +0x70, compared without case): a running script whose name ends in a minigame's launch path
// (LAUNCH_NEW_SCRIPT_WITH_ARGS("content/Mini_Games/BlackJack01/BlackJack01", ...); the launchers' own scripts, in other
// folders or of other names, do not). Walked on a script tick (the thread that changes the list), asked for by the
// gate's poll four times a second. "holster scripts [force <bits>|off]"
constexpr const char* kMiniTails[6] = {"/blackjack01/blackjack01", "/liarsdice/liarsdice",       "/fivefingerfillet/fivefingerfillet",
                                       "/horseshoes/horseshoes",   "/armwrestling/armwrestling", "/poker01/poker01"};
constexpr const char* kMiniNames[6] = {"blackjack", "liar's dice", "five finger fillet", "horseshoes", "arm wrestling", "poker"};
constexpr int kScriptBudget = 800;
std::atomic<int> g_minigame{0}, g_minigame_force{-1};
std::atomic<int> g_script_list_state{0};  // 0 not looked for, 1 found, -1 not on this build
std::atomic<uintptr_t> g_script_list{0};  // the address of the list's head pointer
std::atomic<uint64_t> g_script_walks{0}, g_script_bad{0};
std::atomic<int> g_script_nodes{0};
std::atomic<double> g_script_walk_ms{-1e12};  // the last walk (the review: with no script tick, a load, its answer is not kept)
std::mutex g_script_mx;
std::string g_script_names;  // the running scripts' names as last walked (g_script_mx)
std::string mini_text(int bits) {
    std::string r;
    for (int k = 0; k < 6; ++k)
        if (bits & (1 << k)) r += (r.empty() ? "" : ", ") + std::string(kMiniNames[k]);
    return r.empty() ? "none" : r;
}
// false: something that is not a script's node (the walk ends there)
bool script_walk(uintptr_t node, int depth, int& budget, int& bits, std::string& names) {
    while (node && budget > 0) {
        --budget;
        char nm[73];  // (the review: 72 bytes, the longest minigame's path 54: less to read past a node's end)
        int32_t state = 0;
        uintptr_t next = 0, child = 0;
        if ((node & 7) || node < 0x10000 || !raw(node + 0x70, nm, sizeof(nm) - 1) || !raw(node + 0x30, &state, sizeof(state)) ||
            !raw(node + 8, &next, sizeof(next)) || !raw(node + 0x50, &child, sizeof(child)))
            return false;
        nm[sizeof(nm) - 1] = 0;
        char rawnm[sizeof(nm)];  // (cycle RC1: the name as the game has it, for the list; matched on the lowered copy)
        std::memcpy(rawnm, nm, sizeof(nm));
        size_t n = 0;
        for (; nm[n]; ++n) {
            const unsigned char c = static_cast<unsigned char>(nm[n]);
            if (c < 0x20 || c > 0x7e) return false;
            nm[n] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
        }
        if (n && state) {
            size_t e = n;  // (without an extension)
            for (size_t i = n; i-- > 0 && nm[i] != '/';)
                if (nm[i] == '.') {
                    e = i;
                    break;
                }
            for (int k = 0; k < 6; ++k) {
                const size_t tl = std::strlen(kMiniTails[k]);
                if (e >= tl && !std::memcmp(nm + e - tl, kMiniTails[k], tl)) bits |= 1 << k;
            }
            if (names.size() < 6000) {
                if (!names.empty()) names += ' ';
                names.append(rawnm, n);
                if (depth) names += "@" + std::to_string(depth);  // (a child: its depth)
            }
        }
        if (child && depth < 8 && !script_walk(child, depth + 1, budget, bits, names)) return false;
        node = next;
    }
    return true;
}
void scripts_tick() {  // (a script tick's thread: api::run_on_tick)
    const uintptr_t list = g_script_list.load(std::memory_order_relaxed);
    if (!list) return;
    uintptr_t root = 0;
    int bits = 0, budget = kScriptBudget;
    std::string names;
    if (raw(list, &root, sizeof(root)) && root && !script_walk(root, 0, budget, bits, names)) g_script_bad.fetch_add(1, std::memory_order_relaxed);
    g_script_nodes.store(kScriptBudget - budget, std::memory_order_relaxed);
    g_script_walks.fetch_add(1, std::memory_order_relaxed);
    g_script_walk_ms.store(log::now_ms(), std::memory_order_relaxed);
    if (const int was = g_minigame.exchange(bits, std::memory_order_relaxed); was != bits)
        log::info("[holster] the minigames' scripts running: %s (was %s; %d scripts in the game's list)", mini_text(bits).c_str(), mini_text(was).c_str(),
                  kScriptBudget - budget);
    std::lock_guard lock(g_script_mx);
    g_script_names.swap(names);
}
void script_list_find() {
    unsigned char b[7] = {};
    const uintptr_t fn = anchors::present(anchors::Id::ScriptNamedRunning) ? anchors::addr(anchors::Id::ScriptNamedRunning) : 0;
    if (fn && raw(fn + 0x23, b, sizeof(b)) && b[0] == 0x48 && b[1] == 0x8b && b[2] == 0x35) {  // mov rsi, [rip + disp32]
        int32_t disp = 0;
        std::memcpy(&disp, b + 3, sizeof(disp));
        g_script_list.store(fn + 0x2a + static_cast<intptr_t>(disp), std::memory_order_relaxed);
        g_script_list_state = 1;
        log::info("[holster] the game's script list at RDR.exe+0x%llx: the table games told apart by their scripts",
                  static_cast<unsigned long long>(g_script_list.load() - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
    } else {
        g_script_list_state = -1;
        log::warn("[holster] the game's script list not found on this build: the table games are not told apart by it");
    }
}
// the running scripts with `part` in their name (lower case), up to `cap` characters
std::string scripts_with(const char* part, size_t cap, bool slashes = false) {  // slashes: for the test channel's answer (cycle RD1: it ends at a backslash's escape)
    std::lock_guard lock(g_script_mx);
    std::istringstream in(g_script_names);
    std::string w, r;
    while (in >> w) {  // (round 132: the braces were missing: only the last name was ever given)
        if (r.size() + w.size() + 1 > cap) continue;
        std::string lw = w;  // (the names as the game has them: `part` looked for without case)
        for (char& ch : lw) ch = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch;
        if (slashes)
            for (char& ch : w) ch = ch == '\\' ? '/' : ch;
        if (!part || lw.find(part) != std::string::npos) r += (r.empty() ? "" : " ") + w;
    }
    return r;
}
const uint32_t kSetNextWeapon = 0xBFD6D55F;     // ACTOR_SET_NEXT_WEAPON(actor, eWeapon): its slot's object becomes it (not drawn)
std::atomic<uint64_t> g_sec_switches{0}, g_sec_refused{0};
const uint32_t kPutItemAway = 0x13A63AA7;      // ACTOR_PUT_ITEM_AWAY(actor)
const uint32_t kHolsterWeapon = 0xFE9903CC;    // ACTOR_HOLSTER_WEAPON(actor, animated)
// [Holsters] InstantPutAway (2026-10-31, the off-hand build step 2, R3; the user: "All weapons and items play the games
// holstering animation when let go of, hide this"): the mod's put-aways (HoldToKeep's let-go, a let-go at a holster) are
// the game's instant holster, ACTOR_HOLSTER_WEAPON(actor, 0), not ACTOR_PUT_ITEM_AWAY's clip (r77 h1: the prop left the
// hand over 0.2-0.65 s; the instant one cleared the hand in one tick, no refusal, a second gun kept; r81 gP3: the same
// riding, the horse's walk kept). The carry's own put-away keeps the clip (its placement follows the item's states)
std::atomic<bool> g_instant_put{false};  // (2026-11-03, round 77: off, its step; the game's put-away, its clip's draws hidden)
std::atomic<uint64_t> g_put_holds{0};  // (round 76) instant put-aways whose step was taken back
// (GOAL-doorway-and-poker item 4.6, the review: a let-go's put-away holstered whatever was in hand; the other hand's
// draw queued before it, PUT_WEAPON_IN_HAND then the holster, was put away at once. The fix's review: the game's state
// shows a draw a script tick or more after it, so a let-go an update or two later still saw the gun pressed for) the last
// weapon put in a hand and when: a put-away of another weapon within 1 s of it is refused (holster_frame's thread)
int g_put_in_hand_w = -1;
double g_put_in_hand_ms = -1e12;
void queue_put_in_hand(const uint64_t* args) {
    api::queue_native(kPutWeaponInHand, args, 3, 0, nullptr);
    g_put_in_hand_w = static_cast<int>(args[1]);
    g_put_in_hand_ms = log::now_ms();
}
bool put_in_hand_other(int w) {  // another weapon than w put in a hand within the last second
    return log::now_ms() - g_put_in_hand_ms < 1000.0 && g_put_in_hand_w != w;
}
std::atomic<uint64_t> g_putaway_skipped{0};  // let-gos not put away: another weapon put in hand since the press
void put_item_away(int32_t actor, int weapon = -1) {
    const uint64_t a = static_cast<uint64_t>(static_cast<uint32_t>(actor));
    if (g_instant_put.load(std::memory_order_relaxed)) {
        const uint64_t args[2] = {a, 0ull};
        api::queue_native(kHolsterWeapon, args, 2, 0, nullptr);
    } else {
        body::put_away_leaving(weapon);  // (round 77) its draws on the way to the holster hidden: the clip not seen, John not moved
        const uint64_t args[1] = {a};
        api::queue_native(kPutItemAway, args, 1, 0, nullptr);
    }
}
// SET_PLAYER_MELEE_MODE_SELECTED: the handler (0x140ad7470) takes only its first argument, the flag (the natives DB's
// "player, mode" order is not the handler's): P +0x3bc bit 5, the weapon wheel's Unarmed (FUN_1403f92e0). Any later
// draw into the hand clears it (FUN_14033d6d0, FUN_140341430).
const uint32_t kMeleeModeSelected = 0xAC1285A3;

void holster_frame() {
    {  // (round 143) [Holsters] WheelAtHolster: a pick from the wheel a trigger opened at a holster: once the game has put a
       // new weapon in hand (4 s at most, no grip pressed meanwhile), that holster's weapon and put away into it
        static int a_zone = -1, a_before = -2;
        static double a_until = 0.0, a_pick_ms = 0.0;
        static bool a_lt = false;
        static int x_before = -2;  // (cycle HP30) a cancel: a weapon the game draws within 1 s of it is put away
        static double x_until = 0.0;
        {
            int b = -2;
            if (wheel::take_cancel(&b) && b != -2) x_before = b, x_until = log::now_ms() + 1000.0;
            if (x_before != -2) {
                RdrvrActorState xs{};
                if (log::now_ms() > x_until) {
                    x_before = -2;
                } else if (api::actor_state(&xs) && xs.valid && xs.weapon >= 0 && xs.weapon != x_before) {
                    put_item_away(xs.actor, xs.weapon);
                    log::info("[holster] the wheel at a holster cancelled: the game drew %s at its close: put away",
                              xs.weapon < kWeapons ? kWeaponLabel[xs.weapon] : "an item");
                    x_before = -2;
                }
            }
        }
        static int c_w = -1;  // (cycle HP27) the weapon put away for the pick, checked again once
        static double c_ms = 0.0;
        if (c_w >= 0 && log::now_ms() - c_ms > 700.0) {  // still in hand (the draw's own end ran over the put-away): again
            RdrvrActorState cs{};
            if (api::actor_state(&cs) && cs.valid && cs.weapon == c_w) {
                put_item_away(cs.actor, c_w);
                log::info("[holster] the wheel at a holster: %s still in hand after the put-away: put away again", kWeaponLabel[c_w]);
            }
            c_w = -1;
        }
        int z = -1, before = -2, ctrl = -1;
        if (wheel::take_assign(&z, &before, &ctrl) && before != -2) {  // (the review: not with the weapon before unknown)
            a_zone = z;
            a_before = before;
            // (cycle HP24: a draw came over 1.5 s after the pick: 4 s, the window closed by any grip pressed instead, so a
            // grip draw from another holster is never taken for the pick's)
            a_until = log::now_ms() + 4000.0;
            a_pick_ms = log::now_ms();
            a_lt = false;
        }
        if (a_zone >= 0 && (hands::get(0).grip > 0.6f || hands::get(1).grip > 0.6f)) {
            log::info("[holster] the wheel at a holster: a grip pressed before the pick's weapon came: nothing changed");
            a_zone = -1;
        }
        if (a_zone >= 0) {
            RdrvrActorState st{};
            const bool have = api::actor_state(&st) && st.valid;
            if (have && st.weapon >= 0 && st.weapon < kWeapons && st.weapon != a_before) {
                const int w = st.weapon;
                // (the review) only a weapon this holster can draw (its slots, or any with AnyWeapon): else left in hand
                if (!g_any_weapon.load(std::memory_order_relaxed) && !weapon_fits(g_zones[a_zone], w, st.equip_slot[w])) {
                    log::info("[holster] the wheel at a holster: %s does not go in the %s (AnyWeapon off): left in hand", kWeaponLabel[w], g_zones[a_zone].key);
                } else {
                    set_zone_weapon(a_zone, w, true);
                    xinput::set_lt_override(0);  // (cycle HP27: LT still held drew it again after the put-away)
                    put_item_away(st.actor, w);
                    c_w = w;
                    c_ms = log::now_ms();
                    g_wheel_assigned.fetch_add(1, std::memory_order_relaxed);
                    log::info("[holster] the wheel at a holster: %s into the %s (its weapon now), put away", kWeaponLabel[w], g_zones[a_zone].key);
                }
                a_zone = -1;
            } else if (!a_lt && log::now_ms() - a_pick_ms > 400.0) {
                // (cycles HP24, HP25: from empty hands the game often only selects the pick, drawing it at the next trigger:
                // seconds later) LT pressed for it, the game's own draw of the selected weapon (the aim's)
                xinput::set_lt_override(200);
                a_lt = true;
                g_wheel_lt_draws.fetch_add(1, std::memory_order_relaxed);
            } else if (log::now_ms() > a_until) {
                g_wheel_assign_lost.fetch_add(1, std::memory_order_relaxed);
                log::info("[holster] the wheel at a holster: no new weapon in hand within 4 s (weapon %d; the pick an empty place?): nothing changed",
                          have ? st.weapon : -1);
                a_zone = -1;
            }
        }
    }
    static bool grip_down[2] = {false, false}, pending[2] = {false, false};
    static int pending_w[2] = {-1, -1};  // (item 4.6) the weapon in hand at the press that armed `pending`
    static int zone_in[2] = {-1, -1}, press_zone[2] = {-1, -1};
    static uint64_t draw_req[2] = {0, 0};  // a queued GET_WEAPON_EQUIPPED, answered on a script tick
    static double draw_ms[2] = {0, 0};
    static int draw_slot[2] = {-1, -1};
    static bool round_held[2] = {false, false}, two_hold[2] = {false, false}, trig_down = false;
    static int round_n[2] = {0, 0};               // the rounds that hand holds (RoundsPerGrab), while round_held
    // [Hands] DualWieldCrossReload (2026-10-27): the gun the held rounds are for (0 the gun in hand, 1 the second gun),
    // and whether a hand holding a gun took them (the other gun's rounds: its own gun's fire held back meanwhile)
    static int round_tgt[2] = {0, 0};
    static bool round_x[2] = {false, false};
    // [Reload] PistolMagazines (2026-10-28): the held is a magazine (the Mauser's: a stripper clip), round_n its rounds
    static bool round_mag[2] = {false, false};
    // (the review) a let-go's rounds for the copy that waited for its spare to settle (dual::load_copy_rounds -1): the
    // rounds and since when, tried each update for a second
    static int defer_n[2] = {0, 0};
    static double defer_ms[2] = {0.0, 0.0};
    static double round_next_ms[2] = {0.0, 0.0};  // RoundsGoIn=one: the next round's time at the ring
    static int recent_in[2] = {0, 0};             // the rounds last put in at the ring and when: the game's count may not
    static double recent_ms[2] = {-1e12, -1e12};  // show them yet (a queued native), so a let-go just after counts them
    static bool two_paused[2] = {false, false};  // [Reload] ManualBreak: the two-handed hold waiting while the gun is open
    static bool sec_pending[2] = {false, false};  // [Hands] DualWield: the second gun's hand let go here puts it back
    // the put-back arms once the hand has left the zone it took the second gun at (round 10: a grip again at the same
    // holster a second after the take put it straight back)
    static bool sec_armed[2] = {false, false};
    static int sec_zone[2] = {-2, -2};  // the zone at the first frame the second gun is out (-1: none); -2: no second gun
    static int sec_from[2] = {-1, -1};  // the holster the second gun (or the copy) was taken at, set at its begin (ShowGuns' hide)
    // [Hands] CarryByForegrip: the gun hand's press that may hand the long gun to the front hand (at its let-go); the gun
    // hand's grab point at the hand-over in the drawn gun's frame (carry_grip_g: the take-back is near it on the carried
    // gun as placed) and in the front controller's target frame (carry_grip_c: until the gun is placed as a carry); the
    // carried gun's holster (drawn_from then); after a take-back, the front hand still on the foregrip is two-handed again (1 s)
    static bool carry_press[2] = {false, false}, retwo[2] = {false, false};
    static bool carry_cover[2] = {false, false};      // in cover at some time during the hand-over's press
    static double carry_press_ms[2] = {0.0, 0.0};     // its press
    static float carry_grip_c[3] = {0.0f, 0.0f, 0.0f};
    static float carry_grip_g[3] = {0.0f, 0.0f, 0.0f};
    static bool carry_grip_g_ok = false;
    static int carry_from = -1;
    static double retwo_ms = 0.0;
    // [Holsters] HoldToKeep: the gun each controller's grip keeps (0 none, 1 the gun in hand, 2 the second gun or the
    // copy), its eWeapon (-1: a slot's draw still asked), the holster it came from (-1 not known: the game's own draw),
    // when it was taken and whether it has been seen in the hand (or out, the second) since; since when each grip is let
    // go (-1 held); the hand-over's last try; since when the frame end has had no body (-1)
    static int keep[2] = {0, 0}, keep_w[2] = {-1, -1}, keep_from[2] = {-1, -1};
    static double game_throw_ms[2] = {-1e12, -1e12};  // (C1) hand h let go of the game's own throw then (-1e12: none)
    static double hold_take_ms = -1e12;               // (C17) the gun in hand's last hold take (log::now_ms)
    static double hold_take_ms_hold = -1e12;          // (C17b) the last take by a hold alone (not the nothing-in-hand reset)
    static int game_throw_w[2] = {-1, -1};            // (C1) and its eWeapon
    static double keep_ms[2] = {0.0, 0.0}, up_since[2] = {-1.0, -1.0}, keep_try_ms[2] = {-1e12, -1e12}, keep_out = -1.0;
    static bool keep_seen[2] = {false, false};
    static const char* keep_why[2] = {nullptr, nullptr};  // the let-go's wait, as last logged (each new reason logged once)
    static bool loose_held[2] = {false, false};  // made loose (the menu, the body lost) with its grip held: held again if still held
    // an animated draw (its clip: the put-away waits kHoldDrawSettleMs from the gun in hand), the hand-over's failed tries,
    // when each hand last let go of rounds, the last hold's put-away, the last busy state (Dead Eye, a menu, a scene), the
    // last change of the seat (mounted or not), and since when the item in hand is the one it is
    static bool keep_anim[2] = {false, false};
    static int keep_try_n[2] = {0, 0};
    static double rounds_let_ms[2] = {-1e12, -1e12}, hold_put_ms = -1e12, busy_last = -1e12, seat_ms = -1e12, hand_since = 0.0;
    static bool seat_was = false;
    static int hand_was = -2;
    static int draw_keep[2] = {-1, -1};  // the controller holding each controller's slot draw (draw_req), -1 none
    static double last_ms = 0;
    static double flip_since = 0;  // AimWhenRaised's debounce
    const double now_ms = log::now_ms();
    const float dt = last_ms > 0 ? static_cast<float>((now_ms - last_ms) * 0.001) : 0.0f;
    last_ms = now_ms;
    {  // the game's gate (a minigame runs, the game holds the player's control): polled every frame this runs, before any
       // return below (round 125's review)
        static uint64_t gate_req[4] = {0, 0, 0, 0};
        static double gate_ms[4] = {-1e9, -1e9, -1e9, -1e9};
        static bool gate_val[4] = {false, true, false, false};  // a minigame running; the player controllable; in a gringo; on a train
        static int gringo_step = 0;  // 0: GET_CURRENT_GRINGO asked; 1: IS_GRINGO_VALID of its answer
        static double ans_ms[4] = {-1e12, -1e12, -1e12, -1e12};  // each one's last answer
        // the control held in a gringo, in a gringo on a train: since when (ms; -1 not), last true, and the bit made of it
        static double table_since = -1.0, seat_since = -1.0, table_true = -1e12, seat_true = -1e12;
        static bool table_on = false, seat_on = false;
        RdrvrActorState gst{};
        const uint64_t gactor = api::actor_state(&gst) && gst.valid ? static_cast<uint64_t>(static_cast<uint32_t>(gst.actor)) : 0;
        for (int i = 0; i < 4; ++i) {
            RdrvrNativeResult r{};
            if (gate_req[i] && api::wait_native(gate_req[i], &r, 0)) {
                gate_req[i] = 0;
                const uint64_t v = r.value & 0xffffffffull;
                if (i == 2 && gringo_step == 0) {
                    if (v) {  // a gringo: is it valid (the scripts' own test, IS_GRINGO_VALID(GET_CURRENT_GRINGO(actor)))
                        gate_req[2] = api::queue_native(kIsGringoValid, &v, 1, 0, nullptr);
                        gringo_step = 1;
                    } else {
                        gate_val[2] = false;
                        ans_ms[2] = now_ms;
                    }
                } else if (i == 2) {
                    gate_val[2] = v != 0;
                    gringo_step = 0;
                    ans_ms[2] = now_ms;
                } else {
                    gate_val[i] = v != 0;
                    ans_ms[i] = now_ms;
                }
            } else if (gate_req[i] && now_ms - gate_ms[i] > 1000.0) {  // (no script tick: a load, the pause menu)
                api::cancel_op(gate_req[i]);
                gate_req[i] = 0;
                if (i == 2) gringo_step = 0;
            }
            if (!gate_req[i] && now_ms - gate_ms[i] >= 250.0) {
                const uint64_t player = 0;
                const uint64_t a2[2] = {gactor, 0};
                if (i == 0) {
                    gate_req[i] = api::queue_native(kIsMinigameRunning, nullptr, 0, 0, nullptr);
                } else if (i == 1) {
                    gate_req[i] = api::queue_native(kIsPlayerControllable, &player, 1, 0, nullptr);
                } else if (!gactor) {
                    gate_val[i] = false;  // (no actor: neither)
                    ans_ms[i] = now_ms;
                } else if (i == 2) {
                    gate_req[i] = api::queue_native(kGetCurrentGringo, a2, 1, 0, nullptr);
                    gringo_step = 0;
                } else {
                    gate_req[i] = api::queue_native(kIsActorOnTrain, a2, 2, 0, nullptr);
                }
                gate_ms[i] = now_ms;
            }
        }
        {  // (round 130) which minigame's script runs: the list walked on the next script tick, four times a second
            static double scripts_ms = -1e9;
            if (now_ms - scripts_ms >= 250.0) {
                scripts_ms = now_ms;
                if (g_script_list_state.load(std::memory_order_relaxed) == 0) script_list_find();
                if (g_script_list.load(std::memory_order_relaxed)) api::run_on_tick(scripts_tick);
            }
        }
        int gate_now = (gate_val[0] ? 1 : 0) | (gate_val[1] ? 0 : 2) | (gate_val[2] ? 4 : 0) | (gate_val[3] ? 8 : 0);
        // (the review) the holds run only over fresh answers (no script tick, the pause menu: the last answers stand, and the
        // hold neither runs on nor ends), and end only once their condition has been false for kHoldOffMs (one poll's
        // "no actor", a native answered 0: not the game over)
        const bool fresh = now_ms - std::fmin(ans_ms[1], std::fmin(ans_ms[2], ans_ms[3])) < 700.0;
        auto hold = [&](bool cond, double hold_ms, double& since, double& last_true, bool& on) {
            if (!fresh) {
                if (since >= 0.0) since += static_cast<double>(dt) * 1000.0;
                return;
            }
            if (cond) {
                last_true = now_ms;
                if (since < 0.0) since = now_ms;
                if (now_ms - since >= hold_ms) on = true;
            } else if (now_ms - last_true > kHoldOffMs) {
                since = -1.0;
                on = false;
            }
        };
        hold((gate_now & 6) == 6, kTableHoldMs, table_since, table_true, table_on);
        // (round 129, the user on the train's seat: "Camera is still too far forward"; their log: on a train for 39 s, never in
        // a gringo: the seat's gringo is not GET_CURRENT_GRINGO's) the seat by John himself: on a train with his head under
        // 1.38 m over his place (1.21-1.25 seated at the tables, 1.56-1.60 standing) and not crouching; over 1.48 he stands
        // (the review) not in cover; not lying (a fall, knocked down: his head under 0.6 m), nor with his head unread
        static bool seat_low = false;
        if (gactor && gst.head_valid) {
            const float hh = gst.head_pos[1] - gst.pos[1];
            if (hh > 0.6f && hh < 1.38f) seat_low = true;
            else if (!(hh > 0.6f && hh < 1.48f)) seat_low = false;
        } else {
            seat_low = false;
        }
        const bool seat_cond = (gate_now & 8) != 0 &&
                               ((gate_now & 4) != 0 || (seat_low && !(gst.flags & (RDRVR_ACTOR_CROUCHING | RDRVR_ACTOR_IN_COVER))));
        hold(seat_cond, kSeatHoldMs, seat_since, seat_true, seat_on);
        gate_now |= (table_on ? 16 : 0) | (seat_on ? 32 : 0);
        g_game_gate_ms.store(now_ms, std::memory_order_relaxed);
        if (const int was = g_game_gate.load(std::memory_order_relaxed); gate_now != was) {
            if ((gate_now & 3) && !(was & 3)) g_gate_closes.fetch_add(1, std::memory_order_relaxed);
            log::info("[holster] the game's gate %d -> %d (a minigame %d, the control held %d, in a gringo %d, on a train %d): the holsters %s%s", was,
                      gate_now, gate_now & 1, (gate_now >> 1) & 1, (gate_now >> 2) & 1, (gate_now >> 3) & 1,
                      gate_now & 1 ? "closed (a minigame)" : gate_now & 2 ? (gate_table(gate_now) ? "closed to draws (a table game)" : "closed to draws") : "open",
                      gate_seated(gate_now) && !gate_table(gate_now) ? "; seated on a train" : "");
            if (gate_table(gate_now) && !gate_table(was))  // (round 130) which one, and the names it was told by (the next log's proof)
                log::info("[holster] a table game: by the scripts running %s (the list %d, %d scripts) | the minigames' scripts: %s",
                          mini_text(g_minigame.load(std::memory_order_relaxed)).c_str(), g_script_list_state.load(std::memory_order_relaxed),
                          g_script_nodes.load(std::memory_order_relaxed), scripts_with("mini", 700).c_str());
            g_game_gate.store(gate_now, std::memory_order_relaxed);
        }
    }
    body::table_clip_frame();  // (round 145) poker's sit-down: the players drawn at John's head
    horseshoe::frame();  // (round 133) [Gestures] HorseshoeThrow: the shoe thrown by the hand while horseshoes' script runs
    const double hold_letgo = g_hold_letgo.load(std::memory_order_relaxed);
    for (int h = 0; h < 2; ++h) {
        const float g = hands::get(h).grip;
        const bool was = grip_down[h];
        grip_down[h] = g > g_grip_on ? true : g < g_grip_on - 0.2f ? false : was;
        // [Holsters] HoldToKeep: a kept gun's grip back before the let-go counted: a blip, the gun kept
        if (grip_down[h] && up_since[h] >= 0.0 && keep[h] != 0) {
            const double len = now_ms - up_since[h];
            if (len < hold_letgo) {
                g_hold_blips.fetch_add(1, std::memory_order_relaxed);
                g_hold_blip_ms.store(static_cast<float>(len), std::memory_order_relaxed);
                log::info("[holster] %s hand's grip let go %.0f ms (under the %.0f ms HoldToKeep counts): a blip, the %s kept", h ? "right" : "left", len,
                          hold_letgo, keep[h] == 2 ? "second gun" : "gun");
            } else {
                log::info("[holster] %s hand's grip held again after %.0f ms (its let-go was waiting): the %s kept", h ? "right" : "left", len,
                          keep[h] == 2 ? "second gun" : "gun");
            }
        }
        if (grip_down[h]) up_since[h] = -1.0;
        else if (up_since[h] < 0.0) up_since[h] = now_ms;
    }
    body::BodyPoints bp;
    RdrvrActorState st{};
    const uintptr_t actor = holster_player_actor();
    // (2026-11-03, round 75, the user: letting go of a sidearm "moves you slightly", the whole view sliding and staying, to the
    // left from the right holster and back; not seen in the simulator, cycles N39, N40) a let-go's put-away: John's place
    // and heading then and 1 s after, logged (his right +, forward +)
    static struct {
        double ms = -1.0;
        float p[3] = {}, h = 0.0f;
        const char* what = "";
    } letgo_tr;
    // (2026-11-03, round 76: InstantPutAway=0 stopped it; the user: "Keep instant, undo the slide") the game's instant holster
    // steps John 10-20 cm sideways (from the right holster to his left, from the left to his right): for 0.7 s after an
    // instant put-away, his own stick still, he is put back where he stood (TELEPORT_ACTOR, at most every 40 ms) whenever he
    // is 1.5 cm to 0.6 m from it on the level
    static struct {
        double ms = -1.0, sent_ms = -1.0;
        float p[3] = {};
        int sends = 0;
    } put_hold;
    auto hold_place = [&](const RdrvrActorState& s0, double t) {
        // (on foot only: a horse, a wagon or cover moves him itself)
        if (!g_instant_put.load(std::memory_order_relaxed) || !s0.valid ||
            (s0.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_VEHICLE | RDRVR_ACTOR_IN_COVER)))
            return;
        put_hold.ms = t;
        put_hold.sent_ms = -1.0;
        put_hold.sends = 0;
        std::memcpy(put_hold.p, s0.pos, sizeof(put_hold.p));
    };
    static double points_ms = 0;  // the last frame the body's points were read
    const bool on = g_holsters.load(std::memory_order_relaxed) && pose::anchor_active();
    const bool pts = on && body::body_points(&bp);
    if (pts) points_ms = now_ms;
    // (r313 B15: a 400 ms hitch, a new model's shader made, read as out of first person: the off hand's boom bait put back)
    // missed reads in a row: one frame after a long frame is a hitch, not a cutscene
    static int missed_reads = 0;
    missed_reads = pts ? 0 : missed_reads + 1;
    turret_drive(pts ? &bp : nullptr, actor, now_ms);  // [Hands] MountedGunFirstPerson (experimental)
    if (on && !pts && (now_ms - points_ms < 100.0 || missed_reads < 3)) return;  // a missed read: everything kept as it was
    if (put_hold.ms > 0.0 && api::actor_state(&st) && st.valid && st.actor) {
        xinput::PadState ps{};
        const bool moving = controls::pad(&ps) && std::sqrt(static_cast<float>(ps.lx) * ps.lx + static_cast<float>(ps.ly) * ps.ly) / 32767.0f > 0.24f;
        const float dx = st.pos[0] - put_hold.p[0], dz = st.pos[2] - put_hold.p[2], dd = std::sqrt(dx * dx + dz * dz);
        const bool off_foot = (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_VEHICLE | RDRVR_ACTOR_IN_COVER)) != 0;
        if (now_ms - put_hold.ms > 700.0 || moving || off_foot || menu::visible() || std::fabs(st.pos[1] - put_hold.p[1]) > 0.10f || dd > 0.6f) {
            if (put_hold.sends) g_put_holds.fetch_add(1, std::memory_order_relaxed);
            if (put_hold.sends)
                log::info("[holster] the instant put-away's step taken back (%d teleport%s)%s", put_hold.sends, put_hold.sends == 1 ? "" : "s",
                          moving ? ", until the stick moved" : "");
            put_hold.ms = -1.0;
        } else if (dd > 0.015f && now_ms - put_hold.sent_ms >= 40.0) {
            const float p[3] = {put_hold.p[0], st.pos[1], put_hold.p[2]};
            const uint64_t args[5] = {static_cast<uint32_t>(st.actor), 0, 0, 0, 0};
            api::queue_native(0x2D54B916u, args, 5, 2, p);  // TELEPORT_ACTOR(the player, &p, 0, 0, 0): args[1] the place (pose.cpp's rs_snap)
            put_hold.sent_ms = now_ms;
            ++put_hold.sends;
        }
    }
    if (pts && api::actor_state(&st) && st.valid && letgo_tr.ms > 0.0 && now_ms - letgo_tr.ms >= 1000.0) {
        const float hr = letgo_tr.h * 0.0174532925f, dx = st.pos[0] - letgo_tr.p[0], dz = st.pos[2] - letgo_tr.p[2];
        // (GET_HEADING: forward (-sin h, cos h) in x, z as the game's actors face; right its turn)
        const float fwd = -std::sin(hr) * dx + std::cos(hr) * dz, rgt = std::cos(hr) * dx + std::sin(hr) * dz;
        log::info("[holster] 1 s after the %s's put-away: John moved %.3f m (right %+.3f, forward %+.3f, up %+.3f), turned %+.1f deg", letgo_tr.what,
                  std::sqrt(dx * dx + dz * dz), rgt, fwd, st.pos[1] - letgo_tr.p[1], std::remainder(st.heading_deg - letgo_tr.h, 360.0f));
        letgo_tr.ms = -1.0;
    }
    if (!pts || !api::actor_state(&st) || !st.actor || !actor) {
        for (int h = 0; h < 2; ++h) {
            g_consumed[h] = false;
            pending[h] = false;
            zone_in[h] = -1;
            draw_req[h] = 0;
            round_held[h] = false;
            round_mag[h] = false;
            round_x[h] = false;
            g_cross_fire_hold[h] = false;
            g_rounds_for_sec.store(false, std::memory_order_relaxed);  // (S3, the review)
            two_hold[h] = false;
            g_wanted[h] = false;
            carry_press[h] = false;
        }
        g_raised.store(false, std::memory_order_relaxed);  // no aim stance held for a gun the holsters no longer see
        g_gun_at_zone.store(false, std::memory_order_relaxed);
        flip_since = 0;
        g_two_blend = 0.0f;
        round_draw::set_held(-1, -1);
        for (int h = 0; h < 2; ++h) {  // ([Reload] RoundSource's status: nothing held, the palms not known)
            g_held_n[h].store(0, std::memory_order_relaxed);
            g_held_w[h].store(-1, std::memory_order_relaxed);
            g_palm_y[h].store(-2.0f, std::memory_order_relaxed);
            g_near_d[h].store(-1.0f, std::memory_order_relaxed);
        }
        held_prop::want(0, nullptr, nullptr);  // out of first person: the shell goes
        held_prop::want_pool(nullptr, 0, nullptr, nullptr, 0, 0, nullptr);  // and the shells' pool (2026-10-27), every object
        held_prop::want(1, nullptr, nullptr);  // and the copy's own model
        for (int i = 0; i < 4; ++i) held_prop::want(held_prop::kHolsterSlot + i, nullptr, nullptr);  // and the holsters' guns
        for (int i = 0; i < 2; ++i) held_prop::want(held_prop::kModelSlot + i, nullptr, nullptr);    // and their models
        for (int k = 0; k < 3; ++k) held_prop::want(held_prop::kBeltPropSlot + k, nullptr, nullptr);  // and the belt's props (item 2)
        // (the review) the default model's rewrite: all collapsed while its object lives, then off
        body::set_belt_model_pieces(held_prop::slot_holds(held_prop::kBeltPropSlot + 1, "player_default") ? 1u : 0u);
        offhand::frame(nullptr, nullptr, nullptr, -1, false);  // and the off hand's item (put back)
        held_prop::frame(0);
        // [Holsters] HoldToKeep: the kept guns wait (a missed read); over half a second without the body they are loose
        // (below, as the frame end has it again)
        if (keep_out < 0.0) keep_out = now_ms;
        for (int h = 0; h < 2; ++h) g_keeping[h].store(false, std::memory_order_relaxed);
        std::lock_guard lock(g_hdiag_mutex);
        g_hdiag.valid = false;
        g_markers_valid = false;
        g_hl_valid = false;
        return;
    }
    reload::watch_reload_flag(st);  // (2026-10-27, DualWieldCrossReload's review: the copy's spare writes wait on the game's reload)
    if (keep_out >= 0.0) {  // [Holsters] HoldToKeep: back after a long loss (a cutscene, a load, third person): the held guns loose
        if (now_ms - keep_out > 500.0 && (keep[0] != 0 || keep[1] != 0)) {
            log::info("[holster] the body back after %.0f ms: the held guns loose (HoldToKeep: each stays until its grip is pressed and let go)",
                      now_ms - keep_out);
            g_hold_loose.fetch_add(1, std::memory_order_relaxed);
            for (int h = 0; h < 2; ++h) loose_held[h] = keep[h] != 0 && grip_down[h];
            keep[0] = keep[1] = 0;
        }
        keep_out = -1.0;
    }
    Zone zones[kZones];
    {
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(zones, g_zones, sizeof(zones));
    }
    // run 10 item 10, AnyWeapon; 2026-10-13 (the user: "change the option to have any weapon in any holster to only apply
    // to the gun holsters"): the hips and the backs take any weapon; the knife's, the satchel's and the lasso's their own
    // items, the chest the rounds
    const bool any_item = g_any_weapon.load(std::memory_order_relaxed);
    zones[kAmmoZone].weapon = -1;
    for (Zone& z : zones) {
        if (z.weapon == kRounds) z.weapon = -1;  // (an older AnyWeapon choice: the rounds are the chest's)
        // (an older choice of another kind at the knife's, the satchel's or the lasso's: their own items only)
        if (z.weapon >= 0 && z.weapon < kWeapons && !(any_item && gun_zone(z))) {
            if (!weapon_fits(z, z.weapon, st.equip_slot[z.weapon]) && st.owned_tick) z.weapon = -1;
        }
    }
    auto zone_rounds = [&](int z) { return z == kAmmoZone; };
    auto any_here = [&](int z) { return any_item && z >= 0 && z < kZones && gun_zone(zones[z]); };
    const bool lefty = controls::left_handed() && g_mirror_lefty.load(std::memory_order_relaxed);  // (the layout mirrored)
    const float hd = pose::torso_heading_deg() * 0.0174532925f;  // run 9: the horse's, not the ride's turned view
    const float fwd[3] = {-std::sin(hd), 0.0f, -std::cos(hd)}, right[3] = {std::cos(hd), 0.0f, -std::sin(hd)}, up[3] = {0, 1, 0};
    // the holsters' own frame: the body's, or turned with the headset ([Holsters] TurnWithHead, below)
    float hr[3] = {right[0], right[1], right[2]}, hf[3] = {fwd[0], fwd[1], fwd[2]};
    float zp[kZones][3], bases[kZones][3];
    bool zok[kZones];
    // 2026-10-13 (the user: the left ring "still moves around. When I move the ring, the holster moves. When I move the
    // holster, the ring moves"; "Two options should be selectable. One for the visual holster and the holster ring to
    // always be at the same spot. One for them to be moved seperately"): every zone starts from its bone as the game
    // hangs it, without a belt piece's move (the drawn bone has it); [Holsters] RingsFollowHolsters then puts the move on
    // the zones of moved pieces (below). The LeftHip with the mirrored left holster has its own offset (LeftHipRingOffset):
    // it was the RightHip's mirrored, so the right ring's and the right holster's moves moved it
    static const int kBonePiece[body::kHolsterBones] = {0, 4, 2, 1, -1, -1, 3};  // pistol, rifle, thrower, melee, the chest, the lower back, the lasso
    auto zoff = [&](int z, int k) { return zone_on_copy(zones[z]) ? zones[z].off_copy[k] : zones[z].off[k]; };
    // the twins' mirror (and the left-handed layout's) through the pelvis as drawn: the root bone sways against the body
    // (r12_twinbase: 6 cm sideways standing still), and a twin mirrored through it went twice that (the user's "Left
    // sidearm ring still moves around")
    const float* mid = bp.bone_ok[body::kPelvisPoint] ? bp.bone[body::kPelvisPoint] : bp.root;
    for (int z = 0; z < kZones; ++z) {
        // the wheel mode (run 8 item 5): no holster draws or put-aways, the chest's rounds kept
        zok[z] = zones[z].enabled && bp.bone_ok[zones[z].bone] && (zone_rounds(z) || !wheel::wheel_mode());
        float base[3] = {bp.bone[zones[z].bone][0], bp.bone[zones[z].bone][1], bp.bone[zones[z].bone][2]};
        if (body::own_pieces() && zones[z].bone >= 0 && zones[z].bone < body::kHolsterBones && kBonePiece[zones[z].bone] >= 0) {
            float piv[3], want[3], Rw[9];  // the drawn bone p = want + Rw (p0 - piv): p0 = piv + Rw^T (p - want)
            if (body::piece_rigid(kBonePiece[zones[z].bone], piv, want, Rw)) {
                const float q[3] = {base[0] - want[0], base[1] - want[1], base[2] - want[2]};
                for (int r = 0; r < 3; ++r) base[r] = piv[r] + Rw[r] * q[0] + Rw[3 + r] * q[1] + Rw[6 + r] * q[2];
            }
        }
        if (zones[z].mirror) {  // the twin's bone: mirrored across the body's middle
            const float d = (base[0] - mid[0]) * right[0] + (base[2] - mid[2]) * right[2];
            for (int k = 0; k < 3; ++k) base[k] -= 2.0f * d * right[k];
        }
        std::memcpy(bases[z], base, sizeof(base));
        for (int k = 0; k < 3; ++k)
            zp[z][k] = base[k] + right[k] * zoff(z, 0) + up[k] * zoff(z, 1) + fwd[k] * zoff(z, 2);
        if (lefty) {  // the whole layout mirrored across the body's middle: the sidearm on the left hip
            const float d = (zp[z][0] - mid[0]) * right[0] + (zp[z][2] - mid[2]) * right[2];
            for (int k = 0; k < 3; ++k) zp[z][k] -= 2.0f * d * right[k];
        }
    }
    {  // [Holsters] Anchor=headset: every zone moved with the neck's offset since recentre (in the game camera's rows:
       // the camera is the neutral head, level, facing the body), so the holsters stay with the player when the drawn
       // body does not; with BodyFollowsHead the body already moves sideways with the head: only the height then
        float no[3], sh[3] = {0, 0, 0};
        const bool seated = (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_IN_COVER)) != 0;
        // run 10 item 4 (the user: "after in game cutscenes holsters are wherever they were when the cutscene ended, one
        // time being across the road"): the frame's camera is the anchored one only near John (the neutral head over the
        // root); a script's or a cutscene's camera elsewhere: the anchor neither followed nor its places taken
        bool cam_here = false;
        if (bp.cam_ok) {
            const float* c = bp.cam + 12;
            const float dx = c[0] - bp.root[0], dz = c[2] - bp.root[2], dy = c[1] - bp.root[1];
            cam_here = dx * dx + dz * dz < 0.6f * 0.6f && dy > -0.3f && dy < 1.4f;
            if (!cam_here && g_anchor.load(std::memory_order_relaxed) == 1 && !seated) g_place_offcam.fetch_add(1, std::memory_order_relaxed);
        }
        const bool use = g_anchor.load(std::memory_order_relaxed) == 1 && !seated && cam_here && camera_lever::neck_offset(no);
        // run 10 item 1 (the user: "after getting off horse, holsters are too high"): the places are the zones relative to
        // the neutral head, so a dismount or a cutscene keeps them; before, each frame without the anchor (riding, a
        // cutscene) took them again on the next, inside the dismount's animation (the right hip 0.53-0.70 m below the head
        // against 0.75-0.80). Taken again only after a recentre or when the anchor is newly chosen.
        static int was_choice = 0;  // the anchor's setting at the last update (headset chosen anew: the places taken again)
        const int choice = g_anchor.load(std::memory_order_relaxed);
        static uint32_t take_gen = 0;  // bumped when the setting turns to headset
        if (choice == 1 && was_choice != 1) ++take_gen;
        was_choice = choice;
        static int was_anchor = 0;  // the last update used the headset anchor
        if (use) {
            // the zones' places relative to the neutral head (the camera), in the body's yaw frame: taken over 30 frames
            // standing, after the anchor is chosen and after each recentre; then the zones rigid to the neck
            static float acc[kZones][3], rel[kZones][3];
            static int rel_n = -1;  // frames averaged; -1 none yet
            static bool rel_ok = false;
            static uint32_t rel_gen = ~0u;
            const float* cp = bp.cam + 12;
            const uint32_t gen = camera_lever::recentre_gen() + take_gen * 0x10000u;
            const bool standing = !(st.flags & RDRVR_ACTOR_CROUCHING);
            if (gen != rel_gen) {
                rel_gen = gen;
                rel_n = 0;
                rel_ok = false;  // the body's places until the new ones are taken
                std::memset(acc, 0, sizeof(acc));
            }
            // John settled: the right hip against the camera and the camera's height each moving under 0.2 and 0.1 m/s
            // for a quarter of a second (a dismount, a stand from a crouch, the eye height still easing: not yet)
            // (judged over 100 ms windows: this runs faster than the frames, a frame's step over a millisecond is no speed)
            static float last_hip[3] = {0, 0, 0}, last_cy = 0.0f;
            static double pl_ms = 0.0, still_since = -1.0;
            static bool still = false;
            const double now = log::now_ms();
            {
                const float h[3] = {bases[0][0] - cp[0], bases[0][1] - cp[1], bases[0][2] - cp[2]};
                const double pdt = (now - pl_ms) * 0.001;
                if (pdt >= 0.1 || pdt < 0.0) {
                    if (pdt < 0.5 && pdt > 0.0) {
                        const float dh = std::sqrt((h[0] - last_hip[0]) * (h[0] - last_hip[0]) + (h[1] - last_hip[1]) * (h[1] - last_hip[1]) +
                                                   (h[2] - last_hip[2]) * (h[2] - last_hip[2]));
                        still = dh / pdt < 0.2 && std::fabs((cp[1] - bp.root[1]) - last_cy) / pdt < 0.1;  // over the root: a slope settles
                    } else {
                        still = false;  // a stall: judged again from here
                    }
                    std::memcpy(last_hip, h, sizeof(h));
                    last_cy = cp[1] - bp.root[1];
                    pl_ms = now;
                }
                if (!still || !standing) still_since = -1.0;
                else if (still_since < 0.0) still_since = now;
            }
            const bool settled = still_since >= 0.0 && now - still_since >= 250.0;
            if (rel_n >= 0 && rel_n < 30 && !settled) g_place_waits.fetch_add(1, std::memory_order_relaxed);
            if (rel_n >= 0 && rel_n < 30 && standing && settled) {
                for (int z = 0; z < kZones; ++z) {
                    // (2026-10-13: the bases without the belt pieces' moves, above: taken with them, the move counted
                    // twice, the user's "Everytime I load in, the holster positions are way off")
                    const float d[3] = {bases[z][0] - cp[0], bases[z][1] - cp[1], bases[z][2] - cp[2]};
                    acc[z][0] += d[0] * right[0] + d[2] * right[2];
                    acc[z][1] += d[1];
                    acc[z][2] += d[0] * fwd[0] + d[2] * fwd[2];
                }
                if (++rel_n == 30) {
                    for (int z = 0; z < kZones; ++z)
                        for (int k = 0; k < 3; ++k) rel[z][k] = acc[z][k] / 30.0f;
                    rel_ok = true;
                    g_place_takes.fetch_add(1, std::memory_order_relaxed);
                    {
                        std::lock_guard lk(g_zone_mutex);
                        std::memcpy(g_place_hip, rel[0], sizeof(g_place_hip));
                    }
                    log::info("[holster] the headset anchor's places taken (the right hip %.2f m right, %.2f m below the head, %.2f m ahead)",
                              rel[0][0], -rel[0][1], rel[0][2]);
                }
            }
            {  // [Holsters] TurnWithHead: the frame turned by the headset's yaw since recentre, past the dead zone
                static float zyaw = 0.0f;
                static uint32_t yaw_gen = ~0u;
                // 2026-10-27 ([XR] BodyFollowsTurn): the body turned by the user's own turn (its heading, hd, carries it): the
                // frame turned back by as much, so it stays where it was in the world (the head's yaw over the body drops
                // by the same: no swing of the holsters). The follow's own steps only (the review: a recentre's correction
                // of the total never fed in). Off, they never change: as before
                static float bt_last = 0.0f;
                const float bt_now = pose::body_turn_steps_deg();
                if (bt_now != bt_last) zyaw = std::remainder(zyaw - std::remainder(bt_now - bt_last, 360.0f), 360.0f);
                bt_last = bt_now;
                if (gen != yaw_gen || was_anchor != 1) {  // a recentre, or the anchor taken again: facing the body
                    yaw_gen = gen;
                    zyaw = 0.0f;
                }
                float hy = 0.0f;
                if (!g_turn_head.load(std::memory_order_relaxed)) {
                    zyaw = 0.0f;
                } else if (camera_lever::head_yaw_deg(&hy)) {  // (no head pose this frame: kept where they point)
                    // ([XR] BodyFollowsTurn, the review) the head over the body as commanded (hd's frame, zyaw's): the
                    // origin's yaw has taken only what the camera shows of the follow yet (off: 0, nothing changed)
                    const float bt_pend = std::remainder(pose::body_turn_total_deg() - camera_lever::body_turn_shown(), 360.0f);
                    if (bt_pend != 0.0f) hy -= bt_pend;
                    // [Holsters] TurnByHands: both hands well ahead of the neck: the target halfway to their direction
                    float hands_yaw = -999.0f;
                    float nk[3];
                    if (g_turn_hands.load(std::memory_order_relaxed) && bp.hand_ok[0] && bp.hand_ok[1] && camera_lever::neck_offset(nk)) {
                        const float nl[3] = {nk[0], nk[1] - 0.10f, nk[2] + 0.08f};  // the neck point (the head's model)
                        float nw[3];
                        for (int k = 0; k < 3; ++k) nw[k] = cp[k] + bp.cam[k] * nl[0] + bp.cam[4 + k] * nl[1] + bp.cam[8 + k] * nl[2];
                        // the aiming pose, wherever it points: both hands 0.30 m or more out from the neck, within 0.35 m of
                        // each other (a look aside while aiming is the case; not against the head's or the holsters' facing)
                        bool ahead = true;
                        float mx = 0, mz = 0;
                        for (int j = 0; j < 2; ++j) {
                            const float vx = bp.hand[j][0] - nw[0], vz = bp.hand[j][2] - nw[2];
                            ahead = ahead && std::sqrt(vx * vx + vz * vz) > 0.30f;
                            mx += 0.5f * vx, mz += 0.5f * vz;
                        }
                        const float sx = bp.hand[0][0] - bp.hand[1][0], sz = bp.hand[0][2] - bp.hand[1][2];
                        ahead = ahead && std::sqrt(sx * sx + sz * sz) < 0.35f;
                        if (ahead) {
                            hands_yaw = std::remainder(std::atan2(-mx, -mz) * 57.29578f - hd * 57.29578f, 360.0f);
                            hy += 0.5f * std::remainder(hands_yaw - hy, 360.0f);
                        }
                    }
                    g_hands_yaw_pub.store(hands_yaw, std::memory_order_relaxed);
                    float d = std::remainder(hy - zyaw, 360.0f);
                    const float dz = g_turn_dead.load(std::memory_order_relaxed);
                    if (d > dz) zyaw += d - dz;
                    else if (d < -dz) zyaw += d + dz;
                    zyaw = std::remainder(zyaw, 360.0f);
                }
                g_zone_yaw_pub.store(zyaw, std::memory_order_relaxed);
                const float a = hd + zyaw * 0.0174532925f;
                hf[0] = -std::sin(a), hf[2] = -std::cos(a);
                hr[0] = std::cos(a), hr[2] = -std::sin(a);
            }
            if (rel_ok) {
                for (int z = 0; z < kZones; ++z) {
                    for (int k = 0; k < 3; ++k)
                        zp[z][k] = cp[k] + hr[k] * (rel[z][0] + zoff(z, 0)) + up[k] * (rel[z][1] + zoff(z, 1)) +
                                   hf[k] * (rel[z][2] + zoff(z, 2));
                    if (lefty) {  // mirrored across the camera's middle (the body's, as the body mode's)
                        const float d = (zp[z][0] - cp[0]) * hr[0] + (zp[z][2] - cp[2]) * hr[2];
                        for (int k = 0; k < 3; ++k) zp[z][k] -= 2.0f * d * hr[k];
                    }
                }
            }
            const bool follow = body::follows_head();
            float nn[3] = {no[0], no[1], no[2]};
            float lean = 0.0f;
            if (g_lean_steady.load(std::memory_order_relaxed)) {
                // [Holsters] LeanSteady: the torso pivoting at the hips: its drop gives the lean's most forward reach
                // (L sin a with L (1 - cos a) = the drop), the forward move up to that is the lean, the rest a step
                // (2026-10-27: the model shared with the physical crouch, camera_lever::lean_split)
                lean = camera_lever::lean_split(nn, g_torso_len.load(std::memory_order_relaxed));
            }
            g_lean_pub.store(lean, std::memory_order_relaxed);
            const float n[3] = {follow ? 0.0f : nn[0], nn[1], follow ? 0.0f : nn[2]};
            for (int k = 0; k < 3; ++k) sh[k] = bp.cam[k] * n[0] + bp.cam[4 + k] * n[1] + bp.cam[8 + k] * n[2];
            for (int z = 0; z < kZones; ++z)
                for (int k = 0; k < 3; ++k) zp[z][k] += sh[k];
        }
        was_anchor = use ? 1 : 0;
        g_anchor_used.store(use, std::memory_order_relaxed);
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(g_anchor_shift, sh, sizeof(sh));
        if (bp.cam_ok) std::memcpy(g_anchor_cam, bp.cam + 12, sizeof(g_anchor_cam));
    }
    // [Horse] StickTurn (2026-10-10, the user: "When on a horse, looking around with the joystick. The holsters do not
    // turn with you"): riding, the right stick's turn of the view turns the whole layout with it, about the head's
    // vertical axis: the player's own hips turn with their view while John keeps facing the horse (run 9 had the holsters
    // follow the horse)
    const float ride_turn = (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_VEHICLE)) ? pose::ride_turn_deg() : 0.0f;
    if (ride_turn != 0.0f && bp.cam_ok) {
        const float* cp = bp.cam + 12;
        const float a = hd + ride_turn * 0.0174532925f;
        const float tf[3] = {-std::sin(a), 0.0f, -std::cos(a)}, tr[3] = {std::cos(a), 0.0f, -std::sin(a)};
        for (int z = 0; z < kZones; ++z) {  // each zone's place in the horse's frame, the same in the turned one
            const float d[3] = {zp[z][0] - cp[0], zp[z][1] - cp[1], zp[z][2] - cp[2]};
            const float x = d[0] * hr[0] + d[2] * hr[2], f = d[0] * hf[0] + d[2] * hf[2];
            for (int k = 0; k < 3; ++k) zp[z][k] = cp[k] + tr[k] * x + up[k] * d[1] + tf[k] * f;
        }
        std::memcpy(hr, tr, sizeof(hr));
        std::memcpy(hf, tf, sizeof(hf));
    }
    // run 12 item 4, [Holsters] OwnBelt, RingsFollowHolsters: each ring where it is put, from its bone as the game hangs
    // it. (2026-10-13, the user: "Change the option ... to mean that they hold their position relative to eachother.
    // Moving one should move the other the same way": the link is kept by the moves, link_piece_moved / link_zone_moved,
    // so turning it on or off moves nothing; before, a ring took its piece's whole move here, and the rings the user had
    // placed with it off jumped when it was turned on)
    {
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(g_link_rgt, hr, sizeof(g_link_rgt));
        std::memcpy(g_link_fwd, hf, sizeof(g_link_fwd));
        g_link_lefty = lefty;
    }
    // the weapon manager (actor +0x70): +0x80 the item in hand, +0x448 the current slot, +0xa8 + s * 0x70 the slots
    uintptr_t wmgr = 0, in_hand = 0;
    int32_t cur = -1;
    rd(actor + 0x70, &wmgr);
    if (wmgr) {
        rd(wmgr + 0x80, &in_hand);
        rd(wmgr + 0x448, &cur);
    }
    auto slot_filled = [&](int s) {
        uintptr_t item = 0;
        return s >= 0 && s < 8 && wmgr && rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item;
    };
    auto slot_gun = [&](int s) -> int {  // the eWeapon of a slot's object (-1: none)
        uintptr_t item = 0, W = 0, info = 0;
        int16_t wt = -1;
        if (s >= 0 && s < 8 && wmgr && rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item && rd(item + 0xa0, &W) && W &&
            rd(W + 0x28, &info) && info)
            rd(info + 8, &wt);
        return wt;
    };
    auto slot_loaded = [&](int s) {  // the slot's gun model streamed in (W +0x2c0 = 2, as dual's resolve wants)
        uintptr_t item = 0, W = 0;
        int32_t prop = 0;
        return s >= 0 && s < 8 && wmgr && rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item && rd(item + 0xa0, &W) && W &&
               rd(W + 0x2c0, &prop) && prop == 2;
    };
    // a second gun waiting for its slot to hold the holster's gun (ACTOR_SET_NEXT_WEAPON queued), per controller
    struct SecWait {
        int slot = -1, weapon = -1, zone = -1;
        double since = 0.0;
        bool swap = false;  // (step 5, TorchSwap) the gun in hand going to its slot: ready only once out of the hand
    };
    static SecWait sec_wait[2];
    const bool choosing = g_weapon_choice.load(std::memory_order_relaxed) && st.owned_tick != 0;
    auto owns = [&](int w) { return choosing && w >= 0 && w < kWeapons && (st.owned >> w & 1); };
    // what each slot last held in hand (so a twin can draw another gun than its partner's)
    static int slot_weapon[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    static int zone_last[kZones] = {-1, -1, -1, -1, -1, -1, -1, -1};  // the gun each zone drew last (a twin keeps its own)
    static int drawn_from = -1;  // the zone of the last draw (round 13 item 10: the gun in hand's own holster)
    static int put_zone_of[kWeapons];  // run 7 item 1b: the zone each gun was last put away in (-1: not known)
    static int zone_item[kZones] = {-1, -1, -1, -1, -1, -1, -1, -1};  // run 10 item 10: the item put away there (AnyWeapon)
    static const bool put_zone_init = [] {
        for (int& z : put_zone_of) z = -1;
        return true;
    }();
    (void)put_zone_init;
    controls::commit_draw_hand(st.weapon, in_hand != 0);  // a draw by the other hand: its hand once the gun is in it
    if (in_hand && cur >= 0 && cur < 8 && st.weapon >= 0 && st.weapon < kWeapons) slot_weapon[cur] = st.weapon;
    // a gun goes back to the holster that chose it; else to any holster of its slot
    auto exact = [&](int zi) { return in_hand && zok[zi] && owns(zones[zi].weapon) && st.weapon == zones[zi].weapon; };
    bool any_exact = false;
    for (int zi = 0; zi < kZones; ++zi) any_exact = any_exact || exact(zi);
    // the holster's own test (its choice, else its slots), as before AnyWeapon
    auto holds_slot = [&](int zi) {
        if (!in_hand) return false;
        if (exact(zi)) return true;
        if (any_exact) return false;
        for (int s : zones[zi].slots)
            if (s >= 0 && s == cur) return true;
        return false;
    };
    auto holds = [&](int zi) {
        if (!in_hand) return false;
        if (any_item && zone_rounds(zi)) return false;  // a zone of rounds holds no item
        if (holds_slot(zi)) return true;
        // run 10 item 10 (AnyWeapon): an automatic holster takes the item in hand unless another item was put there (its
        // game slots' guns do not stop it: cycle D5, the Back's long guns made the Schofield draw the Bolt Action)
        const int w = st.weapon >= 0 && st.weapon < kWeapons ? st.weapon : -1;
        return any_here(zi) && !any_exact && zones[zi].weapon == -1 && w >= 0 && (zone_item[zi] == w || zone_item[zi] < 0);
    };
    // the weapon a zone draws: its choice; else for a twin, another owned weapon of its slots than the partner's (another
    // kind first: a rifle against a shotgun); else -1 (the slots in order, the game's)
    auto draw_weapon = [&](int zi) -> int {
        const Zone& z = zones[zi];
        if (owns(z.weapon)) return z.weapon;
        if (any_here(zi) && z.weapon == -1 && owns(zone_item[zi]) && !(in_hand && st.weapon == zone_item[zi])) return zone_item[zi];  // what was put there
        const int zl = zone_last[zi];
        if (choosing && zl >= 0 && zl < kWeapons && owns(zl)) {  // its own last gun, while owned and of its slots
            bool fits = false;  // (by the slot only: a hip draws the Sawed-off only as its choice; the review)
            for (int s : z.slots) fits = fits || (s >= 0 && st.equip_slot[zl] == s);
            const int other = z.partner >= 0 ? (owns(zones[z.partner].weapon) ? zones[z.partner].weapon : zone_last[z.partner]) : -1;
            if (fits && zl != other) return zl;
        }
        if (z.partner < 0 || !choosing) return -1;
        const Zone& p = zones[z.partner];
        int pw = owns(p.weapon) ? p.weapon : zone_last[z.partner] >= 0 && owns(zone_last[z.partner]) ? zone_last[z.partner] : -1;
        for (int s : p.slots)
            if (pw < 0 && s >= 0 && slot_weapon[s] >= 0) pw = slot_weapon[s];
        const int ps = pw >= 0 && pw < kWeapons ? st.equip_slot[pw] : -1;
        for (int pass = 0; pass < 2; ++pass)
            for (int s : z.slots) {
                if (s < 0 || (pass == 0) == (s == ps)) continue;
                for (int w = kWeapons - 1; w >= 0; --w)
                    if (owns(w) && st.equip_slot[w] == s && w != pw) return w;
            }
        return -1;
    };
    // [Hands] OffHandItems: the off-hand item a zone gives (the knife or the torch, a throwable), -1 none: the zone's draw
    // if it is one, else its melee or thrown slot's item (the weapon manager's: the game's own slot 0 or 2), owned
    auto offhand_item = [&](int zi) -> int {
        auto owned = [&](int w) { return w >= 0 && w < kWeapons && st.owned_tick != 0 && (st.owned >> w & 1); };
        const int dw = draw_weapon(zi);
        if (dw >= 0) return offhand::kind_of(dw) != offhand::kNone && owned(dw) ? dw : -1;
        for (int s : zones[zi].slots) {
            if (s != 0 && s != 2) continue;
            const int w = slot_gun(s);
            if (offhand::kind_of(w) != offhand::kNone && owned(w)) return w;
        }
        return -1;
    };
    // the gun hand (a controller; John's right hand is always the gun's) and the front hand, the loading point (a
    // tenth of a metre out along the barrel from the gun hand's wrist) and the barrel's world direction
    const int gun_h = controls::gun_hand(), off_h = 1 - gun_h;
    // the barrel in the IK target's axes: the latest aim ray's, else its measured constant (rigid with the controller,
    // 10 degrees above the target's -z, every gun; the target's own -z is tipped by the hand calibration)
    float bt[3] = {0.0f, 0.174f, -0.985f}, u[3];
    aim::barrel_in_target(bt);
    for (int k = 0; k < 3; ++k)
        u[k] = bp.target_rot[bp.gun][k * 3] * bt[0] + bp.target_rot[bp.gun][k * 3 + 1] * bt[1] + bp.target_rot[bp.gun][k * 3 + 2] * bt[2];
    float load_pt[3], load_alt[3] = {};  // load_alt: with every gun's ring, when the gun has its own (the glow's fallback)
    bool load_alt_ok = false;
    const int gj = bp.gun, fj = 1 - gj;  // John's gun hand and his front hand
    float gb[2][3];  // [Hands] InteractOffset: each hand's interaction spot (what grabs); the gun hand's wrist stays the gun's reference
    for (int j = 0; j < 2; ++j) grab_point(bp, j, gb[j]);
    float lo[3], lr = 0.12f;
    bool touch = false;
    {
        std::lock_guard lock(g_zone_mutex);
        touch = g_insert_touch;
    }
    g_gun_in_hand.store(reload::is_gun(st.weapon) ? st.weapon : -1, std::memory_order_relaxed);  // (C35: the review, the guns only)
    bool lo_own = false;
    gun_adjust(st.weapon, kAdjLoadRing, lo, &lr, &lo_own, bp.gun);  // round 13 item 8: the gun's own loading ring, else every gun's (C8: its hand's)
    {  // [Reload] LoadPointOffset in the gun's axes: right (the target's x), up (square to the barrel), forward (the barrel)
        const float* Rg = bp.target_rot[bp.gun];
        float rgt[3] = {Rg[0], Rg[3], Rg[6]}, upv[3] = {rgt[1] * u[2] - rgt[2] * u[1], rgt[2] * u[0] - rgt[0] * u[2], rgt[0] * u[1] - rgt[1] * u[0]};
        const float ul = std::sqrt(upv[0] * upv[0] + upv[1] * upv[1] + upv[2] * upv[2]);
        if (ul > 1e-4f)
            for (float& x : upv) x /= ul;
        else
            upv[0] = upv[2] = 0.0f, upv[1] = 1.0f;
        // (step 8, R14) a muzzle-loader's round goes in at its muzzle: 0.55 m out along the barrel (actions.cpp's ram zone)
        const float ahead = actions::muzzle_loader(st.weapon) ? 0.55f : 0.10f;
        for (int k = 0; k < 3; ++k) load_pt[k] = bp.hand[gj][k] + u[k] * (ahead + lo[2]) + rgt[k] * lo[0] + upv[k] * lo[1];
        if (lo_own) {  // (round 74) every gun's ring here too: the glow's fallback
            float le[3], lre = 0.12f;
            bool lte = false;
            load_point(le, &lre, &lte);
            for (int k = 0; k < 3; ++k) load_alt[k] = bp.hand[gj][k] + u[k] * (ahead + le[2]) + rgt[k] * le[0] + upv[k] * le[1];
            load_alt_ok = std::isfinite(load_alt[0] + load_alt[1] + load_alt[2]);
        }
        // (2026-11-01, GOAL-offhand-changes C37; the user: every interaction spot moves when the gun is held by the off hand:
        // the rings locked to the gun's model) the two-handed hold turns the drawn gun off the gun hand's target, which the
        // point above follows: learned in the drawn gun's frame while held in the gun hand alone (not through a shot), and
        // placed from that frame while two-handed
        const bool mfr = !bp.gun_frame_ok && bp.gun_mframe_ok;
        const float* FR = mfr ? bp.gun_mframe_R : bp.gun_frame_R;
        const float* FO = mfr ? bp.gun_mframe_o : bp.gun_frame_o;
        if ((bp.gun_frame_ok || mfr) && st.weapon >= 0 && st.weapon < kWeapons && gj >= 0 && gj <= 1) {
            const float tb = g_two_blend.load(std::memory_order_relaxed);
            // (C37c, the review: not while carried or on the carry's put-away: the frame there is the front hand's)
            if (tb < 0.05f && g_fire_phase.load(std::memory_order_relaxed) < 0.0f && actions::since_shot_ms() > 1500.0 && !dual::state().carry &&
                !dual::carry_returning()) {
                const float d[3] = {load_pt[0] - FO[0], load_pt[1] - FO[1], load_pt[2] - FO[2]};
                for (int j = 0; j < 3; ++j) g_lg[st.weapon][gj][j] = FR[j] * d[0] + FR[3 + j] * d[1] + FR[6 + j] * d[2];
                g_lg_ok[st.weapon][gj] = true;
            } else if (tb > 0.05f && g_lg_ok[st.weapon][gj]) {
                const float* g = g_lg[st.weapon][gj];
                float m[3];
                for (int k = 0; k < 3; ++k) m[k] = FO[k] + FR[k * 3] * g[0] + FR[k * 3 + 1] * g[1] + FR[k * 3 + 2] * g[2];
                for (int k = 0; k < 3; ++k) load_pt[k] = load_pt[k] + (m[k] - load_pt[k]) * std::fmin(1.0f, tb);
                g_load_on_model.fetch_add(1, std::memory_order_relaxed);
                load_alt_ok = false;  // (round 74, the review: the fallback not placed on the model: none two-handed)
            }
        }
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(g_load_diag.pt, load_pt, sizeof(g_load_diag.pt));
    }
    // [Reload] InsertOnTouch: the drawn round's distance from the point (the hand holding one), else the wrist's.
    // lp: the loading point (load_pt, the gun in hand's; DualWieldCrossReload: load_sec, the second gun's)
    auto round_at = [&](int rj, const float* lp, float* d_round, float* d_wrist) {
        const float dx = gb[rj][0] - lp[0], dy = gb[rj][1] - lp[1], dz = gb[rj][2] - lp[2];
        *d_wrist = std::sqrt(dx * dx + dy * dy + dz * dz);
        *d_round = -1.0f;
        float pz[12];
        if (round_draw::pose(bp, rj, pz)) {
            const float ex = pz[9] - lp[0], ey = pz[10] - lp[1], ez = pz[11] - lp[2];
            *d_round = std::sqrt(ex * ex + ey * ey + ez * ez);
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_load_diag.round, pz + 9, sizeof(g_load_diag.round));
            std::memcpy(g_load_diag.axis, pz + 3, sizeof(g_load_diag.axis));
            g_load_diag.round_ok = true;
        }
    };
    const bool gun = reload::is_gun(st.weapon) && in_hand, long_gun = gun && reload::is_long_gun(st.weapon);
    // [Hands] DualWieldBesideTorch (2026-10-29): the torch (33) in hand stands in for a gun in hand for DualWield's second
    // gun's draw only (dual::second_beside: a gun, or exactly the torch with the option on). Everything else keyed on `gun`
    // (rounds, the copy, DualWieldCrossReload, OffHandItems, the raise) stays a gun's: with the torch, the second gun
    // loads by its own chest squeeze, and (F3, 2026-11-01) the torch hand's pressed cross take at the chest (item_x below)
    const bool sec_beside = gun || (in_hand && dual::beside_torch(st.weapon));
    {  // the user, 2026-10-09: "a revolver facing backwards in my holster and then also coming out backwards when I grab it"
       // (the right hip, the right hand, the Cattleman and another revolver; not seen in the simulator): once a draw, 0.4 s
       // in, the drawn sidearm's barrel in the gun hand's frame (forward about (0 0.17 -0.98)), BACKWARDS when it points back
        static int diag_w = -1;
        static double diag_ms = 0.0;
        static bool diag_done = false;
        if (gun && st.weapon >= 0 && st.weapon <= 7) {
            if (st.weapon != diag_w) {
                diag_w = st.weapon;
                diag_ms = now_ms;
                diag_done = false;
            }
            float b[3];
            if (!diag_done && now_ms - diag_ms > 400.0 && aim::barrel_in_target(b)) {
                diag_done = true;
                const bool back = b[2] > 0.0f;
                log::write(back ? log::Level::Warn : log::Level::Info,
                           "[holster] the %s in hand (from the %s, the %s hand): its barrel in the hand's frame (%.2f %.2f %.2f)%s | the hips' choices %s, %s",
                           kWeaponLabel[st.weapon], drawn_from >= 0 ? zones[drawn_from].key : "?", gun_h ? "right" : "left", b[0], b[1], b[2],
                           back ? " BACKWARDS" : "", token_of(zones[0].weapon), token_of(zones[5].weapon));
            }
        } else {
            diag_w = -1;
        }
    }
    // the empty gun clicks: the gun hand's trigger pulled with nothing loaded (with or without spare rounds), not while
    // the menu has the controllers. [Reload] EmptyClick: a click sound from the gun hand's side, the buzz, both or none
    {
        const bool t = hands::get(gun_h).trigger > 0.6f && !whistle::suppressed(gun_h);  // run 8: the whistle's trigger clicks nothing
        if (t && !trig_down && gun && st.clip < 0.5f && reload::hand_reload() && !menu::visible() &&
            // an open revolver or an unworked action: its own buzz, never the empty click (round 10); DualWieldCrossReload's
            // hold (the gun hand holding the second gun's rounds): its pull is held back, not empty
            !actions::fire_blocked() && !g_cross_fire_hold[gun_h].load(std::memory_order_relaxed)) {
            audio::empty_click(gun_h, st.weapon);  // the family's click (round 9: the user's sounds)
            g_clicks.fetch_add(1, std::memory_order_relaxed);
        }
        trig_down = t;
    }
    // the front hand at the foregrip: near the barrel line, ahead of the gun hand
    float fore_along = 0.0f, fore_perp = 1e9f;
    {
        float p[3], perp2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            p[k] = gb[fj][k] - bp.hand[gj][k];
            fore_along += p[k] * u[k];
        }
        for (int k = 0; k < 3; ++k) perp2 += (p[k] - fore_along * u[k]) * (p[k] - fore_along * u[k]);
        fore_perp = std::sqrt(perp2);
    }
    // the foregrip point (body.cpp: the game's grip on the drawn gun, moved by ForegripOffset) within its radius; until
    // the game's grip is known, the old test: near the barrel line, 0.12 - 0.85 m ahead of the gun hand
    float fore_r = 0.20f, fore_d = 1e9f, fore_zone[3] = {bp.fore[0], bp.fore[1], bp.fore[2]};
    {  // [Reload] SteadyRing: the grip point's mean in the drawn gun's frame 0.4-1.2 s after the draw, kept for that draw
        static int lock_w = -2;
        static double lock_t0 = 0.0;
        static float sum[3] = {}, locked[3] = {};
        static int n = 0;
        static bool have = false;
        if (st.weapon != lock_w || !long_gun) {
            lock_w = st.weapon;
            lock_t0 = now_ms;
            sum[0] = sum[1] = sum[2] = 0.0f;
            n = 0;
            have = false;
        }
        // (run 8: the lock caught the template before the gun's own grip was learned; the grip itself is steadied
        // now in body.cpp, so the ring is the grip point again: the lock stays off)
        if (false && long_gun && bp.fore_ok && bp.gun_frame_ok && g_steady_ring.load(std::memory_order_relaxed)) {
            const float* G = bp.gun_frame_R;  // columns: the gun's axes
            if (!have) {
                const double age = now_ms - lock_t0;
                if (age >= 400.0 && age <= 1200.0) {
                    const float d[3] = {bp.fore[0] - bp.gun_frame_o[0], bp.fore[1] - bp.gun_frame_o[1], bp.fore[2] - bp.gun_frame_o[2]};
                    for (int i = 0; i < 3; ++i) sum[i] += G[0 * 3 + i] * d[0] + G[1 * 3 + i] * d[1] + G[2 * 3 + i] * d[2];
                    ++n;
                } else if (age > 1200.0 && n > 0) {
                    for (int i = 0; i < 3; ++i) locked[i] = sum[i] / static_cast<float>(n);
                    have = true;
                    g_ring_locks.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (have)
                for (int k = 0; k < 3; ++k) fore_zone[k] = bp.gun_frame_o[k] + G[k * 3] * locked[0] + G[k * 3 + 1] * locked[1] + G[k * 3 + 2] * locked[2];
        }
    }
    {  // [Reload] ForegripZoneOffset / ForegripZoneRadius: the ring, apart from where John's hand sits (bp.fore)
        float zo[3];
        gun_adjust(st.weapon, kAdjForeRing, zo, &fore_r, nullptr, bp.gun);  // round 13 item 8: the gun's own ring, else every gun's (C8: its hand's)
        const float* Rg = bp.target_rot[bp.gun];
        float rgt[3] = {Rg[0], Rg[3], Rg[6]}, upv[3] = {rgt[1] * u[2] - rgt[2] * u[1], rgt[2] * u[0] - rgt[0] * u[2], rgt[0] * u[1] - rgt[1] * u[0]};
        const float ul = std::sqrt(upv[0] * upv[0] + upv[1] * upv[1] + upv[2] * upv[2]);
        if (ul > 1e-4f)
            for (float& x : upv) x /= ul;
        else
            upv[0] = upv[2] = 0.0f, upv[1] = 1.0f;
        if (bp.gun_frame_ok && g_ring_gun_axes.load(std::memory_order_relaxed)) {
            // run 8 item 2: the drawn gun's own axes (columns: x right, y up, z back), so the ring keeps its place on the gun
            // whatever the hand's turn against it (the Sawed-off's ring offset, 6 cm across, swung about the gun as the
            // controller's axes and the drawn gun's parted)
            const float* G = bp.gun_frame_R;
            for (int k = 0; k < 3; ++k) fore_zone[k] += G[k * 3] * zo[0] + G[k * 3 + 1] * zo[1] - G[k * 3 + 2] * zo[2];
        } else {
            for (int k = 0; k < 3; ++k) fore_zone[k] += rgt[k] * zo[0] + upv[k] * zo[1] + u[k] * zo[2];
        }
    }
    if (bp.fore_ok) {
        float dd = 0.0f;
        for (int k = 0; k < 3; ++k) dd += (gb[fj][k] - fore_zone[k]) * (gb[fj][k] - fore_zone[k]);
        fore_d = std::sqrt(dd);
    }
    if (bp.fore_ok && bp.gun_frame_ok && long_gun) fore_trace(st.weapon, bp.gun_frame_R, bp.gun_frame_o, fore_zone, now_ms);
    const dual::State ds = dual::state();  // [Hands] DualWield: the second gun, if one is out
    // [Hands] DualWieldCrossReload (2026-10-27, the user: "When dual weilding, add option for both hands to be able to grab
    // the other guns bullets and reload"): a second gun out (DualWield's, or the copy of DualWieldCopy with its own count;
    // not a carry, never fired) with a gun in hand: each hand takes rounds at the chest for the gun in the OTHER hand, as
    // a free hand does. cross_tgt(h): the gun controller h takes rounds for (1 the second gun or the copy: the gun hand's;
    // 0 the gun in hand: the other hand's), -1 not a cross take (the hand's own rules as before)
    // (2026-10-31, the off-hand build step 3, R17; 53.8, the user: "Can't grab round with gun hand while holding only with
    // off hand") [Reload] CarriedRounds: a gun carried by the foregrip, the gun hand empty: that hand takes rounds at the
    // chest for the carried gun, as for a second gun (its loading point on the carried gun as placed, its count dual's)
    const bool carry_x = g_carry_rounds.load(std::memory_order_relaxed) && ds.carry && ds.ready && !in_hand && ds.ctrl >= 0 && ds.ctrl != gun_h;
    // (2026-11-01, GOAL-offhand-fixes F3; round 63 check 3: "no rounds at the chest" for the Volcanic beside the knife) the
    // knife, the torch or a throwable in hand beside a second gun (DualWieldBesideItems, TorchSwap): that hand takes rounds at
    // the chest for the second gun, as the carry's empty gun hand does (no hand had a take for it: cross needed a gun in hand);
    // round 55's entry without a press only for the knife (the review: a throwable's wind-up through the chest took rounds,
    // and their let-go wait lost the throw; held rounds hold RT back, the torch's light and the throw's arming)
    const bool item_x = dual::cross_reload() && ds.on && !ds.carry && !ds.copy && in_hand && !gun && offhand::kind_of(st.weapon) != offhand::kNone &&
                        ds.ctrl >= 0 && ds.ctrl != gun_h;
    const bool cross = (dual::cross_reload() && ds.on && !ds.carry && gun && ds.ctrl >= 0 && ds.ctrl != gun_h) || carry_x || item_x;
    auto cross_tgt = [&](int h) { return !cross ? -1 : carry_x || item_x ? (h == gun_h ? 1 : -1) : h == ds.ctrl ? 0 : h == gun_h ? 1 : -1; };
    // the gun shown in the other hand: the second gun; the copy's own model (DualWieldOwnModel, the other hip's sidearm),
    // else the gun in hand's (its rounds' calibre drawn, its own loading ring)
    const int sec_shown = ds.copy && dual::copy_model() >= 0 ? dual::copy_model() : ds.weapon;
    // the spare the other gun loads from, for the take (a handful's size; dual reads its own when it loads): the copy's the
    // gun in hand's (one ammo type), the second gun's its own type's
    const float sec_spare = ds.copy ? st.spare : ds.spare;
    // the second gun's loading point and ring, as the gun in hand's (above) from the hand holding it: its IK target's
    // barrel (the same constant direction, rigid with the controller: every gun's) and the gun's own loading ring
    float load_sec[3] = {0.0f, 0.0f, 0.0f}, lr_sec = 0.12f;
    // (2026-10-31, the off-hand build step 4, S3; 55.9, the user: "Works for reloading right gun, not left") the other hand's
    // gun's ring no smaller than the default LoadPointRadius (the LeMat's own 4 cm, set for the gun hand's reach, held out
    // in the other hand: r79 s2, the ring 3.6 cm from the copy's held place)
    auto sec_ring_floor = [&]() {
        float go[3], gr = 0.0f;
        gun_adjust(-1, kAdjLoadRing, go, &gr);
        lr_sec = std::fmax(lr_sec, gr);
    };
    float crg[9], cog[3];
    const bool carry_pt = carry_x && body::carry_frame(crg, cog);  // (R17: the carried gun's frame, columns its axes, -z its barrel)
    const bool sec_pt = cross && ds.ready && ds.john >= 0 && ds.john <= 1 && bp.hand_ok[ds.john] && (!carry_x || carry_pt);
    // (2026-11-02, GOAL-offhand-changes C37c; round 68 check 6, the user's screenshots: carried by the off hand the loading
    // ring sat elsewhere than on the gun in hand) the carried gun's loading point where the gun in hand had it on the gun:
    // its point learned in the drawn gun's frame (the gun hand's, else the other's), on the carried gun as drawn
    const int lgh = sec_shown >= 0 && sec_shown < kWeapons ? (g_lg_ok[sec_shown][1] ? 1 : g_lg_ok[sec_shown][0] ? 0 : -1) : -1;
    if (sec_pt && carry_pt && lgh >= 0) {
        float los[3];
        gun_adjust(sec_shown, kAdjLoadRing, los, &lr_sec, nullptr, lgh);  // (its ring's size the gun hand's)
        sec_ring_floor();
        const float* g = g_lg[sec_shown][lgh];
        for (int k = 0; k < 3; ++k) load_sec[k] = cog[k] + crg[k * 3] * g[0] + crg[k * 3 + 1] * g[1] + crg[k * 3 + 2] * g[2];
        g_load_on_model.fetch_add(1, std::memory_order_relaxed);
    } else if (sec_pt && carry_pt) {
        float los[3];
        gun_adjust(sec_shown, kAdjLoadRing, los, &lr_sec, nullptr, ds.john);  // (C8: the carrying hand's)
        sec_ring_floor();
        // (the review) from the gun's grip on the carried frame (the take-back's point), as the gun in hand's is from the hand
        float base[3];
        for (int k = 0; k < 3; ++k)
            base[k] = cog[k] + (carry_grip_g_ok ? crg[k * 3] * carry_grip_g[0] + crg[k * 3 + 1] * carry_grip_g[1] + crg[k * 3 + 2] * carry_grip_g[2] : 0.0f);
        for (int k = 0; k < 3; ++k)
            load_sec[k] = base[k] - crg[k * 3 + 2] * (0.10f + los[2]) + crg[k * 3] * los[0] + crg[k * 3 + 1] * los[1];
    } else if (sec_pt) {
        const int sj = ds.john;
        const float* Rs = bp.target_rot[sj];
        float us[3], los[3];
        for (int k = 0; k < 3; ++k) us[k] = Rs[k * 3] * bt[0] + Rs[k * 3 + 1] * bt[1] + Rs[k * 3 + 2] * bt[2];
        gun_adjust(sec_shown, kAdjLoadRing, los, &lr_sec, nullptr, sj);  // (C8: the second gun's hand's)
        sec_ring_floor();
        float rgt[3] = {Rs[0], Rs[3], Rs[6]}, upv[3] = {rgt[1] * us[2] - rgt[2] * us[1], rgt[2] * us[0] - rgt[0] * us[2], rgt[0] * us[1] - rgt[1] * us[0]};
        const float ul = std::sqrt(upv[0] * upv[0] + upv[1] * upv[1] + upv[2] * upv[2]);
        if (ul > 1e-4f)
            for (float& x : upv) x /= ul;
        else
            upv[0] = upv[2] = 0.0f, upv[1] = 1.0f;
        const float sec_ahead = actions::muzzle_loader(ds.weapon) ? 0.55f : 0.10f;  // (step 8) a muzzle-loader second gun's: its muzzle
        for (int k = 0; k < 3; ++k) load_sec[k] = bp.hand[sj][k] + us[k] * (sec_ahead + los[2]) + rgt[k] * los[0] + upv[k] * los[1];
    }
    {
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(g_load_diag.sec_pt, load_sec, sizeof(load_sec));
        g_load_diag.sec_r = lr_sec;
        g_load_diag.sec_ok = sec_pt;
    }
    // the second gun takes rounds now: ready, room by its own count, spare rounds of its type, its kind's gates (a gun
    // that opens or is worked by hand to load, with ManualActions' LoadOnlyWhenOpen: actions.cpp works only the gun in
    // hand, so such a second gun is refused, as can_load says for any gun not in hand), no Dead Eye, not the game's reload.
    // The copy: no gate of a hand-worked action. It has none of its own (actions.cpp's open, hammer and cylinder are the
    // gun in hand's, the same eWeapon: can_load(its weapon) would read the gun in hand's cylinder), it fires without
    // being cocked or shut, and its chest squeeze loads it ungated: so it takes rounds whatever the gun in hand's state
    // [Reload] PistolMagazines (2026-10-28): a magazine pistol in hand (the Mauser: its clip): where its magazine goes in
    // (the well under the grip, the Mauser's clip guide), the ring the magazine's top must reach there, and whether it
    // takes one now (its magazine out, the Mauser's room; spare rounds; not Dead Eye nor the game's reload)
    const bool magg = gun && actions::mag_gun(st.weapon);
    float mag_pt[3] = {0.0f, 0.0f, 0.0f};
    constexpr float kMagRing = 0.05f;
    const bool mag_pt_ok = magg && actions::mag_point(bp, st.weapon, mag_pt, nullptr);
    const bool mag_loadable = mag_pt_ok && reload::hand_reload() && actions::mag_wanted(st);
    // the held magazine's top (its lips; the clip's foot, pushed down into the gun's top) from John's hand rj, its
    // distance from the well (-1: not known)
    auto mag_top_d = [&](int rj, float* top = nullptr, float* mid = nullptr) {
        float mp[12];
        if (!mag_pt_ok || !round_draw::mag_pose(bp, rj, st.weapon, mp)) return -1.0f;
        const float len = st.weapon == 3 ? 0.0f : round_draw::mag_length(st.weapon);
        const float half = 0.5f * round_draw::mag_length(st.weapon);
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float tp = mp[9 + k] + mp[3 + k] * len;
            if (top) top[k] = tp;
            if (mid) mid[k] = mp[9 + k] + mp[3 + k] * half;
            d2 += (tp - mag_pt[k]) * (tp - mag_pt[k]);
        }
        return std::sqrt(d2);
    };
    // ([Hands] DualWieldOpen, 2026-10-28: a revolver opened by its own stick takes them while open; the copy then too)
    auto sec_gate = [&]() { return actions::can_load_second(ds); };
    const bool sec_loadable = sec_pt && reload::hand_reload() && ds.weapon >= 0 && ds.clip >= 0.0f && ds.clip_max >= 1.0f &&
                              ds.clip + 0.5f <= ds.clip_max && sec_spare >= 1.0f && sec_gate() &&
                              !(st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING));
    // up to n of controller h's rounds into the gun they are for (round_tgt), its insert sound at its loading point and
    // both hands' buzz: the gun in hand by the game's own count as before (sx: its state to judge by, the let-go's with
    // the rounds just queued), the second gun by its own (dual::load_second), the copy by its own (dual::load_copy_rounds:
    // -1 while the spare it sets is not settled, the rounds kept for the next update). The rounds put in
    auto put_in = [&](int h, int n, const RdrvrActorState& sx) -> int {
        int in = 0;
        if (round_tgt[h] == 1) {
            // (the copy: the gun in hand's spare, read from the inventory by dual; the gun in hand's clip, which it shares
            // as a weapon object, untouched: its count is dual's own)
            if (sec_pt && reload::hand_reload() && sec_gate() && !(st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING)))
                in = ds.copy ? dual::load_copy_rounds(n, st.actor) : dual::load_second(n, st.actor);
            if (in > 0) {
                // (its sound only: actions.cpp's state is the gun in hand's; the copy's cylinder is not turned, nor the
                // gun in hand's, which shares its eWeapon)
                actions::round_in(sec_shown, load_sec, in, true);
                if (ds.carry) actions::carried_round_in(ds.weapon);  // (R17, the review)
                controllers::pulse(ds.ctrl, 0.5f, 20);
                g_cross_in.fetch_add(static_cast<uint64_t>(in), std::memory_order_relaxed);
            }
        } else {
            in = actions::can_load(st.weapon) ? (n > 1 ? reload::insert_rounds(sx, n) : reload::insert_round(sx) ? 1 : 0) : 0;
            if (in > 0) {
                actions::round_in(st.weapon, load_pt, in);
                controllers::pulse(gun_h, 0.5f, 20);
                if (round_x[h]) g_cross_in.fetch_add(static_cast<uint64_t>(in), std::memory_order_relaxed);
            }
        }
        if (in > 0) {
            g_rounds_in.fetch_add(static_cast<uint64_t>(in), std::memory_order_relaxed);
            controllers::pulse(h, 0.5f, 20);
        }
        return in;
    };
    const bool fore_near = !ds.on && long_gun && reload::two_handed() && bp.hand_ok[0] && bp.hand_ok[1] &&
                           (bp.fore_ok ? fore_d < fore_r : fore_along > 0.12f && fore_along < 0.85f && fore_perp < 0.18f);
    // the Double-barrel open: the off hand is for its barrels (actions.cpp), no two-handed hold until it is shut
    const bool broken = actions::break_open();
    // a hand at an action's part (a bolt, the open barrels, a breech, the semi-auto's handle) grips that part, not the
    // foregrip (cycle M2: the Springfield's trapdoor lies 11 cm from the foregrip's point, inside its ring)
    const bool fore_ok = fore_near && !broken && !actions::grip_wanted(off_h);
    // [Hands] CarryByForegrip: mounting or dismounting (riding or driving begun or ended) ends a carry (the game holsters
    // for it). (2026-11-01, GOAL-offhand-changes C16; the user: on horseback the grip hand lets go and the foregrip hand
    // carries) a carry begun riding rides on: only the change ends it
    const bool seated_now = (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)) != 0;
    const bool deadeye_now = (st.weapon_flags & RDRVR_WEAPON_DEADEYE) != 0;
    static bool carry_seated = false;  // seated_now at the carry's hand-over
    if (ds.carry && seated_now != carry_seated) dual::end(seated_now ? "mounted" : "dismounted");
    // the hand-over's state (the gun hand's press, at its let-go too): a long gun of slot 4-6 fully two-handed, John's
    // front hand on the front controller, no part of its action open or unworked. (C16: either of John's hands the gun
    // hand: drawn into his left, the left-handed transplant, his right carries it; the review: none on a mount's own frame,
    // seat_was and seat_ms are updated below)
    auto carry_can = [&](int h) {
        // (fully: 0.999, as body.cpp learns the front hand's hold and fingers only then: the review)
        return dual::carry_enabled() && h == gun_h && long_gun && in_hand && cur >= 4 && cur <= 6 && two_hold[off_h] && grip_down[off_h] && !ds.on &&
               g_two_blend.load(std::memory_order_relaxed) >= 0.999f && bp.ctrl[fj] == off_h && bp.hand_ok[0] && bp.hand_ok[1] &&
               !broken && !actions::grip_wanted(h) && actions::carry_handover_ok() && !deadeye_now && !menu::visible() &&
               seated_now == seat_was && now_ms - seat_ms >= kHoldSeatMs && !(st.flags & RDRVR_ACTOR_IN_COVER) && !(st.weapon_flags & RDRVR_WEAPON_RELOADING);
    };
    // the take-back's point: where the gun hand let go of the gun, on the carried gun as the game update last placed it
    // (body::carry_frame; round 36: in the carrier's target frame alone it stayed with the controller while the gun's T
    // held a turn the hand made in its first second, and the gun's grip was out of reach), else in the carrier's target
    // frame (the put-away: the gun is placed rigidly in that frame then; "skel carry old": always)
    float take_w[3] = {}, take_c[3] = {};
    bool take_gun = false;
    const bool take_ok = ds.carry && !dual::carry_returning() && ds.john >= 0 && ds.john <= 1 && bp.hand_ok[ds.john];
    if (take_ok) {
        const float* R = bp.target_rot[ds.john];  // columns: the axes
        for (int k = 0; k < 3; ++k)
            take_c[k] = bp.hand[ds.john][k] + R[k * 3] * carry_grip_c[0] + R[k * 3 + 1] * carry_grip_c[1] + R[k * 3 + 2] * carry_grip_c[2];
        float Rg[9], og[3];
        take_gun = carry_grip_g_ok && !body::carry_old_test() && body::carry_frame(Rg, og);
        for (int k = 0; k < 3; ++k)
            take_w[k] = take_gun ? og[k] + Rg[k * 3] * carry_grip_g[0] + Rg[k * 3 + 1] * carry_grip_g[1] + Rg[k * 3 + 2] * carry_grip_g[2] : take_c[k];
    }
    // the take-back: an empty hand (the other controller than the carrier's) within 10 cm of that point
    // ([Reload] CarriedBolt: not the hand the carried bolt has, nearer its bolt than this point or gripping it: actions.cpp)
    auto carry_near = [&](int h, int jh) {
        if (!take_ok || dual::carry_returning() || h == ds.ctrl || in_hand || !bp.hand_ok[jh] || actions::grip_wanted(h)) return false;
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) d2 += (gb[jh][k] - take_w[k]) * (gb[jh][k] - take_w[k]);
        // ([Reload] CarriedBoltTrigger, round 55: the grip at the carried bolt takes the gun back too: the trigger works it)
        return d2 < 0.10f * 0.10f || actions::carried_bolt_near(gb[jh]);
    };
    {  // [Reload] CarriedBolt (actions.cpp): the take-back's point, for the carried bolt's zone to give way to it
        std::lock_guard lock(g_take_mx);
        std::memcpy(g_take_pt, take_w, sizeof(g_take_pt));
        std::memcpy(g_take_ctl, take_c, sizeof(g_take_ctl));
        g_take_ok = take_ok;
        g_take_gun = take_gun;
        g_take_ms = now_ms;
    }
    // the gun in hand takes a round now (the game's count: the gun not full, spare rounds; its action's gate; no Dead Eye,
    // not the game's reload)
    auto gun_loadable = [&]() {
        return reload::hand_reload() && gun && st.clip + 0.5f <= st.clip_max && st.spare >= 1.0f && actions::can_load(st.weapon) &&
               !(st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING));
    };
    // a take of rounds into controller h's hand (the chest's, or [Reload] RoundSource=hand's): xt its cross target
    // (cross_tgt), can: the gun they are for takes them now; where: the log line's ending ("" at the chest). Held as
    // round_held, RoundsPerGrab of them (the target's room and spare), drawn and loaded at the loading point as before.
    // (Item 3's magazines: a take of another kind, by the same two sources)
    auto take_rounds = [&](int h, int xt, bool can, const char* where) {
        round_held[h] = can;
        round_mag[h] = false;  // (the review: never a stale magazine's)
        controllers::pulse(h, can ? 0.4f : 0.15f, can ? 25 : 10);
        if (can) {
            // [Reload] RoundsPerGrab: as many as the gun has room for and the spare ammo gives (the target's own)
            round_tgt[h] = xt == 1 ? 1 : 0;
            round_x[h] = xt >= 0;
            const float room = xt == 1 ? std::fmin(ds.clip_max - ds.clip, sec_spare) : std::fmin(st.clip_max - st.clip, st.spare);
            const int per = g_per_grab.load(std::memory_order_relaxed);
            round_n[h] = static_cast<int>(std::fmax(1.0f, std::fmin(static_cast<float>(per), std::floor(room + 0.01f))));
            round_next_ms[h] = 0.0;
            {  // (cycle r174) the closest distances counted from this grab
                std::lock_guard lock(g_zone_mutex);
                g_load_diag.min_round_d = g_load_diag.min_wrist_d = -1.0f;
            }
            g_rounds_taken.fetch_add(1, std::memory_order_relaxed);
            if (xt >= 0) g_cross_takes.fetch_add(1, std::memory_order_relaxed);
            if (xt == 1)
                log::info("[holster] %s hand (the gun hand) took %d round%s for the %s (DualWieldCrossReload; weapon %d, clip %.0f/%.0f, spare %.0f)%s",
                          h ? "right" : "left", round_n[h], round_n[h] == 1 ? "" : "s", ds.carry ? "carried gun" : ds.copy ? "copy" : "second gun", ds.weapon, ds.clip,
                          ds.clip_max, sec_spare, where);
            else
                log::info("[holster] %s hand took %d round%s%s (clip %.0f/%.0f, spare %.0f)%s", h ? "right" : "left", round_n[h], round_n[h] == 1 ? "" : "s",
                          xt == 0 ? (ds.copy ? " for the gun in hand, holding the copy (DualWieldCrossReload)"
                                             : " for the gun in hand, holding the second gun (DualWieldCrossReload)")
                                  : "",
                          st.clip, st.clip_max, st.spare, where);
        } else if (xt >= 0) {
            log::info("[holster] %s hand %s, dual wielding: no round for the %s (DualWieldCrossReload; %s) | clip %.0f of %.0f, spare %.0f, hand reload %d, "
                      "Dead Eye or reloading %d",
                      h ? "right" : "left", *where ? (std::strstr(where, "palm") ? "palm up" : "near the gun") : "at the chest",
                      xt == 1 ? (ds.carry ? "carried gun" : ds.copy ? "copy" : "second gun") : "gun in hand",
                      xt == 1 ? (!sec_pt ? "not ready" : !sec_gate() ? "its action shut" : "full, no spare rounds, Dead Eye or a reload")
                              : (!actions::can_load(st.weapon) ? "its action shut" : "full, no spare rounds, Dead Eye or a reload"),
                      xt == 1 ? ds.clip : st.clip, xt == 1 ? ds.clip_max : st.clip_max, xt == 1 ? sec_spare : st.spare, reload::hand_reload() ? 1 : 0,
                      (st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING)) ? 1 : 0);  // (R17: which of them)
        }
    };
    // [Reload] PistolMagazines: a magazine (the Mauser: a stripper clip) into controller h's hand, from the chest or by
    // RoundSource, as the rounds' take: held as round_held with round_mag, the gun's size of rounds from the spare in it
    // (the clip: its ten), nothing taken from the spare until it goes in (actions::insert_mag)
    auto take_mag = [&](int h, bool can, const char* where) {
        round_held[h] = can;
        round_mag[h] = can;
        controllers::pulse(h, can ? 0.4f : 0.15f, can ? 25 : 10);
        if (!can) {
            g_mag_refused.fetch_add(1, std::memory_order_relaxed);
            log::info("[holster] %s hand: no %s (the gun takes none now: its magazine in, the Mauser full or its clip in, no spare rounds, Dead Eye "
                      "or a reload)%s", h ? "right" : "left", st.weapon == 3 ? "stripper clip" : "magazine", where);
            return;
        }
        round_tgt[h] = 0;
        round_x[h] = false;
        round_next_ms[h] = 0.0;
        const float sp = std::floor(st.spare + 0.01f);
        round_n[h] = static_cast<int>(st.weapon == 3 ? std::fmin(10.0f, sp) : std::fmin(std::floor(st.clip_max + 0.01f), sp));
        g_rounds_taken.fetch_add(1, std::memory_order_relaxed);
        g_mag_takes.fetch_add(1, std::memory_order_relaxed);
        log::info("[holster] %s hand took %s (%d rounds; clip %.0f/%.0f, spare %.0f)%s", h ? "right" : "left",
                  st.weapon == 3 ? "a stripper clip" : "a magazine", round_n[h], st.clip, st.clip_max, st.spare, where);
    };
    // [Reload] RoundSource gun / palm: controller h (John's hand jh) may take rounds in the hand now, its grip pressed: a
    // gun in hand; the free hand, or (DualWieldCrossReload) a dual-wielding hand for the other gun (cross_tgt); out of
    // every holster's zone; not where a part wants the press (a part of the gun's action: actions::grip_wanted, the
    // bolt's among them; the foregrip; the carried gun's grip); not while placing or in the menu. gun: its grab point
    // within kNearShell of the target gun's loading ring (inside it as well); palm: turned palm up (its palm normal: the
    // IK target's -x for John's right hand, +x for his left, body.cpp hand_turn's) and outside that ring by 5 cm. Its
    // press is the holster's (g_wanted), and only while the target gun takes rounds now (else the press stays the game's
    // or the prop grab's). take_dist: the grab point's distance from the loading point that hand would load (-1: not a
    // hand that may take now, or the target takes none), its ring's radius in *ring
    auto palm_y_of = [&](int jh) {
        if (!bp.hand_ok[jh]) return -2.0f;
        const float* R = bp.target_rot[jh];
        return (jh == 1 ? -1.0f : 1.0f) * R[3];  // the x column's world y (columns: the axes; +y up)
    };
    auto take_dist = [&](int h, int jh, int zone, float* ring) {
        *ring = 0.0f;
        if (g_round_src.load(std::memory_order_relaxed) == 0 || !reload::hand_reload() || !(gun || item_x) || zone >= 0 || !bp.hand_ok[jh] || menu::visible() ||
            g_place.load(std::memory_order_relaxed) || g_piece_place.load(std::memory_order_relaxed))
            return -1.0f;
        const int xt = cross_tgt(h);
        if (xt < 0 && !(h == off_h && !(ds.on && h == ds.ctrl))) return -1.0f;
        if (xt == 1 ? !sec_pt : !bp.hand_ok[gj]) return -1.0f;  // (the review: load_pt is the gun hand's, tracked)
        if (actions::grip_wanted(h) || (h == off_h && fore_ok) || carry_near(h, jh)) return -1.0f;
        // (the review) only while the target takes rounds now (room, spare rounds, its gate; no Dead Eye, not the game's
        // reload): otherwise the press is the game's (RB: cover, Dead Eye's paint), the prop grab's or the wheel's as before
        // ([Reload] PistolMagazines: a magazine pistol's magazine, at its well)
        const bool mg = xt < 0 && magg;
        if (!(xt == 1 ? sec_loadable : mg ? mag_loadable : gun_loadable())) return -1.0f;
        const float* lp = xt == 1 ? load_sec : mg ? mag_pt : load_pt;
        *ring = xt == 1 ? lr_sec : mg ? kMagRing : lr;
        const float dx = gb[jh][0] - lp[0], dy = gb[jh][1] - lp[1], dz = gb[jh][2] - lp[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    auto hand_take_at = [&](int h, int jh, int zone) {
        float ring = 0.0f;
        const float d = take_dist(h, jh, zone, &ring);
        if (d < 0.0f) return false;
        if (g_round_src.load(std::memory_order_relaxed) == 1) return d < ring + kNearShell;
        return palm_y_of(jh) > kPalmUp && d > ring + 0.05f;
    };
    // [Reload] PhysicalStock, TriggerTakesRound: each controller's trigger (pulled, holding the gun at the foregrip, its round)
    static bool trig_on[2] = {false, false}, trig_grab[2] = {false, false}, trig_take[2] = {false, false};
    // [Hands] CarryByForegrip: the hand-over, the gun hand h let go while two-handed: the front hand carries the gun in hand
    // where it is drawn (its two-handed place, body::two_hand_P), the game's item put away, the fists selected. can_pre:
    // the caller's own gate (the hand-over's press let go away from the holsters); from: the carried gun's holster. True
    // once carried. (The hand-over's let-go and [Holsters] HoldToKeep's; logged either way)
    auto hand_over = [&](int h, bool can_pre, int from, bool quiet = false) -> bool {
        uintptr_t W = 0;
        alignas(16) float T[16];
        float hand0[3] = {}, rot0[9] = {};
        const bool can = can_pre && carry_can(h) && !trig_grab[off_h] && bp.hand_ok[gj] && rd(in_hand + 0xa0, &W) && W;
        if (can && body::two_hand_P(W, T, hand0, rot0, fj)) {  // (T holds P, the gun as drawn: dual makes T on the game thread)
            const float* R = bp.target_rot[fj];  // columns: the axes; the gun hand's grab point in the front target's frame
            const float d[3] = {gb[gj][0] - bp.hand[fj][0], gb[gj][1] - bp.hand[fj][1], gb[gj][2] - bp.hand[fj][2]};
            for (int i = 0; i < 3; ++i) carry_grip_c[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2];
            // and in the drawn gun's own frame (P's axis rows, made unit), as carry_frame gives the carried gun's
            const float dg[3] = {gb[gj][0] - T[12], gb[gj][1] - T[13], gb[gj][2] - T[14]};
            carry_grip_g_ok = true;
            for (int i = 0; i < 3; ++i) {
                const float* ax = T + i * 4;
                const float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                carry_grip_g_ok = carry_grip_g_ok && l > 1e-6f && std::isfinite(l);
                carry_grip_g[i] = carry_grip_g_ok ? (ax[0] * dg[0] + ax[1] * dg[1] + ax[2] * dg[2]) / l : 0.0f;
            }
            carry_from = from;
            dual::begin_carry(off_h, fj, cur, in_hand, W, T, hand0, rot0);
            if (dual::state().carry) {
                carry_seated = seated_now;
                const uint64_t args[1] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor))};
                api::queue_native(kPutItemAway, args, 1, 0, nullptr);
                hold_put_ms = now_ms;  // ([Holsters] HoldToKeep: no press takes it on its way out)
                const uint64_t m[2] = {1, 1};  // the fists selected, always: the game's LT does not draw the long gun again
                if (!seated_now) api::queue_native(kMeleeModeSelected, m, 2, 0, nullptr);  // (C16, the review: not riding)
                sec_from[off_h] = carry_from;
                if (st.weapon >= 0 && st.weapon < kWeapons && carry_from >= 0 && carry_from < kZones) put_zone_of[st.weapon] = carry_from;
                controllers::pulse(h, 0.3f, 20);
                controllers::pulse(off_h, 0.4f, 25);
                log::info("[holster] %s hand let go of the %s: carried by the %s hand's foregrip (from the %s)", h ? "right" : "left",
                          st.weapon >= 0 && st.weapon < kWeapons ? kWeaponLabel[st.weapon] : "?", off_h ? "right" : "left",
                          carry_from >= 0 && carry_from < kZones ? zones[carry_from].key : "?");
                return true;
            }
            if (!quiet) {  // (quiet: HoldToKeep's tries after its first, each half second)
                controllers::pulse(h, 0.15f, 10);
                log::info("[holster] %s hand let go of the two-handed gun: not carried (refused)", h ? "right" : "left");
            }
            return false;
        }
        if (!quiet)
            log::info("[holster] %s hand let go of the two-handed gun: not carried (%s; W %p, \"skel carry\": the capture)", h ? "right" : "left",
                      !can ? "no longer two-handed, or at a holster" : "its two-handed place not known", reinterpret_cast<void*>(W));
        return false;
    };
    // [Holsters] HoldToKeep (g_hold's comment): on in the holsters' mode; a hold taken by controller h (kind 1 the gun in hand,
    // 2 the second gun or the copy) of eWeapon w (-1: a slot's draw, not known yet) from holster `from` (-1: not known)
    const bool hold_on = g_hold.load(std::memory_order_relaxed) && !wheel::wheel_mode();
    // the game's own states no put-away (nor a put-back, a hand-over, a carry's end) cuts into (crash policy: none of them
    // is a path the holsters' put-away takes): busy now, the last time it was, the last mounting or dismounting
    const int hold_vf = hold_on ? vr_mode::state_flags() : 0;
    const char* const busy = !hold_on                                                     ? nullptr  // (off: nothing read)
                             : deadeye_now                                                ? "Dead Eye"
                             : controls::game_menu_open()                                ? "a game menu"
                             : (st.flags & RDRVR_ACTOR_CAM_AWAY) || (hold_vf & 16)       ? "a script's camera or a cutscene"
                             : (hold_vf & (2 | 8))                                       ? "the game paused or faded"
                             : nullptr;
    if (busy) busy_last = now_ms;
    if (seated_now != seat_was) {
        seat_was = seated_now;
        seat_ms = now_ms;
    }
    const bool hold_settled = !busy && now_ms - busy_last >= kHoldBusyGraceMs && now_ms - seat_ms >= kHoldSeatMs;
    bool hold_press[2] = {false, false}, hold_drew[2] = {false, false};  // this frame: a fresh press, and one that took (drew) something
    static int oh_up_zone[2] = {-2, -2};  // the zone a hand was at when its grip opened (-1 none, -2 the grip held)
    static double ho_ms[2] = {0.0, 0.0};  // (step 3) the hold's last hand-over by each controller (log::now_ms), 0 none
    // anim: the game's draw clip plays (InstantDraw off, a game-drawn gun taken): its put-away waits for the clip
    auto hold_take = [&](int h, int kind, int w, int from, bool anim = false) {
        hold_drew[h] = true;
        if (kind == 1) hold_take_ms = hold_take_ms_hold = now_ms;  // (C17) the gun in hand taken again: its put-away over
        if (!hold_on) return;
        keep[h] = kind;
        keep_w[h] = w;
        keep_from[h] = from;
        keep_ms[h] = now_ms;
        keep_seen[h] = false;
        keep_why[h] = nullptr;
        keep_anim[h] = kind == 1 && anim;
        keep_try_ms[h] = -1e12;
        keep_try_n[h] = 0;
        if (kind == 1 && keep[1 - h] == 1) keep[1 - h] = 0;  // (the game holds one gun in hand: this one now)
        g_hold_takes.fetch_add(1, std::memory_order_relaxed);
    };
    // a draw by controller h at holster `from` of eWeapon w: held by the grip of the hand the gun lands in. With
    // DrawToGrabbingHand (draw_any_active) that is h; else (the option off, the left-handed layout's transplant) the
    // layout's gun hand: held by its grip if that is held now, else loose (as a game-drawn gun: the gun hand's press takes it)
    // Returns the controller holding it (-1: none, loose, or the option off)
    auto hold_draw = [&](int h, int w, int from, bool anim) -> int {
        const int land = controls::draw_any_active() ? h : gun_h;
        if (land == h) {
            hold_take(h, 1, w, from, anim);
            return hold_on ? h : -1;
        }
        hold_drew[h] = true;
        if (grip_down[land]) {
            hold_take(land, 1, w, from, anim);
            return hold_on ? land : -1;
        }
        if (keep[land] == 1) keep[land] = 0;  // (its gun goes for this one, loose)
        return -1;
    };
    // the hand that holds a gun is the hold's grip from its press (g_wanted: never the game's LB/RB): the kept, the gun hand
    // with a gun in hand, the second gun's or the carried gun's hand. In Dead Eye the gun hand's grip is the game's (RB: its
    // paint at [DeadEye] Level 3; the let-go waits through Dead Eye, so opening and closing the hand paints)
    auto hold_wants = [&](int h) {
        if (!hold_on || (deadeye_now && h == gun_h)) return false;
        return keep[h] != 0 || (h == gun_h && (gun || (in_hand && keepable(st.weapon)))) || (ds.on && h == ds.ctrl);
    };
    // HoldToKeep's let-go of the gun in hand (h's grip, let go len ms): put away as at a holster (the loop's let-go there,
    // below): ACTOR_PUT_ITEM_AWAY, shown at the holster it came from, the fists; a second gun out becomes the gun in hand
    // other_go: the second gun's hand lets go too now (past its debounce, none of its own waits): both go back
    auto hold_put_away = [&](int h, double len, bool other_go) {
        const dual::State dn = dual::state();
        const int w = st.weapon >= 0 && st.weapon < kWeapons ? st.weapon : -1, from = keep_from[h] >= 0 && keep_from[h] < kZones ? keep_from[h] : -1;
        if (put_in_hand_other(w)) {  // (item 4.6) another weapon put in hand within the second: it is not put away
            g_putaway_skipped.fetch_add(1, std::memory_order_relaxed);
            keep[h] = 0;
            log::info("[holster] %s hand let go of the %s (HoldToKeep, %.0f ms): not put away, the %s put in hand within the second",
                      h ? "right" : "left", w >= 0 ? kWeaponLabel[w] : "?", len,
                      g_put_in_hand_w >= 0 && g_put_in_hand_w < kWeapons ? kWeaponLabel[g_put_in_hand_w] : "?");
            return;
        }
        g_puts.fetch_add(1, std::memory_order_relaxed);
        g_hold_puts.fetch_add(1, std::memory_order_relaxed);
        g_hold_put_len.store(static_cast<float>(len), std::memory_order_relaxed);
        g_hold_put_zone.store(from, std::memory_order_relaxed);
        if (w >= 0 && from >= 0) {
            put_zone_of[w] = from;
            if (any_here(from)) {  // (AnyWeapon: that holster holds it now, and no other)
                for (int& zi : zone_item)
                    if (zi == w) zi = -1;
                zone_item[from] = w;
            }
        }
        keep[h] = 0;
        hold_put_ms = now_ms;
        controllers::pulse(h, 0.3f, 20);
        const int oc = dn.ctrl;
        if (other_go && dn.on && !dn.copy && !dn.carry && oc >= 0 && oc <= 1 && oc != h && keep[oc] == 2 && !grip_down[oc] && up_since[oc] >= 0.0 &&
            now_ms - up_since[oc] >= hold_letgo) {
            // the second gun's hand let go too (past its debounce): both go back, the second not made the gun in hand first
            dual::end("let go with the gun in hand (HoldToKeep): back to its holster");
            g_hold_sec_puts.fetch_add(1, std::memory_order_relaxed);
            keep[oc] = 0;
            log::info("[holster] %s hand's second gun let go with the gun in hand (HoldToKeep): put back, not made the gun in hand", oc ? "right" : "left");
        } else if (dn.on && !dn.copy && !dn.carry && dn.ready && dn.weapon >= 0 && dn.weapon < kWeapons && dn.ctrl >= 0 && dn.ctrl != h) {
            const int sec_ctrl = dn.ctrl, sec_w = dn.weapon;
            dual::end("the gun in hand let go: it becomes the gun in hand (HoldToKeep)");
            const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(sec_w), 0ull};
            queue_put_in_hand(args);
            controls::set_draw_hand(sec_ctrl, sec_w, false);  // that hand, once the gun is in it
            if (keep[sec_ctrl] == 2) {  // its grip keeps it as the gun in hand now
                const int sf = sec_from[sec_ctrl];
                hold_take(sec_ctrl, 1, sec_w, sf);
            }
            log::info("[holster] %s hand let go of the %s (HoldToKeep, %.0f ms): put away; the second gun (the %s) is the gun in hand now, in the %s hand",
                      h ? "right" : "left", w >= 0 ? kWeaponLabel[w] : "?", len, kWeaponLabel[sec_w], sec_ctrl ? "right" : "left");
            return;
        }
        put_item_away(st.actor, w);  // ([Holsters] InstantPutAway: the game's instant holster)
        const bool fists = g_unarmed.load(std::memory_order_relaxed);
        if (fists) {
            const uint64_t m[2] = {1, 1};
            api::queue_native(kMeleeModeSelected, m, 2, 0, nullptr);
            g_unarmed_sets.fetch_add(1, std::memory_order_relaxed);
        }
        log::info("[holster] %s hand let go of the %s (HoldToKeep, %.0f ms): put away (back at the %s)%s", h ? "right" : "left", w >= 0 ? kWeaponLabel[w] : "?",
                  len, from >= 0 ? zones[from].key : "game's own holster", fists ? ", the fists selected" : "");
        hold_place(st, now_ms);
        if (st.valid) {
            letgo_tr.ms = now_ms;
            std::memcpy(letgo_tr.p, st.pos, sizeof(letgo_tr.p));
            letgo_tr.h = st.heading_deg;
            letgo_tr.what = w >= 0 ? kWeaponLabel[w] : "?";
        }
    };
    static bool was_down[2] = {false, false};
    int in_z[2] = {-1, -1};
    // the chest's take by controller h (its press there, or round 55's dual-wield entry below): a round (a magazine) from the
    // chest for the gun that takes it, the copy's or the second gun's squeeze, or the gun hand's (ChestSqueeze); the
    // press's own state (consumed, its zone) is the caller's
    auto chest_take = [&](int h) {
        const bool loadable = gun_loadable();
        // [Hands] DualWieldCrossReload: a dual-wielding hand takes the other gun's rounds here; its press is the cross
        // take's, not the chest squeeze's (the same gesture: with the option on the squeeze stays for one gun out and
        // for the copy)
        const int xt = cross_tgt(h);
        const int sq = xt >= 0 ? 0 : reload::chest_squeeze();
        const bool sec_hand = ds.on && h == ds.ctrl;
        if (sec_hand && sq && ds.copy && !actions::second_mag_pistol(ds)) {  // [Hands] DualWieldCopy: the copy pressed to the chest, a round of its own count (C7: not a magazine pistol's)
            round_held[h] = false;
            const bool in = reload::hand_reload() && !(st.weapon_flags & RDRVR_WEAPON_DEADEYE) && dual::load_copy_round(ds.weapon, st.spare, st.actor);
            if (in) g_squeezes.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(h, in ? 0.5f : 0.15f, in ? 25 : 10);
        } else if (sec_hand && sq && !ds.carry && !actions::second_mag_pistol(ds)) {  // the second gun pressed to the chest: one round from its own spare ammo (the game's trade; C7: not a magazine pistol)
            round_held[h] = false;
            if (ds.ready && reload::hand_reload() && !(st.weapon_flags & RDRVR_WEAPON_DEADEYE)) {
                const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(static_cast<uint32_t>(ds.weapon)),
                                          0x3F800000ull};  // 1.0f's bits (a float read with movss)
                // (2026-10-27, the cross reload's re-review: noted, a spare change as any other)
                reload::note_spare_native(api::queue_native(0xCC69DCC1, args, 3, 0, nullptr));  // ACTOR_ADD_WEAPON_AMMO
                g_squeezes.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.5f, 25);
                log::info("[wield] the second gun squeezed at the chest: one round (weapon %d, clip %.0f)", ds.weapon, ds.clip);
            } else {
                controllers::pulse(h, 0.15f, 10);
            }
        } else if (h == gun_h && sq) {  // the gun itself pressed to the chest: one round or a full clip
            round_held[h] = false;
            const bool full = sq == 2;
            const bool done = loadable && (full ? reload::fill_clip(st) : reload::insert_round(st));
            if (done) {
                if (bp.hand_ok[gj]) actions::round_in(st.weapon, bp.hand[gj]);
                float nr = full ? st.clip_max - st.clip : 1.0f;
                if (nr > st.spare) nr = st.spare;
                g_rounds_in.fetch_add(1, std::memory_order_relaxed);
                g_squeezes.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, full ? 0.8f : 0.5f, full ? 70 : 25);
                log::info("[holster] the gun hand squeezed at the chest: %s (clip %.0f -> %.0f of %.0f, spare %.0f)", full ? "a full reload" : "one round",
                          st.clip, st.clip + nr, st.clip_max, st.spare);
            } else {
                controllers::pulse(h, 0.15f, 10);  // full, no spare rounds, Dead Eye
            }
        }
        // a hand holding the second gun takes no round (DualWieldCrossReload: each dual-wielding hand takes the other
        // gun's: the gun hand the second gun's, by its own count, the second gun's hand the gun in hand's)
        const bool can = xt == 1 ? sec_loadable : xt == 0 ? loadable : loadable && h == off_h && !sec_hand;
        const bool squeezed = (h == gun_h || sec_hand) && sq;
        if (xt == 1 && actions::second_mag_wanted(ds) && !squeezed) {  // (C7) a magazine for the second pistol
            round_held[h] = round_mag[h] = true;
            round_tgt[h] = 1;
            round_x[h] = true;
            round_next_ms[h] = 0.0;
            round_n[h] = static_cast<int>(std::fmin(std::floor(ds.clip_max + 0.01f), std::floor(sec_spare + 0.01f)));
            g_mag_takes.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(h, 0.4f, 25);
            log::info("[holster] %s hand took a magazine for the second gun (%d rounds)", h ? "right" : "left", round_n[h]);
        } else if (magg && xt < 0 && h == off_h && !sec_hand) take_mag(h, mag_loadable, " at the chest");  // [Reload] PistolMagazines
        else if (!squeezed) take_rounds(h, xt, can, "");
    };
    // (2026-10-30, round 55, the user: "When reloading while dual weilding, moving a hand with a gun in it to the ammo chest
    // holster should automatically pick up the ammo") would chest_take(h) put rounds in a hand, or into a gun, now: the
    // press's own rules (cross_tgt, ChestSqueeze), and not into a full gun (the second gun's squeeze too)
    auto chest_would_take = [&](int h) {
        const int xt = cross_tgt(h);
        const int sq = xt >= 0 ? 0 : reload::chest_squeeze();
        const bool sec_hand = ds.on && h == ds.ctrl;
        const bool de = (st.weapon_flags & RDRVR_WEAPON_DEADEYE) != 0;
        if (sec_hand && sq && ds.copy) return reload::hand_reload() && !de && ds.clip + 0.5f <= ds.clip_max && st.spare >= 1.0f;
        if (sec_hand && sq && !ds.carry) return ds.ready && reload::hand_reload() && !de && ds.clip >= 0.0f && ds.clip + 0.5f <= ds.clip_max;
        if (h == gun_h && sq) return gun_loadable();
        return xt == 1 ? sec_loadable : xt == 0 && gun_loadable();
    };
    static bool auto_rounds[2] = {false, false};  // (round 55) rounds taken by the entry, held without the grip until they go in
    static double auto_ms[2] = {0.0, 0.0};          // when they were taken
    // (the review) an entry counts once the hand was tracked outside the chest's zone kChestRearmMs (a tracking blip at the
    // chest, common there, is no new entry); armed until a take there, so a gun that gets room with the hand resting in it
    // takes then
    static bool chest_armed[2] = {false, false};
    static double chest_out_ms[2] = {-1.0, -1.0};
    constexpr double kChestRearmMs = 200.0, kAutoRoundsMs = 10000.0;
    // (round 114, the user: at the poker table the grip "pulls out a gun and causes John to stand up") the table games run
    // as a minigame (poker: START_MINIGAME) or take the player's control (blackjack, liar's dice, five finger fillet,
    // horseshoes, arm wrestling: SET_PLAYER_CONTROL off), and the mod's draws are natives neither stops. Asked every 250 ms:
    // while a minigame runs no hand is at a holster's zone; while the control is held, none with empty hands (below)
    // (round 125's review) polled at the top of the frame now, before its early returns (the Holsters off, the body's
    // points missed: the gate went stale after 2 s and the fists' gate with it)
    const int gate = g_game_gate.load(std::memory_order_relaxed);
    for (int h = 0; h < 2; ++h) {  // h: the controller; jh: John's hand on it (body.cpp's mapping)
        const int jh = bp.ctrl[0] == h ? 0 : 1;
        if (bp.hand_ok[jh]) {
            // [Holsters] SteadyZones: the zone the hand was in is kept until it is kZoneExit past its radius, and wins
            // over another by kZoneSticky (the left hip's holsters overlap)
            const bool steady = g_steady_zones.load(std::memory_order_relaxed);
            float best = 1e9f;
            for (int z = 0; z < kZones; ++z) {
                if (!zok[z]) continue;
                const float dx = gb[jh][0] - zp[z][0], dy = gb[jh][1] - zp[z][1], dz = gb[jh][2] - zp[z][2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                const bool was = steady && z == zone_in[h];
                const float score = was ? d - kZoneSticky : d;
                if (d < zones[z].radius + (was ? kZoneExit : 0.0f) && score < best) {
                    best = score;
                    in_z[h] = z;
                }
            }
        }
        // (round 114) a minigame closes every zone; the control held only a draw into empty hands (the review: a mission's
        // hold may still let John shoot: his put-aways and the reloads' takes stay)
        // (round 128, the review: not by the table games found by the control held in a gringo: a bed, a camp fire or a
        // mission's use spot holds the control as long, and the control held already refuses a draw into empty hands)
        // (round 139) nor on a mounted gun in first person: the grips take its handle and its crank
        if ((gate & 1) || g_turret.active || ((gate & 2) && !in_hand && !ds.on && !ds.carry && !offhand::held(h))) in_z[h] = -1;
        {  // (round 143) [Holsters] WheelAtHolster: this hand empty (not the gun's, nothing of the off hand's, its grip open)
           // at a holster of weapons, no table game or held control: its trigger may open the wheel (wheel.cpp)
            // (the review: with a weapon in either hand the other trigger has its own jobs: the game's aim, the lasso's, a bolt's)
            const bool armed = in_hand && st.weapon >= 0;
            const bool free_hand = !armed && !ds.on && !offhand::held(0) && !offhand::held(1) && hands::get(h).grip < 0.3f;
            const int wz = g_wheel_hol.load(std::memory_order_relaxed) && !wheel::wheel_mode() && !(gate & 3) && free_hand && in_z[h] >= 0 &&
                                   zone_takes_weapons(in_z[h])
                               ? in_z[h]
                               : -1;
            g_wheel_zone[h].store(wz, std::memory_order_relaxed);
            g_wheel_zone_ms.store(log::now_ms(), std::memory_order_relaxed);
        }
        {  // ([Reload] RoundSource: the status line's)
            float ring = 0.0f;
            g_palm_y[h].store(palm_y_of(jh), std::memory_order_relaxed);
            g_near_d[h].store(take_dist(h, jh, in_z[h], &ring), std::memory_order_relaxed);
            g_near_max[h].store(ring + kNearShell, std::memory_order_relaxed);
        }
        {  // (round 55: the dual-wield chest take's entry)
            const bool in_chest = in_z[h] >= 0 && zone_rounds(in_z[h]);
            if (in_chest) {
                chest_out_ms[h] = -1.0;
            } else if (bp.hand_ok[jh]) {
                if (chest_out_ms[h] < 0.0) chest_out_ms[h] = now_ms;
                if (now_ms - chest_out_ms[h] >= kChestRearmMs) chest_armed[h] = true;
            } else {
                chest_out_ms[h] = -1.0;  // (the review: the 200 ms tracked outside, unbroken)
            }
        }
        if (in_z[h] != zone_in[h]) {
            if (in_z[h] >= 0) {
                controllers::pulse(h, 0.35f, 30);
                g_entries.fetch_add(1, std::memory_order_relaxed);
            }
            zone_in[h] = in_z[h];
        }
        if (sec_wait[h].slot >= 0) {  // the slot holds the holster's gun, its model in: the second gun now (3 s at most)
            SecWait& sw = sec_wait[h];
            const double waited = log::now_ms() - sw.since;
            // (the review: a swap's gun is still in hand the frame it is asked; and a frame with nothing in hand between the gun
            // and the torch does not cancel it. r313 B7: nor ends it, its gun in the slot before the torch is in hand: a swap's
            // gun is ready once the torch is)
            const bool ready = slot_gun(sw.slot) == sw.weapon && slot_loaded(sw.slot) &&
                               (!sw.swap || (in_hand && st.weapon != sw.weapon && sec_beside));
            const bool beside_ok = sec_beside || (sw.swap && !in_hand);
            if (ready || waited > 3000.0 || ds.on || !beside_ok) {
                if (ready && !ds.on && sec_beside) {
                    dual::begin(h, jh, sw.slot, zones[sw.zone].key);
                    sec_from[h] = sw.zone;
                    controllers::pulse(h, 0.5f, 30);
                    if (waited > 30.0) log::info("[wield] the %s ready in slot %d after %.0f ms", kWeaponLabel[sw.weapon], sw.slot, waited);
                } else if (!ready && sw.swap) {  // (the review: the slot ready, the torch never in hand)
                    controllers::pulse(h, 0.15f, 10);
                    log::info("[wield] the torch swap's %s: no second gun after %.0f ms (in hand %d, weapon %d, beside %d)", kWeaponLabel[sw.weapon], waited,
                              in_hand ? 1 : 0, in_hand ? st.weapon : -1, sec_beside ? 1 : 0);
                } else if (!ready) {
                    controllers::pulse(h, 0.15f, 10);
                    log::info("[wield] the %s not ready in slot %d after %.0f ms (it holds the %s, model %s): no second gun", kWeaponLabel[sw.weapon],
                              sw.slot, waited, slot_gun(sw.slot) >= 0 && slot_gun(sw.slot) < kWeapons ? kWeaponLabel[slot_gun(sw.slot)] : "?",
                              slot_loaded(sw.slot) ? "in" : "not in");
                }
                sw.slot = -1;
            }
        }
        if (ds.on && h == ds.ctrl) {
            if (sec_zone[h] == -2) sec_zone[h] = in_z[h];
            if (in_z[h] < 0 || in_z[h] != sec_zone[h]) sec_armed[h] = true;
        } else {
            sec_zone[h] = -2;
            sec_armed[h] = false;
        }
        {  // [Reload] PhysicalStock: the front hand's trigger pulled at the foregrip (the grip not held) holds the gun as the
           // grip does, while it is pulled (a trigger already pulled as the hand reaches the foregrip takes it too)
            const bool tr = hands::get(h).trigger > (trig_on[h] ? 0.4f : 0.6f);
            const bool tr_press = tr && !trig_on[h];
            trig_on[h] = tr;
            // [Reload] TriggerTakesRound: a fresh press at the ammo zone (a trigger already held to aim takes nothing on
            // the way past the chest)
            if (tr_press && !trig_grab[h] && !grip_down[h] && !was_down[h] && h == off_h && gun && zone_rounds(in_z[h]) &&
                !(ds.on && h == ds.ctrl) && g_trig_round.load(std::memory_order_relaxed) && reload::hand_reload() && !menu::visible() &&
                !g_place.load(std::memory_order_relaxed) && !g_piece_place.load(std::memory_order_relaxed)) {
                trig_grab[h] = trig_take[h] = true;
                log::info("[holster] %s trigger pressed at the ammo zone (TriggerTakesRound)", h ? "right" : "left");
            }
            if (tr && !trig_grab[h] && !grip_down[h] && !was_down[h] && h == off_h && in_z[h] < 0 && fore_ok && !two_hold[h] &&
                !(ds.on && h == ds.ctrl) && g_stock.load(std::memory_order_relaxed) && !menu::visible()) {
                trig_grab[h] = true;
                log::info("[holster] %s trigger at the foregrip: the gun held (physical stock)", h ? "right" : "left");
            }
            if (!tr) trig_grab[h] = trig_take[h] = false;
            g_trig_for_round[h].store(g_trig_round.load(std::memory_order_relaxed) && (trig_take[h] || (h == off_h && zone_rounds(in_z[h]))),
                                      std::memory_order_relaxed);
        }
        // [Holsters] HoldToKeep: a hand holding a gun by its grip (kept, or carrying) is down until its let-go has lasted
        // kHoldLetGoMs: a blip is neither a let-go nor a press here (no rounds dropped, no draw at a zone it passes)
        // (2026-10-31, the off-hand build step 2) an off-hand item (OffHandItems) the same, HoldToKeep or not (r79 s7: a 48 ms
        // dip put the knife back)
        const bool held_on = ((hold_on && (keep[h] != 0 || (ds.carry && h == ds.ctrl))) || offhand::held(h)) && !grip_down[h] && up_since[h] >= 0.0 &&
                             now_ms - up_since[h] < hold_letgo;
        if (grip_down[h]) oh_up_zone[h] = -2;           // (the off-hand item's release: the zone at the grip's opening)
        else if (oh_up_zone[h] == -2) oh_up_zone[h] = in_z[h];
        const bool down = grip_down[h] || trig_grab[h] || held_on, pressed = down && !was_down[h], released = !down && was_down[h];
        was_down[h] = down;
        hold_press[h] = pressed && grip_down[h];  // (HoldToKeep: the grip's own press, not a trigger's hold)
        if (released && round_held[h]) rounds_let_ms[h] = now_ms;  // (HoldToKeep: the rounds' let-go, not the gun's)
        // [Hands] OffHandItems: the free hand's grip pressed at the knife's or the satchel's zone with a gun in the other
        // hand (not the second gun's hand, not carrying): the item it would draw (the branch below takes it)
        // (2026-10-31, the off-hand build step 5, O4) beside an item too: the knife, the torch or a throwable in hand (never
        // the lasso: answer 10), the copy never the same item as the one in hand
        const bool oh_base = gun || (in_hand && hold_item_kind(st.weapon) && st.weapon != 21);
        const int oh_pick = pressed && grip_down[h] && in_z[h] >= 0 && h == off_h && oh_base && offhand::enabled() && !(ds.on && h == ds.ctrl) &&
                                    !ds.carry && !offhand::held(h)
                                ? offhand_item(in_z[h])
                                : -1;
        const int oh_w = oh_pick >= 0 && !(!gun && oh_pick == st.weapon) ? oh_pick : -1;
        // ([Holsters] HoldToKeep: after its let-go's debounce, kHoldLetGoMs, as the gun hand's)
        // (and, with HoldToKeep, not during the game's own states: hold_settled)
        if (ds.carry && h == ds.ctrl && !grip_down[h] && (!hold_on || (now_ms - up_since[h] >= hold_letgo && hold_settled))) {  // [Hands] CarryByForegrip: the carried gun let go: at its holster
            dual::end("the front hand let go");
            controllers::pulse(h, 0.3f, 20);
        }
        {  // run 12 item 3: John's belt's pieces placed by hand: a grip within 10 cm of a piece's joint takes it; while held
           // it follows the hand's move and turn since the grip (body::piece_grab); letting go keeps it (saved). 2026-10-13
           // (the user: "Need to be able to rotate guns by hand too, so they better fit the holsters"): a gun shown at its
           // holster too, within 12 cm of it: it follows the hand rigidly; let go: its GunTurn and GunOffset saved
            static int grab_p[2] = {-1, -1};
            static int grab_g[2] = {-1, -1};  // the shown gun held (its kShowZones index)
            static int grab_t[2] = {-1, -1};  // (round 120) the throwable held (its kThrowables index)
            static float T0[2][12];
            static float h0[2][3], R0[2][9], G0[2][12];
            static body::Piece p0[2];
            if (!g_piece_place.load(std::memory_order_relaxed)) {
                grab_p[h] = -1;
                grab_g[h] = -1;
                grab_t[h] = -1;
            } else {
                const float* anc_t = bp.bone_ok[body::kPelvisPoint] ? bp.bone[body::kPelvisPoint] : bp.root;
                auto thr_pose = [&](int w, float* tp, bool adjusted) {  // (round 120) the shown throwable's pose this frame
                    float an[3] = {anc_t[0], anc_t[1], anc_t[2]};
                    return belt_prop_pose(1, hr, up, hf, anc_t, tp, an, false, w, adjusted);
                };
                auto gun_pose = [&](int i, float* gp, bool adjusted) {  // a shown gun's pose this frame (12 floats)
                    const int zi = kShowZones[i];
                    show_pose(zi, zp[zi], hr, up, hf, bp.root, gp);
                    if (adjusted) {
                        float tn[3], go[3];
                        {
                            std::lock_guard lock(g_zone_mutex);
                            std::memcpy(tn, g_zones[zi].gun_turn, sizeof(tn));
                            std::memcpy(go, g_zones[zi].gun_off, sizeof(go));
                        }
                        adjust_pose(tn, go, zp[zi], hr, up, hf, gp);
                    }
                };
                if (pressed && bp.hand_ok[jh]) {
                    int bi = -1;
                    float bd = 0.10f * 0.10f;
                    for (int i = 0; i <= body::kPieces; ++i) {  // (kPieces: the mirrored left holster, 2026-10-13)
                        float j[3];
                        if (i == body::kPieces ? !body::left_holster_where(j) : !body::piece_where(i, j)) continue;
                        const float dx = j[0] - gb[jh][0], dy = j[1] - gb[jh][1], dz = j[2] - gb[jh][2];
                        if (dx * dx + dy * dy + dz * dz < bd) bd = dx * dx + dy * dy + dz * dz, bi = i;
                    }
                    int gi = -1;
                    float gd = 0.12f * 0.12f;
                    if (log::now_ms() - g_shown_ms < 250.0)
                        for (int i = 0; i < 4; ++i) {
                            if (g_shown_last[i] < 0 || !zok[kShowZones[i]]) continue;
                            float gp[12];
                            gun_pose(i, gp, true);
                            const float dx = gp[9] - gb[jh][0], dy = gp[10] - gb[jh][1], dz = gp[11] - gb[jh][2];
                            if (dx * dx + dy * dy + dz * dz < gd) gd = dx * dx + dy * dy + dz * dz, gi = i;
                        }
                    int ti = -1;  // (round 120) the throwable shown at the satchel's place
                    float td = 0.12f * 0.12f;
                    const int tw = g_thr_shown_w;
                    if (tw >= 0 && throwable_index(tw) >= 0 && log::now_ms() - g_thr_shown_ms < 250.0) {
                        float tp[12];
                        if (thr_pose(tw, tp, true)) {
                            // (the review) its place and its belt point (the tomahawk hangs 0.30 m below it: its head at the belt)
                            const float drop = thrown_on_bone(tw).drop;
                            const float bx = tp[9] - up[0] * drop, by = tp[10] - up[1] * drop, bz = tp[11] - up[2] * drop;
                            const float dx = tp[9] - gb[jh][0], dy = tp[10] - gb[jh][1], dz = tp[11] - gb[jh][2];
                            const float ex = bx - gb[jh][0], ey = by - gb[jh][1], ez = bz - gb[jh][2];
                            const float dd = std::fmin(dx * dx + dy * dy + dz * dz, ex * ex + ey * ey + ez * ez);
                            if (dd < td) {
                                td = dd;
                                ti = throwable_index(tw);
                                std::memcpy(T0[h], tp, sizeof(T0[h]));
                            }
                        }
                    }
                    if (ti >= 0 && (bi < 0 || td < bd) && (gi < 0 || td < gd)) {
                        grab_t[h] = ti;
                        std::memcpy(h0[h], gb[jh], sizeof(h0[h]));
                        std::memcpy(R0[h], bp.target_rot[jh], sizeof(R0[h]));
                        controllers::pulse(h, 0.5f, 30);
                        log::info("[holster] placing the %s on the belt by hand (the %s hand)", kWeaponLabel[kThrowableW[ti]], h ? "right" : "left");
                    } else if (gi >= 0 && (bi < 0 || gd < bd)) {
                        grab_g[h] = gi;
                        std::memcpy(h0[h], gb[jh], sizeof(h0[h]));
                        std::memcpy(R0[h], bp.target_rot[jh], sizeof(R0[h]));
                        gun_pose(gi, G0[h], true);
                        controllers::pulse(h, 0.5f, 30);
                        log::info("[holster] placing the %s's gun by hand (the %s hand)", zones[kShowZones[gi]].key, h ? "right" : "left");
                    } else if (bi >= 0) {
                        grab_p[h] = bi;
                        std::memcpy(h0[h], gb[jh], sizeof(h0[h]));
                        std::memcpy(R0[h], bp.target_rot[jh], sizeof(R0[h]));
                        p0[h] = bi == body::kPieces ? body::left_holster_piece() : body::piece(bi);
                        controllers::pulse(h, 0.5f, 30);
                        log::info("[holster] placing John's %s by hand (the %s hand)", bi == body::kPieces ? "left holster" : body::piece_key(bi),
                                  h ? "right" : "left");
                    }
                }
                float Rd[9];  // the hand's turn since the grip: R_now R0^T (the columns its axes)
                {
                    const float* Rn = bp.target_rot[jh];
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            Rd[r * 3 + c] = Rn[r * 3] * R0[h][c * 3] + Rn[r * 3 + 1] * R0[h][c * 3 + 1] + Rn[r * 3 + 2] * R0[h][c * 3 + 2];
                }
                const int gg = grab_g[h];
                if (gg >= 0 && down && bp.hand_ok[jh]) {
                    // the gun rigid with the hand: its axes turned by Rd, its point carried; then the zone's GunTurn (in the
                    // holster's axes: x right, y up, z back, a right-handed frame: Ry(turn) Rx(tilt) Rz(-lean), as
                    // adjust_pose turns it) and GunOffset (right, up, forward) solved against this frame's plain pose
                    const int zi = kShowZones[gg];
                    float G[9], P[3], B[12];
                    for (int a = 0; a < 3; ++a)
                        for (int k = 0; k < 3; ++k) G[a * 3 + k] = Rd[k * 3] * G0[h][a * 3] + Rd[k * 3 + 1] * G0[h][a * 3 + 1] + Rd[k * 3 + 2] * G0[h][a * 3 + 2];
                    {
                        const float q[3] = {G0[h][9] - h0[h][0], G0[h][10] - h0[h][1], G0[h][11] - h0[h][2]};
                        for (int k = 0; k < 3; ++k) P[k] = Rd[k * 3] * q[0] + Rd[k * 3 + 1] * q[1] + Rd[k * 3 + 2] * q[2] + gb[jh][k];
                    }
                    gun_pose(gg, B, false);
                    float M[9];  // world: the plain pose's axes onto the held ones (M = sum over the axes of g b^T)
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) M[r * 3 + c] = G[r] * B[c] + G[3 + r] * B[3 + c] + G[6 + r] * B[6 + c];
                    const float* F[3] = {hr, up, nullptr};
                    const float back[3] = {-hf[0], -hf[1], -hf[2]};
                    F[2] = back;
                    float Ml[9];  // F^T M F
                    for (int a = 0; a < 3; ++a)
                        for (int b = 0; b < 3; ++b) {
                            float v = 0.0f;
                            for (int r = 0; r < 3; ++r)
                                for (int c = 0; c < 3; ++c) v += F[a][r] * M[r * 3 + c] * F[b][c];
                            Ml[a * 3 + b] = v;
                        }
                    const float sa = std::fmax(-1.0f, std::fmin(1.0f, -Ml[5]));
                    float tn[3] = {std::asin(sa) * 57.29578f, std::atan2(Ml[2], Ml[8]) * 57.29578f, -std::atan2(Ml[3], Ml[4]) * 57.29578f};
                    const float* at = zp[zi];
                    const float rel[3] = {B[9] - at[0], B[10] - at[1], B[11] - at[2]};
                    float v[3];
                    for (int k = 0; k < 3; ++k) v[k] = P[k] - at[k] - (M[k * 3] * rel[0] + M[k * 3 + 1] * rel[1] + M[k * 3 + 2] * rel[2]);
                    float go[3] = {v[0] * hr[0] + v[1] * hr[1] + v[2] * hr[2], v[0] * up[0] + v[1] * up[1] + v[2] * up[2],
                                   v[0] * hf[0] + v[1] * hf[1] + v[2] * hf[2]};
                    set_zone_gun(zi, tn, go, false);
                }
                if (gg >= 0 && !down) {
                    const int zi = kShowZones[gg];
                    float tn[3], go[3];
                    zone_gun(zi, tn, go);
                    set_zone_gun(zi, tn, go, true);
                    controllers::pulse(h, 0.3f, 20);
                    log::info("[holster] the %s's gun placed by hand: angle (%.1f %.1f %.1f) place (%.3f %.3f %.3f)", zones[zi].key, tn[0], tn[1], tn[2], go[0],
                              go[1], go[2]);
                    grab_g[h] = -1;
                }
                const int gt = grab_t[h];
                if (gt >= 0 && down && bp.hand_ok[jh]) {  // (round 120) the throwable rigid with the hand; its own place solved
                    float B[12];
                    if (thr_pose(kThrowableW[gt], B, false)) {
                        float D[9], P[3];  // the held axes (as pz: X, Y, Z) and place
                        for (int a = 0; a < 3; ++a)
                            for (int k = 0; k < 3; ++k) D[a * 3 + k] = Rd[k * 3] * T0[h][a * 3] + Rd[k * 3 + 1] * T0[h][a * 3 + 1] + Rd[k * 3 + 2] * T0[h][a * 3 + 2];
                        {
                            const float q[3] = {T0[h][9] - h0[h][0], T0[h][10] - h0[h][1], T0[h][11] - h0[h][2]};
                            for (int k = 0; k < 3; ++k) P[k] = Rd[k * 3] * q[0] + Rd[k * 3 + 1] * q[1] + Rd[k * 3 + 2] * q[2] + gb[jh][k];
                        }
                        float Mw[9];  // world: the plain pose's axes onto the held ones (unit axes: a throwable is drawn at its size)
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c) Mw[r * 3 + c] = D[r] * B[c] + D[3 + r] * B[3 + c] + D[6 + r] * B[6 + c];
                        const float* F[3] = {B, B + 3, B + 6};  // (the review) in its own axes as placed without the adjustment
                        float a[12];
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j) {
                                float v = 0.0f;
                                for (int r = 0; r < 3; ++r)
                                    for (int c = 0; c < 3; ++c) v += F[i][r] * Mw[r * 3 + c] * F[j][c];
                                a[i * 3 + j] = v;
                            }
                        const float o[3] = {P[0] - B[9], P[1] - B[10], P[2] - B[11]};
                        for (int i = 0; i < 3; ++i) a[9 + i] = o[0] * F[i][0] + o[1] * F[i][1] + o[2] * F[i][2];
                        if (thr_adj_ok(a)) {
                            std::lock_guard lock(g_bp_mutex);
                            std::memcpy(g_thr_adj[gt], a, sizeof(a));
                        }
                    }
                }
                if (gt >= 0 && !down) {
                    float a[12];
                    {
                        std::lock_guard lock(g_bp_mutex);
                        std::memcpy(a, g_thr_adj[gt], sizeof(a));
                    }
                    char v[200];
                    std::snprintf(v, sizeof(v), "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f %.3f", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                                  a[8], a[9], a[10], a[11]);
                    config::set("Holsters", (std::string("OwnBeltThrowable") + kThrowableKey[gt] + "Pose").c_str(), v);
                    g_thr_placed.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(h, 0.3f, 20);
                    log::info("[holster] the %s placed on the belt by hand: moved (%.3f %.3f %.3f) m", kWeaponLabel[kThrowableW[gt]], a[9], a[10], a[11]);
                    grab_t[h] = -1;
                }
                const int gp = grab_p[h];
                if (gp >= 0 && down && bp.hand_ok[jh]) {
                    body::Piece np;
                    const body::Piece was = gp == body::kPieces ? body::left_holster_piece() : body::piece(gp);
                    const bool ok = gp == body::kPieces ? body::left_holster_grab(Rd, h0[h], gb[jh], p0[h], &np) : body::piece_grab(gp, Rd, h0[h], gb[jh], p0[h], &np);
                    if (ok) {
                        float dw[3];  // RingsFollowHolsters: its rings moved with it, the same way
                        if (g_rings_follow.load(std::memory_order_relaxed) && body::piece_shift_world(was, np, dw)) link_piece_moved(gp, dw, -1);
                        if (gp == body::kPieces) body::set_left_holster_piece(np, false);
                        else body::set_piece(gp, np, false);
                    }
                }
                if (gp >= 0 && !down) {
                    body::Piece p;
                    if (gp == body::kPieces) {
                        body::save_left_holster_piece();
                        p = body::left_holster_piece();
                    } else {
                        body::save_piece(gp);
                        p = body::piece(gp);
                    }
                    if (g_rings_follow.load(std::memory_order_relaxed)) link_save(gp, -1);
                    g_pieces_placed.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(h, 0.3f, 20);
                    log::info("[holster] John's %s placed by hand: offset (%.3f %.3f %.3f) turn (%.1f %.1f %.1f)",
                              gp == body::kPieces ? "left holster" : body::piece_key(gp), p.d[0], p.d[1], p.d[2], p.turn[0], p.turn[1], p.turn[2]);
                    grab_p[h] = -1;
                }
                g_piece_held[h].store(grab_p[h], std::memory_order_relaxed);
                if (grab_p[h] >= 0 || grab_g[h] >= 0 || grab_t[h] >= 0) {
                    g_consumed[h] = true;
                    press_zone[h] = -1;
                    pending[h] = false;
                    continue;  // no draws, put-aways or rounds while holding a piece or a gun
                }
            }
        }
        {  // placing the holsters by hand: a grip in a ring takes that holster, the hand's move moves it, letting go keeps it
            static int place_z[2] = {-1, -1};
            if (!g_place.load(std::memory_order_relaxed)) {
                place_z[h] = -1;
            } else {
                static int ring_piece[2] = {-1, -1};  // RingsFollowHolsters: the piece moved with the ring (kPieces: the left holster)
                if (pressed && in_z[h] >= 0) {
                    place_z[h] = in_z[h];
                    controllers::pulse(h, 0.5f, 30);
                    ring_piece[h] = link_piece(in_z[h]);
                    log::info("[holster] placing the %s by hand (the %s hand)%s", zones[in_z[h]].key, h ? "right" : "left",
                              ring_piece[h] >= 0 ? ": its holster with it (RingsFollowHolsters)" : "");
                }
                const int pz = place_z[h];
                if (pz >= 0 && down && bp.hand_ok[jh]) {
                    // this frame's zone at zp with its offset; the hand at gb: the offset moved by the difference, in the
                    // holster's frame (right, up, forward; mirrored when left-handed); with RingsFollowHolsters its piece
                    // (and the piece's other rings) moved the same way
                    const float d[3] = {gb[jh][0] - zp[pz][0], gb[jh][1] - zp[pz][1], gb[jh][2] - zp[pz][2]};
                    float loc[3] = {d[0] * hr[0] + d[2] * hr[2], d[1], d[0] * hf[0] + d[2] * hf[2]};
                    if (lefty) loc[0] = -loc[0];
                    {
                        std::lock_guard lock(g_zone_mutex);
                        float* zo = zone_off(g_zones[pz]);
                        for (int k = 0; k < 3; ++k) zo[k] = std::fmin(0.8f, std::fmax(-0.8f, zo[k] + loc[k]));
                    }
                    if (ring_piece[h] >= 0) link_zone_moved(pz, d);
                }
                if (pz >= 0 && !down) {
                    save_zone(pz);
                    if (ring_piece[h] >= 0) link_save(ring_piece[h], pz);
                    g_placed.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(h, 0.3f, 20);
                    float o[3];
                    {
                        std::lock_guard lock(g_zone_mutex);
                        std::memcpy(o, zone_off(g_zones[pz]), sizeof(o));
                    }
                    log::info("[holster] the %s placed by hand: offset (%.3f %.3f %.3f)%s%s", zones[pz].key, o[0], o[1], o[2],
                              zone_on_copy(zones[pz]) ? " (LeftHipRingOffset: on the left holster)" : "",
                              ring_piece[h] >= 0 ? " with its holster" : "");
                    place_z[h] = -1;
                    ring_piece[h] = -1;
                }
                g_consumed[h] = place_z[h] >= 0;
                press_zone[h] = place_z[h];
                pending[h] = false;
                carry_press[h] = false;  // (the review: a hand-over's press cut short here never acts at a later let-go)
                continue;  // no draws, put-aways or rounds while placing
            }
        }
        const bool reheld = grip_down[h] && ho_ms[h] > 0.0 && now_ms - ho_ms[h] < 300.0;  // (HandOverLetGoMs' blip)
        // (2026-11-04, GOAL-next-items item 2; the user: the Pump-action fired, carried and pumped one-handed: "you cannot
        // grab the grip again"; not in the simulator) a grip within 25 cm of the carried gun's take-back point that does not
        // take it back, pressed there or brought there already held (the review): said with why (at most one each 2 s, 50 a
        // session); the distance from the take-back point, or within 60 cm of the carrying hand when there is none (take_ok 0)
        static bool tb_held_in[2] = {false, false};  // (the review: cleared between carries)
        if (!ds.carry) tb_held_in[h] = false;
        if (ds.carry && h != ds.ctrl && bp.hand_ok[jh] && !carry_near(h, jh)) {  // (not the carrying hand's own grip)
            static double said_ms = -1e9;
            static int said = 0;
            bool* held_in = tb_held_in;
            const bool carrier_ok = ds.john >= 0 && ds.john <= 1 && bp.hand_ok[ds.john];
            const float* ref = take_ok ? take_w : carrier_ok ? bp.hand[ds.john] : nullptr;
            float d2 = 1e9f;
            if (ref) {
                d2 = 0.0f;
                for (int k = 0; k < 3; ++k) d2 += (gb[jh][k] - ref[k]) * (gb[jh][k] - ref[k]);
            }
            const float rad = take_ok ? 0.25f : 0.6f;  // (from the carrying hand: the gun's grip 30-50 cm from it)
            const bool close_by = d2 < rad * rad;  // (not `near`: a Windows macro)
            const bool held_enter = grip_down[h] && !pressed && close_by && !held_in[h];
            held_in[h] = grip_down[h] && close_by;
            if (((pressed && close_by) || held_enter) && said < 50 && now_ms - said_ms >= 2000.0) {
                said_ms = now_ms;
                ++said;
                log::info("[holster] the %s hand's grip (%s) %.3f m from the carried gun's %s (weapon %d), not taken back: the point %d "
                          "(returning %d, the carrying hand tracked %d), the carrying controller %d, in hand %d, the action wants this "
                          "grip %d, from the gun's frame %d, the menu %d",
                          h ? "right" : "left", pressed ? "pressed" : "already held", std::sqrt(d2), take_ok ? "take-back point" : "carrying hand",
                          ds.carry_weapon, take_ok ? 1 : 0, dual::carry_returning() ? 1 : 0, carrier_ok ? 1 : 0, h == ds.ctrl ? 1 : 0,
                          in_hand ? 1 : 0, actions::grip_wanted(h) ? 1 : 0, take_gun ? 1 : 0, menu::visible() ? 1 : 0);
            }
        }
        if ((pressed || reheld) && carry_near(h, jh) && !menu::visible()) {
            ho_ms[h] = 0.0;
            // [Hands] CarryByForegrip: the empty hand at the carried gun's grip takes it back: drawn at once into this hand
            // (dual places it at the carry until the game has it in hand), two-handed again if the front hand still grips
            g_consumed[h] = true;
            press_zone[h] = -1;
            pending[h] = false;
            const int w = ds.carry_weapon;
            if (w >= 0 && w < kWeapons) {
                const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(w), 0ull};
                queue_put_in_hand(args);
                controls::set_draw_hand(h, w, true);
                drawn_from = carry_from;
                dual::carry_return();
                retwo[ds.ctrl] = true;
                retwo_ms = now_ms;
                hold_take(h, 1, w, carry_from);  // ([Holsters] HoldToKeep: held by this grip from now)
                g_draws.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.5f, 30);
                log::info("[holster] %s hand at the carried %s's grip: taken back", h ? "right" : "left", kWeaponLabel[w]);
            } else {
                controllers::pulse(h, 0.15f, 10);
                log::info("[holster] %s hand at the carried gun's grip: its weapon not known (%d), not taken back", h ? "right" : "left", w);
            }
        } else if (pressed && !hold_on && in_z[h] < 0 && carry_can(h) && !trig_grab[off_h]) {
            // [Hands] CarryByForegrip: the gun hand's grip pressed away from the holsters while two-handed: let go, the
            // front hand carries the long gun (below, at the let-go). ([Holsters] HoldToKeep: its grip is the gun's hold,
            // the hand-over at its let-go, at once; a press on a gun no grip keeps takes it, after the loop)
            carry_press[h] = true;
            carry_press_ms[h] = now_ms;
            carry_cover[h] = false;
            // the holster's with Mode=wheel only (the review: a consumed grip is held back from the game, controls.cpp,
            // whatever g_wanted says: otherwise the press stays the game's RB throughout)
            g_consumed[h] = wheel::wheel_mode();
            press_zone[h] = -1;
            pending[h] = false;
            log::info("[holster] %s hand's grip pressed on the two-handed %s: let go to leave it in the front hand", h ? "right" : "left",
                      st.weapon >= 0 && st.weapon < kWeapons ? kWeaponLabel[st.weapon] : "?");
        } else if (pressed && zone_rounds(in_z[h]) && h == actions::ramrod_taker()) {  // (C15h: or the gun hand's, the gun carried)
            // (C15, [Reload] RamrodAtChest) the Blunderbuss's round down the barrel: the ramrod taken at the chest
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            actions::take_ramrod(h);
            controllers::pulse(h, 0.35f, 20);
        } else if (pressed && zone_rounds(in_z[h])) {
            // a round from the chest for the gun in the other hand (the game's count: the gun not full, spare rounds)
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            chest_take(h);
        } else if (pressed && in_z[h] < 0 && h == off_h && fore_ok) {
            two_hold[h] = true;
            g_consumed[h] = true;
            g_two_grips.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(h, 0.25f, 20);
            if (bp.fore_ok)
                log::info("[holster] %s hand on the foregrip (%.2f m from its point, the game's grip): two-handed", h ? "right" : "left", fore_d);
            else
                log::info("[holster] %s hand on the foregrip (%.2f m ahead, %.2f off the barrel): two-handed", h ? "right" : "left", fore_along,
                          fore_perp);
        } else if (pressed && grip_down[h] && hand_take_at(h, jh, in_z[h])) {
            // [Reload] RoundSource gun / palm: the rounds appear in the hand gripped near the gun, or turned palm up (the
            // chest's take, without the reach); gun: gripped inside the ring itself, they go straight in below (as rounds
            // brought to it)
            const bool by_gun = g_round_src.load(std::memory_order_relaxed) == 1;
            g_consumed[h] = true;
            press_zone[h] = -1;
            pending[h] = false;
            const int xt = cross_tgt(h);
            const bool mg = xt < 0 && magg;  // ([Reload] PistolMagazines: a magazine appears instead)
            const bool can = xt == 1 ? sec_loadable : mg ? mag_loadable : gun_loadable();
            if (can) g_hand_takes.fetch_add(1, std::memory_order_relaxed);
            else g_hand_refused.fetch_add(1, std::memory_order_relaxed);
            if (mg) take_mag(h, can, by_gun ? " | in the hand, near the gun (RoundSource=gun)" : " | in the hand, palm up (RoundSource=palm)");
            else take_rounds(h, xt, can, by_gun ? " | in the hand, near the gun (RoundSource=gun)" : " | in the hand, palm up (RoundSource=palm)");
            if (!can && xt < 0 && !mg)
                log::info("[holster] %s hand %s: no round (RoundSource=%s; %s)", h ? "right" : "left", by_gun ? "near the gun" : "palm up",
                          by_gun ? "gun" : "palm",
                          !actions::can_load(st.weapon) ? "its action shut" : "full, no spare rounds, Dead Eye or a reload");
        } else if (((oh_w == 33 && g_torch_swap.load(std::memory_order_relaxed)) || (oh_w == 22 && g_knife_swap.load(std::memory_order_relaxed))) &&
                   dual::beside_torch(oh_w) && gun && in_hand && cur >= 0 && cur <= 7 && !ds.on &&
                   controls::draw_any_active()) {  // (the review: the torch must land in the hand that drew it)
            // (2026-10-31, the off-hand build step 5; the user's answer 9, "Torch in hand beside gun") the torch drawn by
            // the off hand with a gun in hand: the torch becomes the game's weapon (its own hit, fire and flame: the game's
            // melee), drawn in this hand at once; the gun becomes the second gun in the gun hand (DualWieldBesideTorch),
            // held by that hand's grip. Not the off-hand copy
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            const int gw = st.weapon;
            // the second gun's take as at a holster: begun once its slot holds it holstered (the torch's draw puts it there),
            // held by the gun hand's grip meanwhile (sec_wait, below: within 3 s)
            const int gz = drawn_from >= 0 && drawn_from < kZones ? drawn_from : in_z[h];
            sec_wait[gun_h] = SecWait{cur, gw, gz, log::now_ms(), true};
            sec_from[gun_h] = gz;
            drawn_from = in_z[h];  // (the review: the item in hand's holster is the torch's now)
            // (C2, KnifeSwap: the knife the same way, its stab the game's own melee)
            const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(oh_w), 0ull};
            queue_put_in_hand(args);  // (instant: no holstering clip of the gun)
            controls::set_draw_hand(h, oh_w, false);
            hold_take(gun_h, 2, gw, gz);
            hold_take(h, 1, oh_w, in_z[h]);
            g_torch_swaps.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(h, 0.5f, 30);
            log::info("[holster] %s hand at the %s: the %s in hand (the game's weapon), the %s the second gun in the %s hand (%s)", h ? "right" : "left",
                      zones[in_z[h]].key, oh_w == 22 ? "knife" : "torch", gw >= 0 && gw < kWeapons ? kWeaponLabel[gw] : "?", gun_h ? "right" : "left",
                      oh_w == 22 ? "KnifeSwap" : "answer 9");
        } else if (oh_w >= 0) {  // [Hands] OffHandItems: the item drawn into the free hand (our copy; the gun stays in hand)
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            if (!offhand::take(h, jh, oh_w, in_z[h], zones[in_z[h]].key, st)) controllers::pulse(h, 0.15f, 10);
        } else if (pressed && in_z[h] >= 0 && ds.on && h == ds.ctrl) {  // [Hands] DualWield: the second gun's hand at a holster
            const Zone& z = zones[in_z[h]];
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            bool its = false;
            for (int s : z.slots) its = its || (s >= 0 && (ds.copy ? s == 1 : s == ds.slot));  // the copy goes back at a hip
            if (!ds.copy && ds.weapon >= 0 && ds.weapon < kWeapons && weapon_fits(z, ds.weapon, ds.slot)) its = true;  // the Sawed-off at a hip
            // (P2, the review) the Sawed-off's copy back at the zone it was taken at, or one that draws the Sawed-off
            if (ds.copy && ds.weapon == 15 && (in_z[h] == sec_from[h] || z.weapon == 15 || draw_weapon(in_z[h]) == 15)) its = true;
            sec_pending[h] = its && sec_armed[h];
            if (its && !sec_armed[h])
                log::info("[wield] %s hand at the %s with the second gun (slot %d): kept (it was just taken here; move away first)",
                          h ? "right" : "left", z.key, ds.slot);
            else if (its)
                log::info("[wield] %s hand at the %s with the second gun (slot %d): let go to put it back", h ? "right" : "left", z.key, ds.slot);
            else
                log::info("[wield] %s hand at the %s: nothing (it holds the second gun, of slot %d)", h ? "right" : "left", z.key, ds.slot);
        } else if (pressed && in_z[h] >= 0 && dual::copy_enabled() && !ds.on && gun && h == off_h &&
                   ((cur == 1 && st.weapon >= 0 && st.weapon <= 7 && draw_weapon(in_z[h]) < 8 && [&] {
                        for (int s : zones[in_z[h]].slots)  // [Hands] DualWieldCopy: a hip (slot 1) with a sidearm in hand
                            if (s == 1) return true;
                        return false;
                    }()) ||
                    // (2026-11-01, step 4, P2) the Sawed-off in hand, the hand at a zone that draws the Sawed-off: a second as the copy
                    (st.weapon == 15 && (draw_weapon(in_z[h]) == 15 || zones[in_z[h]].weapon == 15)))) {
            const Zone& z = zones[in_z[h]];
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            // round 13 item 10 ([Hands] DualWieldSameAtItsHolster): the gun in hand's own holster gives its own model
            // (2026-10-26, "two of the same gun in each holster": a hip that chose the gun in hand gives a second of it too)
            const bool its_own = dual::same_at_its_holster() && ((drawn_from == in_z[h] && zone_last[in_z[h]] == st.weapon) ||
                                                                 (owns(zones[in_z[h]].weapon) && zones[in_z[h]].weapon == st.weapon));
            if (its_own) {
                dual::set_copy_model(dual::copy_as_prop() ? st.weapon : -1);  // run 7 item 1: its own model as a prop
                log::info("[wield] %s hand at the %s, the %s's own holster: a second %s", h ? "right" : "left", z.key, kWeaponLabel[st.weapon],
                          kWeaponLabel[st.weapon]);
            } else {  // [Hands] DualWieldOwnModel: the copy's model, the hip's gun if another owned sidearm, else the first owned revolver
                // (2026-10-10: the hip's gun as it draws and shows it, automatic included; the hip's weapon alone gave an
                // automatic hip's copy the first owned revolver while the hip showed another, which stayed in the holster)
                auto own = [&](int w) { return w >= 0 && w < 8 && w != st.weapon && st.owned_tick != 0 && (st.owned >> w & 1); };
                const int hip_gun = draw_weapon(in_z[h]);
                int other = own(hip_gun) ? hip_gun : own(z.weapon) ? z.weapon : -1;
                static const int kPref[8] = {4, 5, 6, 7, 0, 1, 2, 3};
                for (int w : kPref)
                    if (other < 0 && dual::own_model() && own(w)) other = w;
                if (!dual::own_model() || st.weapon > 7) other = -1;  // (P2, the review: never a revolver's model on the Sawed-off's copy)
                if (other < 0 && dual::copy_as_prop()) other = st.weapon;  // run 7 item 1: the same model, as a prop
                dual::set_copy_model(other);
            }
            dual::begin(h, jh, dual::kCopySlot, z.key);
            sec_from[h] = in_z[h];
            hold_take(h, 2, st.weapon, in_z[h]);  // ([Holsters] HoldToKeep: the copy held by this grip)
            controllers::pulse(h, 0.5f, 30);
        } else if (pressed && in_z[h] >= 0 && dual::enabled() && !ds.on && sec_beside && h == off_h && !holds_slot(in_z[h]) && [&] {
                       for (int s : zones[in_z[h]].slots)  // a gun slot (1 the sidearms, 4-6 the long guns) other than the gun's
                           if ((s == 1 || (s >= 4 && s <= 6)) && s != cur && slot_filled(s)) return true;
                       // (2026-10-26) a hip that chose the Sawed-off: its slot (6), if not the gun in hand's
                       const int hw = owns(zones[in_z[h]].weapon) ? zones[in_z[h]].weapon : -1;
                       return hw == kSawedOff && st.equip_slot[hw] != cur && slot_filled(st.equip_slot[hw]);
                   }()) {
            const Zone& z = zones[in_z[h]];
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            // the gun this holster draws (its choice, its own last, its twin's other), in its slot; else the first filled slot
            const int dw = draw_weapon(in_z[h]);
            const int ws = dw >= 0 && dw < kWeapons ? st.equip_slot[dw] : -1;
            bool listed = dw >= 0 && dw < kWeapons && weapon_fits(z, dw, ws);  // (the Sawed-off at a hip: listed by weapon_fits)
            if (dw >= 0 && listed && in_hand && pair_long(dw) && pair_long(st.weapon)) {  // (P1) two long guns
                g_two_long_refused.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.15f, 10);
                log::info("[wield] %s hand at the %s: the %s beside the %s in hand: two long guns, refused", h ? "right" : "left", z.key, kWeaponLabel[dw],
                          kWeaponLabel[st.weapon]);
            } else if (dw >= 0 && listed && ws == cur) {  // the game keeps one gun a slot: the gun in hand's
                g_sec_refused.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.15f, 10);
                log::info("[wield] %s hand at the %s: the %s shares the slot of the %s in hand (%d): no second gun", h ? "right" : "left", z.key,
                          kWeaponLabel[dw], st.weapon >= 0 && st.weapon < kWeapons ? kWeaponLabel[st.weapon] : "?", cur);
            } else if (dw >= 0 && listed && slot_filled(ws) && !(slot_gun(ws) == dw && slot_loaded(ws))) {
                if (slot_gun(ws) != dw) {  // the game's last gun of that slot is another: switched to this holster's
                    const uint64_t args[2] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(dw)};
                    api::queue_native(kSetNextWeapon, args, 2, 0, nullptr);
                    g_sec_switches.fetch_add(1, std::memory_order_relaxed);
                    log::info("[wield] %s hand at the %s: slot %d switched to the %s (it held the %s)", h ? "right" : "left", z.key, ws,
                              kWeaponLabel[dw], slot_gun(ws) >= 0 && slot_gun(ws) < kWeapons ? kWeaponLabel[slot_gun(ws)] : "?");
                }
                sec_wait[h] = SecWait{ws, dw, in_z[h], log::now_ms()};
                zone_last[in_z[h]] = dw;
                hold_take(h, 2, dw, in_z[h]);  // ([Holsters] HoldToKeep: the second gun, once ready, held by this grip; let go before, none)
            } else if (dw >= 0 && listed && slot_filled(ws)) {  // the slot already holds it, loaded: at once
                zone_last[in_z[h]] = dw;
                dual::begin(h, jh, ws, z.key);
                sec_from[h] = in_z[h];
                hold_take(h, 2, dw, in_z[h]);
                controllers::pulse(h, 0.5f, 30);
            } else if (int sslot = [&] {
                           int f = -1;
                           for (int s : z.slots)
                               if (f < 0 && (s == 1 || (s >= 4 && s <= 6)) && s != cur && slot_filled(s)) f = s;
                           return f;
                       }();
                       in_hand && pair_long(st.weapon) && pair_long(slot_gun(sslot))) {  // (P1, the review: the slot's own gun a long one)
                g_two_long_refused.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.15f, 10);
                log::info("[wield] %s hand at the %s: slot %d's %s beside the %s in hand: two long guns, refused", h ? "right" : "left", z.key, sslot,
                          kWeaponLabel[slot_gun(sslot)], kWeaponLabel[st.weapon]);
            } else {
                dual::begin(h, jh, sslot, z.key);
                sec_from[h] = in_z[h];
                hold_take(h, 2, -1, in_z[h]);
                controllers::pulse(h, 0.5f, 30);
            }
        } else if (pressed && in_z[h] >= 0) {
            const Zone& z = zones[in_z[h]];
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            const int dw = draw_weapon(in_z[h]);
            const bool mine = h == gun_h || !controls::draw_any_active();  // the hand holding the gun
            // [Hands] CarryByForegrip: no draw of the carried gun's slot (the game keeps one gun a slot: it is the carried one)
            int first_slot = -1;
            for (int s : z.slots)
                if (first_slot < 0 && slot_filled(s)) first_slot = s;
            const bool carried = ds.carry && (dw >= 0 && dw < kWeapons ? st.equip_slot[dw] == ds.slot : first_slot == ds.slot);
            if (carried && !(holds(in_z[h]) && mine)) {
                controllers::pulse(h, 0.15f, 10);
                log::info("[holster] %s hand at the %s: its slot %d holds the carried gun: no draw (take it back at its grip)", h ? "right" : "left", z.key,
                          ds.slot);
            } else if (holds(in_z[h]) && mine) {
                pending[h] = true;  // let go here: holstered
                pending_w[h] = st.weapon;
                log::info("[holster] %s hand at the %s with the %s in hand (slot %d): let go to put it away", h ? "right" : "left", z.key,
                          st.weapon >= 0 && st.weapon < kWeapons ? kWeaponLabel[st.weapon] : "?", cur);
            } else if (holds(in_z[h]) && dw < 0) {  // the other hand at the gun's own holster: the gun into that hand
                controls::set_draw_hand(h, -1, true);
                // ([Holsters] HoldToKeep: held by this grip now, from the holster it came from; without DrawToGrabbingHand the
                // gun stays in the gun hand, and its hold with it)
                if (keepable(st.weapon) && controls::draw_any_active())
                    hold_take(h, 1, st.weapon, keep[1 - h] == 1 && keep_from[1 - h] >= 0 ? keep_from[1 - h] : drawn_from);
                else hold_drew[h] = true;
                log::info("[holster] %s hand at the %s: the gun into this hand", h ? "right" : "left", z.key);
            } else if (dw >= 0 && pair_long(dw) && ((ds.carry && pair_long(ds.carry_weapon)) || (ds.on && !ds.carry && !ds.copy && pair_long(ds.weapon)) ||
                                                     (sec_wait[1 - h].slot >= 0 && pair_long(sec_wait[1 - h].weapon)))) {  // (the review: one pending)
                // (P1) a long gun drawn beside a long second gun or a carried one: two long guns
                g_two_long_refused.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.15f, 10);
                log::info("[holster] %s hand at the %s: the %s beside the %s %s: two long guns, refused", h ? "right" : "left", z.key, kWeaponLabel[dw],
                          ds.carry ? kWeaponLabel[ds.carry_weapon] : ds.on ? kWeaponLabel[ds.weapon] : "long gun", ds.carry ? "carried" : "as the second gun");
            } else if (dw >= 0 && throwable_index(dw) >= 0 && offhand::thrown_left(dw, static_cast<uint32_t>(st.actor)) == 0) {
                // (round 123, the user: "when you run out of ammo with a throwing weapon ... you can continue to grab the throwable
                // from there endlessly") none of it left: no draw
                g_none_left_refused.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.15f, 10);
                log::info("[holster] %s hand at the %s: no %s left, no draw", h ? "right" : "left", z.key, kWeaponLabel[dw]);
            } else if (dw >= 0) {  // the holster's chosen weapon, or a twin's other gun (any owned weapon draws)
                const bool instant = g_instant.load(std::memory_order_relaxed);
                const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(dw), instant ? 0ull : 1ull};
                queue_put_in_hand(args);
                zone_last[in_z[h]] = dw;
                drawn_from = in_z[h];
                // ([Holsters] HoldToKeep: a gun held by the grip of the hand it lands in; the knife, the lasso, a throwable keep
                // their own grips)
                if (keepable(dw)) hold_draw(h, dw, in_z[h], !instant);
                else hold_drew[h] = true;
                controls::set_draw_hand(h, dw, !in_hand);  // the gun to the hand that grabbed it, once it is in hand
                g_draws.fetch_add(1, std::memory_order_relaxed);
                g_choice_draws.fetch_add(1, std::memory_order_relaxed);
                log::info("[holster] %s hand at the %s: draw the %s (weapon %d, %s%s)", h ? "right" : "left", z.key, kWeaponLabel[dw], dw,
                          owns(z.weapon) ? "the holster's choice" : z.partner < 0 ? "its own last" : "another than its twin's", instant ? "" : ", animated");
            } else {
                for (int s : z.slots) {
                    if (!slot_filled(s)) continue;
                    const uint64_t args[2] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(s)};
                    draw_req[h] = api::queue_native(kWeaponEquipped, args, 2, 0, nullptr);
                    draw_ms[h] = log::now_ms();
                    draw_slot[h] = s;
                    draw_keep[h] = hold_draw(h, -1, in_z[h], !g_instant.load(std::memory_order_relaxed));  // ([Holsters] HoldToKeep: its weapon at the answer)
                    break;
                }
            }
        }
        if (draw_req[h]) {  // the slot's weapon: put it in the hand
            RdrvrNativeResult r{};
            if (api::wait_native(draw_req[h], &r, 0)) {
                draw_req[h] = 0;
                const int32_t w = static_cast<int32_t>(static_cast<uint32_t>(r.value));
                {  // [Holsters] HoldToKeep: the slot's weapon, if a gun, held by the hand this draw's hold is on (that one only)
                    const int c = draw_keep[h];
                    draw_keep[h] = -1;
                    if (c >= 0 && c <= 1 && keep[c] == 1 && keep_w[c] < 0) {
                        if (w >= 0 && keepable(w)) keep_w[c] = w;
                        else keep[c] = 0;
                    }
                }
                const bool two_long = w >= 0 && w < kWeapons && pair_long(w) &&
                                      ((ds.carry && pair_long(ds.carry_weapon)) || (ds.on && !ds.carry && !ds.copy && pair_long(ds.weapon)));
                if (two_long) {  // (P1) the slot's long gun beside a long second gun or a carried one
                    g_two_long_refused.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(h, 0.15f, 10);
                    log::info("[holster] slot %d's %s beside the %s %s: two long guns, refused", draw_slot[h], kWeaponLabel[w],
                              kWeaponLabel[ds.carry ? ds.carry_weapon : ds.weapon], ds.carry ? "carried" : "as the second gun");
                } else if (w >= 0) {
                    const bool instant = g_instant.load(std::memory_order_relaxed);
                    const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(w),
                                              instant ? 0ull : 1ull};
                    queue_put_in_hand(args);
                    if (press_zone[h] >= 0) zone_last[press_zone[h]] = w;
                    drawn_from = press_zone[h];
                    controls::set_draw_hand(h, w, !in_hand);
                    g_draws.fetch_add(1, std::memory_order_relaxed);
                    log::info("[holster] %s hand at the %s: draw slot %d (weapon %d%s)", h ? "right" : "left",
                              press_zone[h] >= 0 ? zones[press_zone[h]].key : "?", draw_slot[h], w, instant ? "" : ", animated");
                } else {
                    log::warn("[holster] slot %d holds no weapon (%d)", draw_slot[h], w);
                }
            } else if (log::now_ms() - draw_ms[h] > 1000.0) {
                draw_req[h] = 0;
                {  // ([Holsters] HoldToKeep: no draw, nothing held)
                    const int c = draw_keep[h];
                    draw_keep[h] = -1;
                    if (c >= 0 && c <= 1 && keep[c] == 1 && keep_w[c] < 0) keep[c] = 0;
                }
                log::warn("[holster] no answer for the weapon in slot %d", draw_slot[h]);
            }
        }
        if (defer_n[h] > 0) {  // (the review) a let-go's rounds for the copy, waiting for its spare to settle
            const int in = !ds.copy || !sec_pt ? 0 : dual::load_copy_rounds(defer_n[h], st.actor);
            if (in > 0) {
                actions::round_in(sec_shown, load_sec, in, true);
                if (ds.carry) actions::carried_round_in(ds.weapon);  // (R17, the review)
                controllers::pulse(ds.ctrl, 0.5f, 20);
                controllers::pulse(h, 0.5f, 20);
                g_cross_in.fetch_add(static_cast<uint64_t>(in), std::memory_order_relaxed);
                g_rounds_in.fetch_add(static_cast<uint64_t>(in), std::memory_order_relaxed);
                log::info("[holster] %d round%s into the copy, let go at it (its spare settled after %.0f ms)", in, in == 1 ? "" : "s", now_ms - defer_ms[h]);
                defer_n[h] = 0;
            } else if (in == 0 || now_ms - defer_ms[h] > 1000.0) {
                log::info("[holster] %d round%s let go at the copy dropped (%s)", defer_n[h], defer_n[h] == 1 ? "" : "s",
                          in == 0 ? "the copy full, gone or no spare" : "its spare not settled in a second");
                defer_n[h] = 0;
            }
        }
        // (round 55) dual wielding (the second gun or the copy out, not a carry), a hand holding a gun that enters the chest's
        // zone takes there at once what its press would (DualWieldCrossReload: rounds for the other gun; ChestSqueeze: into
        // its own), no press (its grip holds the gun, HoldToKeep or not); only when they would go in. Rounds so taken are
        // held without the grip until they go in (a grip pressed and let go drops them, as any)
        if (chest_armed[h] && in_z[h] >= 0 && zone_rounds(in_z[h]) && (gun || (item_x && h == gun_h && offhand::kind_of(st.weapon) == offhand::kKnife)) && ds.on && !ds.carry && (h == gun_h || h == ds.ctrl) && !pressed &&
            !round_held[h] && hands::get(h).trigger < 0.4f && wheel::holder() != h && !menu::visible() && !g_place.load(std::memory_order_relaxed) &&
            !g_piece_place.load(std::memory_order_relaxed) && chest_would_take(h)) {
            chest_armed[h] = false;
            g_auto_chest.fetch_add(1, std::memory_order_relaxed);
            log::info("[holster] %s hand holding a gun entered the chest (dual wielding): its take there without a press", h ? "right" : "left");
            chest_take(h);
            auto_rounds[h] = round_held[h];
            auto_ms[h] = now_ms;
        }
        // (the review) such rounds never hold the gun's fire for long: its trigger pulled drops them (that gun's fire then held
        // until the trigger is let go, as DualWieldCrossReload's hold), and so do kAutoRoundsMs without reaching the ring
        if (auto_rounds[h] && round_held[h] && (hands::get(h).trigger > 0.6f || now_ms - auto_ms[h] > kAutoRoundsMs)) {
            round_held[h] = false;
            log::info("[holster] %s hand's rounds from the chest (taken without a press) dropped: %s", h ? "right" : "left",
                      hands::get(h).trigger > 0.6f ? "its trigger pulled" : "not loaded within 10 s");
        }
        if (!round_held[h] || pressed) auto_rounds[h] = false;
        if (round_held[h] && round_tgt[h] == 1 && !sec_pt) {
            // [Hands] DualWieldCrossReload: the second gun the rounds were for is gone (put back, ended, the option off) or
            // its hand's points are not known: the rounds are dropped, none taken from the spare
            round_held[h] = false;
            log::info("[holster] %s hand's rounds for the second gun dropped: %s", h ? "right" : "left", !ds.on ? "no second gun" : "the second gun not ready");
        }
        if (!round_held[h]) round_mag[h] = false;
        if (round_held[h] && round_mag[h] && round_tgt[h] == 1 && sec_pt && ds.john >= 0 && ds.john <= 1 && bp.hand_ok[ds.john]) {
            // (C7) the second pistol's magazine: in at its grip (the well), held within 10 cm, or let go within 14 cm (below)
            const float* mg = gb[bp.ctrl[0] == h ? 0 : 1];
            const float* sg = bp.hand[ds.john];
            const float dg = std::sqrt((mg[0] - sg[0]) * (mg[0] - sg[0]) + (mg[1] - sg[1]) * (mg[1] - sg[1]) + (mg[2] - sg[2]) * (mg[2] - sg[2]));
            const bool rel = !grip_down[h];
            if (dg < (rel ? 0.14f : 0.10f)) {
                const int nin = actions::second_mag_insert(ds, st.actor);
                if (nin > 0) {
                    g_mag_ins.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(h, 0.6f, 25);
                    controllers::pulse(ds.ctrl, 0.5f, 20);
                    log::info("[holster] the magazine pushed into the second gun (%.3f m from its grip): %d rounds", dg, nin);
                }
                if (nin >= 0) round_held[h] = round_mag[h] = false;  // (-1: the copy's spare settling: still held, tried next frame)
            } else if (rel) {
                log::info("[holster] the second gun's magazine dropped: let go %.3f m from its grip", dg);
                round_held[h] = round_mag[h] = false;
            }
        }
        if (round_held[h] && round_mag[h] && round_tgt[h] != 1 && !mag_pt_ok) {  // (the pistol put away, the option off, its points not known)
            round_held[h] = round_mag[h] = false;
            log::info("[holster] %s hand's magazine dropped: no magazine gun in hand (or its points not known)", h ? "right" : "left");
        }
        if (round_held[h] && round_mag[h] && round_tgt[h] != 1 && down) {  // [Reload] PistolMagazines: its top reaches the well: in it goes
            const int rj = bp.ctrl[0] == h ? 0 : 1;
            const float md = mag_top_d(rj);
            if (md >= 0.0f && md < kMagRing) {
                int nin = 0;
                if (actions::insert_mag(st, mag_pt, &nin)) {
                    round_held[h] = round_mag[h] = false;
                    g_mag_ins.fetch_add(1, std::memory_order_relaxed);
                    g_mag_rounds_in.store(nin, std::memory_order_relaxed);
                    g_rounds_in.fetch_add(static_cast<uint64_t>(nin), std::memory_order_relaxed);
                    controllers::pulse(h, 0.6f, 25);
                    controllers::pulse(gun_h, 0.5f, 20);
                    log::info("[holster] the %s pushed in (its top %.3f m from the well): %d rounds (clip %.0f -> %.0f of %.0f)",
                              st.weapon == 3 ? "stripper clip" : "magazine", md, nin, st.clip, st.clip + static_cast<float>(nin), st.clip_max);
                }
            }
        }
        if (round_held[h] && !round_mag[h] && (down || auto_rounds[h])) {  // the round reaches the gun: in it goes
            const int rj = bp.ctrl[0] == h ? 0 : 1;  // John's hand holding the round
            const bool to_sec = round_tgt[h] == 1;  // (DualWieldCrossReload: the second gun's loading point and ring)
            const float* lp = to_sec ? load_sec : load_pt;
            const float lrr = to_sec ? lr_sec : lr;
            float d_round = -1.0f, d_wrist = 1e9f;
            round_at(rj, lp, &d_round, &d_wrist);
            {
                std::lock_guard lock(g_zone_mutex);
                g_load_diag.wrist_d = d_wrist;
                g_load_diag.round_d = d_round;
                if (d_round >= 0.0f && (g_load_diag.min_round_d < 0.0f || d_round < g_load_diag.min_round_d)) g_load_diag.min_round_d = d_round;
                if (g_load_diag.min_wrist_d < 0.0f || d_wrist < g_load_diag.min_wrist_d) g_load_diag.min_wrist_d = d_wrist;
                g_load_diag.ring_r = lrr;
                g_load_diag.can_load = (to_sec ? sec_gate() : actions::can_load(st.weapon)) ? 1 : 0;
            }
            const bool by_touch = touch && d_round >= 0.0f;
            // [Reload] RoundsGoIn=one: a round each RoundInsertMs while the hand stays at the ring (the first at once)
            const bool all_in = g_all_in.load(std::memory_order_relaxed);
            if ((by_touch ? d_round < lrr : d_wrist < lrr) && (all_in || now_ms >= round_next_ms[h])) {
                const int want = all_in ? round_n[h] : 1;
                {
                    std::lock_guard lock(g_zone_mutex);
                    ++(by_touch ? g_load_diag.inserts_touch : g_load_diag.inserts_wrist);
                }
                if (by_touch) log::info("[holster] the round touched the loading point (the round %.3f m, the wrist %.3f m from it)", d_round, d_wrist);
                const int in = put_in(h, want, st);
                if (in < 0) {
                    // (the copy's spare not settled this update: the rounds kept, tried again next update)
                } else if (in > 0) {
                    round_n[h] -= in;
                    round_next_ms[h] = now_ms + g_insert_gap_ms.load(std::memory_order_relaxed);
                    recent_in[h] = in;
                    recent_ms[h] = now_ms;
                    if (to_sec)
                        log::info("[holster] %d round%s into the %s (clip %.0f -> %.0f of %.0f; %d left in the hand)", in, in == 1 ? "" : "s",
                                  ds.copy ? "copy" : "second gun", ds.clip, ds.clip + static_cast<float>(in), ds.clip_max, round_n[h] > 0 ? round_n[h] : 0);
                    else
                        log::info("[holster] %d round%s into the gun (clip %.0f -> %.0f of %.0f; %d left in the hand)", in, in == 1 ? "" : "s", st.clip,
                                  st.clip + static_cast<float>(in), st.clip_max, round_n[h] > 0 ? round_n[h] : 0);
                    if (round_n[h] <= 0 || all_in) round_held[h] = false;  // all in, or as many as the gun took
                } else {
                    round_held[h] = false;
                    if (to_sec)
                        log::info("[holster] the round did not go into the %s (clip %.0f/%.0f, spare %.0f, flags %#x)", ds.copy ? "copy" : "second gun", ds.clip,
                                  ds.clip_max, sec_spare, st.weapon_flags);
                    else
                        log::info("[holster] the round did not go in (clip %.0f/%.0f, spare %.0f, flags %#x)", st.clip, st.clip_max, st.spare, st.weapon_flags);
                }
            }
        }
        if (retwo[h]) {  // [Hands] CarryByForegrip: taken back with this hand still on the foregrip: two-handed again (as two_paused)
            if (!grip_down[h] || now_ms - retwo_ms > 1000.0) {
                retwo[h] = false;
            } else if (h == off_h && long_gun && fore_near) {
                retwo[h] = false;
                two_hold[h] = true;
                g_two_grips.fetch_add(1, std::memory_order_relaxed);
                log::info("[holster] %s hand still on the foregrip as the carried gun was taken back: two-handed again", h ? "right" : "left");
            }
        }
        if (two_hold[h] && !(down && long_gun)) two_hold[h] = false;
        if (broken && two_hold[h]) {
            two_hold[h] = false;
            two_paused[h] = true;
        }
        if (two_paused[h] && !(down && long_gun)) two_paused[h] = false;
        if (two_paused[h] && !broken) {  // shut with the hand still on the foregrip: two-handed again
            two_paused[h] = false;
            if (h == off_h && fore_near) {
                two_hold[h] = true;
                log::info("[holster] %s hand still on the foregrip as the gun shut: two-handed again", h ? "right" : "left");
            }
        }
        if (trig_take[h] && !round_held[h]) trig_grab[h] = trig_take[h] = false;  // (the review: the hold as long as its round)
        // ([Hands] CarryByForegrip: an empty hand at the carried gun's grip is the holster's from its press. The gun hand's
        // grip while two-handed only with Mode=wheel (its grip is the wheel, never the hand-over's): otherwise it stays the
        // game's RB at once, its cover (the review: held back, a cover tap was a hand-over), and a hand-over is a hold)
        if (carry_press[h] && (st.flags & RDRVR_ACTOR_IN_COVER)) carry_cover[h] = true;
        g_wanted[h] = in_z[h] >= 0 || (h == off_h && fore_ok) ||
                      (h == gun_h && dual::carry_enabled() && long_gun && two_hold[off_h] && in_z[h] < 0 && wheel::wheel_mode()) ||
                      carry_near(h, jh) || hand_take_at(h, jh, in_z[h]) ||  // ([Reload] RoundSource gun / palm: its press)
                      hold_wants(h);  // ([Holsters] HoldToKeep: the grip of a hand holding a gun is its hold)
        if (released && round_held[h] && round_mag[h] && round_tgt[h] != 1) {  // [Reload] PistolMagazines: let go at the well it goes in, else it falls
            const int rj = bp.ctrl[0] == h ? 0 : 1;
            const float md = mag_top_d(rj);
            int nin = 0;
            if (md >= 0.0f && md < kMagRing + 0.04f && actions::insert_mag(st, mag_pt, &nin)) {
                g_mag_ins.fetch_add(1, std::memory_order_relaxed);
                g_mag_rounds_in.store(nin, std::memory_order_relaxed);
                g_rounds_in.fetch_add(static_cast<uint64_t>(nin), std::memory_order_relaxed);
                log::info("[holster] the %s let go at the well (%.3f m): in, %d rounds", st.weapon == 3 ? "stripper clip" : "magazine", md, nin);
            } else {
                g_mag_away.fetch_add(1, std::memory_order_relaxed);
                float mp[12];
                if (round_draw::mag_pose(bp, rj, st.weapon, mp)) round_draw::drop_mag(mp, st.weapon, round_n[h]);
                log::info("[holster] the %s dropped: let go %.3f m from the well (it goes in within %.2f m holding, %.2f letting go): none taken "
                          "from the spare", st.weapon == 3 ? "stripper clip" : "magazine", md, kMagRing, kMagRing + 0.04f);
            }
            round_held[h] = round_mag[h] = false;
        }
        if (released && round_held[h]) {  // round 10: two rounds taken and let go before the gun, silently lost
            const int rj = bp.ctrl[0] == h ? 0 : 1;
            const bool to_sec = round_tgt[h] == 1;  // (DualWieldCrossReload: the second gun's loading point and ring)
            const float lrr = to_sec ? lr_sec : lr;
            float d_round = -1.0f, d = 1e9f;
            round_at(rj, to_sec ? load_sec : load_pt, &d_round, &d);
            if (touch && d_round >= 0.0f) d = d_round;  // InsertOnTouch: the round's own distance
            // let go at the gun: the rest of the handful in at once
            const int left = round_n[h] > 1 ? round_n[h] : 1;
            RdrvrActorState sx = st;  // (the review: a round queued at the ring a frame ago may not be in the count yet)
            // (2026-10-27, the cross reload's review: counted only until the actor state was read after the tick it ran on,
            // not for 250 ms, which refused rounds already shown)
            if (now_ms - recent_ms[h] < 1000.0 && !reload::spare_seen(st)) {
                sx.clip += static_cast<float>(recent_in[h]);
                sx.spare -= static_cast<float>(recent_in[h]);
            }
            const int in = d < lrr + 0.04f ? put_in(h, left, sx) : 0;
            if (in < 0) {  // the copy's spare not settled: its rounds go in when it is (within a second)
                defer_n[h] = left;
                defer_ms[h] = now_ms;
                log::info("[holster] %d round%s let go at the copy: in as its spare settles", left, left == 1 ? "" : "s");
            } else if (in > 0) {
                {
                    std::lock_guard lock(g_zone_mutex);
                    ++g_load_diag.inserts_let_go;
                }
                // (the clip as this update read it: rounds put in at the ring a moment ago may not be in it yet)
                log::info("[holster] %d round%s into the %s, let go at it (%.2f m; clip %.0f + %d of %.0f)", in, in == 1 ? "" : "s",
                          to_sec ? (ds.copy ? "copy" : "second gun") : "gun", d, to_sec ? ds.clip : st.clip, in, to_sec ? ds.clip_max : st.clip_max);
            } else {
                log::info("[holster] the round dropped: let go %.2f m from the %s's loading point (it goes in within %.2f m holding, %.2f m letting go)",
                          d, to_sec ? (ds.copy ? "copy" : "second gun") : "gun", lrr, lrr + 0.04f);
            }
        }
        if (released && press_zone[h] >= 0 && press_zone[h] < kZones && in_z[h] != press_zone[h] && zok[press_zone[h]]) {
            const int pz = press_zone[h], jr = bp.ctrl[0] == h ? 0 : 1;
            const float dx = gb[jr][0] - zp[pz][0], dy = gb[jr][1] - zp[pz][1], dz = gb[jr][2] - zp[pz][2];
            const float rr = zones[pz].radius * g_release_margin.load(std::memory_order_relaxed);
            if (bp.hand_ok[jr] && dx * dx + dy * dy + dz * dz < rr * rr) in_z[h] = pz;  // let go at the edge: still there
        }
        {  // [Hands] DualWieldCrossReload: a gun hand's rounds hold its own gun's fire, and after them until its trigger is let
           // go (a trigger held through the reload fires nothing when the rounds are in: a fresh pull does)
            const bool held_x = round_held[h] && round_x[h];
            if (!round_held[h]) round_x[h] = false;
            const bool was_hold = g_cross_fire_hold[h].load(std::memory_order_relaxed);
            const bool hold = held_x || (was_hold && hands::get(h).trigger > 0.4f);  // (let go below 0.4, as the triggers elsewhere)
            if (hold != was_hold) {
                g_cross_fire_hold[h].store(hold, std::memory_order_relaxed);
                log::info("[holster] %s hand's gun: fire %s (DualWieldCrossReload: %s)", h ? "right" : "left", hold ? "held back" : "free again",
                          hold ? "it holds the other gun's rounds" : "the rounds gone, its trigger let go");
            }
        }
        if (released) {
            // [Hands] OffHandItems: the item let go: at the zone it came from, put back; elsewhere as its kind says
            // (the review: the let-go debounced, the zone the hand was at when the grip opened, not 100 ms later)
            const int oz = oh_up_zone[h] >= 0 ? oh_up_zone[h] : in_z[h];
            if (offhand::held(h)) offhand::release(h, oz >= 0 && oz == offhand::held_zone(h));
            round_held[h] = false;
            round_x[h] = false;
            two_hold[h] = false;
            if (sec_pending[h] && in_z[h] >= 0 && in_z[h] == press_zone[h]) {
                dual::end((std::string("put back at the ") + zones[in_z[h]].key).c_str());
                if (keep[h] == 2) keep[h] = 0;  // ([Holsters] HoldToKeep: put back here already)
                controllers::pulse(h, 0.4f, 25);
            }
            sec_pending[h] = false;
            // ([Holsters] HoldToKeep: a held gun let go away from its holster is put away after the loop, not here)
            if (pending[h] && keep[h] == 0 && !(g_consumed[h] && in_z[h] >= 0 && in_z[h] == press_zone[h] && in_hand))
                log::info("[holster] %s hand let go away from the %s (in zone %d, item %d): not put away", h ? "right" : "left",
                          press_zone[h] >= 0 ? zones[press_zone[h]].key : "?", in_z[h], in_hand ? 1 : 0);
            if (carry_press[h]) {  // [Hands] CarryByForegrip: let go away from the holsters, still two-handed: the hand-over
                carry_press[h] = false;
                // (the grip was the game's RB meanwhile, except with Mode=wheel: held 0.4 s, never in cover, so a cover tap
                // is not a hand-over)
                const double held_ms = now_ms - carry_press_ms[h];
                const bool hold_ok = wheel::wheel_mode() || (held_ms >= kCarryHoldMs && !carry_cover[h]);
                if (!hold_ok)
                    log::info("[holster] %s hand let go of the two-handed gun's grip after %.0f ms%s: not carried (a tap, the game's RB)",
                              h ? "right" : "left", held_ms, carry_cover[h] ? " (in cover)" : "");
                else
                    hand_over(h, in_z[h] < 0, drawn_from);
            }
            if (g_consumed[h] && pending[h] && in_z[h] >= 0 && in_z[h] == press_zone[h] && in_hand && ds.on && !ds.copy && !ds.carry && ds.ready &&
                ds.weapon >= 0 && ds.weapon < kWeapons && ds.ctrl >= 0 && ds.ctrl != h) {
                // 2026-10-10 (the user's other PC: the gun in hand put away, the second gun stayed in the other hand with no
                // gun "in hand": every holster refused that hand): the second gun becomes the gun in hand, in its own hand
                if (st.weapon >= 0 && st.weapon < kWeapons) put_zone_of[st.weapon] = in_z[h];
                const int sec_ctrl = ds.ctrl, sec_w = ds.weapon;
                dual::end("the gun in hand put away: it becomes the gun in hand");
                const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(sec_w), 0ull};
                queue_put_in_hand(args);
                controls::set_draw_hand(sec_ctrl, sec_w, false);  // that hand, once the gun is in it
                g_puts.fetch_add(1, std::memory_order_relaxed);
                keep[h] = 0;  // ([Holsters] HoldToKeep: put away here; the second gun's grip keeps it as the gun in hand)
                hold_put_ms = now_ms;
                if (keep[sec_ctrl] == 2) hold_take(sec_ctrl, 1, sec_w, sec_from[sec_ctrl]);
                log::info("[holster] %s hand let go at the %s: put away; the second gun (the %s) is the gun in hand now, in the %s hand",
                          h ? "right" : "left", zones[in_z[h]].key, kWeaponLabel[sec_w], sec_ctrl ? "right" : "left");
            } else if (g_consumed[h] && pending[h] && in_z[h] >= 0 && in_z[h] == press_zone[h] && in_hand &&
                       (st.weapon != pending_w[h] || put_in_hand_other(pending_w[h]))) {  // (item 4.6) another weapon in hand since the press
                g_putaway_skipped.fetch_add(1, std::memory_order_relaxed);
                keep[h] = 0;  // (the fix's review: no HoldToKeep put-away of the draw later either)
                hold_put_ms = now_ms;
                log::info("[holster] %s hand let go at the %s: not put away (the %s pressed for; %s): the other hand's draw kept",
                          h ? "right" : "left", zones[in_z[h]].key, pending_w[h] >= 0 && pending_w[h] < kWeapons ? kWeaponLabel[pending_w[h]] : "?",
                          st.weapon != pending_w[h] ? "another weapon in hand now" : "another weapon put in hand within the second");
            } else if (g_consumed[h] && pending[h] && in_z[h] >= 0 && in_z[h] == press_zone[h] && in_hand) {
                put_item_away(st.actor, st.weapon);  // ([Holsters] InstantPutAway)
                g_puts.fetch_add(1, std::memory_order_relaxed);
                keep[h] = 0;  // ([Holsters] HoldToKeep: put away here)
                hold_put_ms = now_ms;
                if (st.weapon >= 0 && st.weapon < kWeapons) {
                    put_zone_of[st.weapon] = in_z[h];  // item 1b: where it was put away
                    if (any_here(in_z[h])) {  // run 10 item 10: that zone holds it now (and no other does)
                        for (int& zi : zone_item)
                            if (zi == st.weapon) zi = -1;
                        zone_item[in_z[h]] = st.weapon;
                    }
                }
                const bool fists = g_unarmed.load(std::memory_order_relaxed);
                if (fists) {
                    const uint64_t m[2] = {1, 1};
                    api::queue_native(kMeleeModeSelected, m, 2, 0, nullptr);
                    g_unarmed_sets.fetch_add(1, std::memory_order_relaxed);
                }
                log::info("[holster] %s hand let go at the %s: put away%s", h ? "right" : "left", zones[in_z[h]].key,
                          fists ? ", the fists selected" : "");
                hold_place(st, now_ms);
                if (st.valid) {  // (round 76) John's move in the second after, as the let-go's
                    letgo_tr.ms = now_ms;
                    std::memcpy(letgo_tr.p, st.pos, sizeof(letgo_tr.p));
                    letgo_tr.h = st.heading_deg;
                    letgo_tr.what = st.weapon >= 0 && st.weapon < kWeapons ? kWeaponLabel[st.weapon] : "?";
                }
            }
            g_consumed[h] = false;
            pending[h] = false;
            press_zone[h] = -1;
        }
    }
    {  // [Holsters] HoldToKeep (g_hold's comment): each controller's gun held by its grip, after the loop's presses and let-gos
        const dual::State dn = dual::state();  // (the loop may have begun or ended one)
        const bool placing = g_place.load(std::memory_order_relaxed) || g_piece_place.load(std::memory_order_relaxed);
        const bool gun_in = in_hand && keepable(st.weapon);  // (a gun, or with HoldItems the knife, torch, lasso, a throwable)
        if (in_hand && offhand::kind_of(st.weapon) == offhand::kThrown) offhand::prime_type(st.weapon);  // (HoldItems' throw counts it)
        // (HoldItems) the item's own let-go waits: a knife's or torch's swing (put away once the hand slows: a swing with a
        // grip dip keeps it), the lasso's rope held (the game's aim trigger held, info +0x244 1 or 2, or a catch, L +0x18 5:
        // research/run17/lasso.md 1.2)
        auto item_wait = [&](int c) -> const char* {
            const int w = keep_w[c];
            if (!g_hold_items.load(std::memory_order_relaxed) || !hold_item_kind(w)) return nullptr;
            if (w == 22 || w == 33) {
                float v[3], o[3], sp = 0.0f;
                if (gestures::hand_throw(c, 60.0, v, o, &sp) && sp > gestures::swing_speed()) return "a swing under way";
            } else if (w == 21) {
                const aim::LassoProbe lp = aim::lasso_probe();
                if (lp.ok && (lp.user_state == 5 || lp.aim_state == 1 || lp.aim_state == 2)) return "the lasso's rope held (LT, or a catch)";
            }
            return nullptr;
        };
        // controller c's own let-go waits (as the list below has them for its hand): its gun not seen yet, the rounds it let
        // go of, the chest where its grip takes rounds, where RoundSource gives them
        auto own_wait = [&](int c) {
            const int jc = bp.ctrl[0] == c ? 0 : 1;
            return !keep_seen[c] || (up_since[c] >= 0.0 && rounds_let_ms[c] >= up_since[c] && now_ms - rounds_let_ms[c] < kHoldRoundsMs) ||
                   (zone_rounds(in_z[c]) && (cross_tgt(c) >= 0 || reload::chest_squeeze() != 0)) || hand_take_at(c, jc, in_z[c]);
        };
        {
            const int hw = in_hand ? st.weapon : -1;
            if (hw != hand_was) {
                hand_was = hw;
                hand_since = now_ms;
            }
        }
        for (int h = 0; h < 2; ++h) {
            const char* const side = h ? "right" : "left";
            auto what = [&]() -> const char* {
                return keep[h] == 2 ? (dn.copy ? "copy" : "second gun") : keep_w[h] >= 0 && keep_w[h] < kWeapons ? kWeaponLabel[keep_w[h]] : "gun";
            };
            if (!grip_down[h]) loose_held[h] = false;
            if (!hold_on || menu::visible() || placing) {  // loose: it stays until its grip is pressed (taken) and let go
                if (keep[h] != 0) {
                    log::info("[holster] %s hand's %s loose (HoldToKeep %s): it stays until that grip is pressed and let go", side, what(),
                              !hold_on ? "off, or the weapon wheel's mode" : placing ? ": placing by hand" : ": the menu has the controllers");
                    if (hold_on) g_hold_loose.fetch_add(1, std::memory_order_relaxed);
                    loose_held[h] = hold_on && grip_down[h];
                    keep[h] = 0;
                }
                continue;
            }
            const int jh = bp.ctrl[0] == h ? 0 : 1;
            // (not a press of the gun hand in Dead Eye: its grip is the game's paint there)
            if (keep[h] == 0 && !hold_drew[h] && (hold_press[h] || loose_held[h]) && now_ms - hold_put_ms >= kHoldPutMs && !(deadeye_now && h == gun_h)) {
                // a press on a gun no grip holds (the game's own draw: LT, a script, a cutscene's end; a gun loose): this grip
                // takes it, as if drawn (its let-go puts it away; two-handed, the carry). Made loose by the menu (or the body
                // lost) with this grip held, and still held now: held again, without a press
                loose_held[h] = false;
                int k = 0, w = -1;
                if (h == gun_h && gun_in && keep[1 - h] != 1) k = 1, w = st.weapon;
                else if (dn.on && !dn.carry && dn.ctrl == h) k = 2, w = dn.weapon;
                if (k) {
                    hold_take(h, k, w, -1, true);  // (the game's own draw may still be playing its clip)
                    g_hold_adopts.fetch_add(1, std::memory_order_relaxed);
                    log::info("[holster] %s hand's grip takes the %s (HoldToKeep: no grip held it; let go to put it away)", side,
                              k == 2 ? (dn.copy ? "copy" : "second gun") : w >= 0 && w < kWeapons ? kWeaponLabel[w] : "gun");
                }
            }
            if (keep[h] == 0) continue;
            // the gun still where the hold left it: in the hand (the drawn one once it came), the second gun out in this hand
            if (keep[h] == 1) {
                if (!keep_seen[h]) {
                    if (keep_w[h] >= 0 && gun_in && st.weapon == keep_w[h]) {
                        keep_seen[h] = true;
                    } else if (now_ms - keep_ms[h] > 2500.0) {
                        log::info("[holster] %s hand's draw not in the hand 2.5 s on (weapon %d, in hand %d): not held (HoldToKeep)", side, keep_w[h],
                                  in_hand ? st.weapon : -1);
                        keep[h] = 0;
                        continue;
                    }
                } else if (!gun_in || st.weapon != keep_w[h]) {
                    g_hold_gone.fetch_add(1, std::memory_order_relaxed);
                    log::info("[holster] %s hand's %s gone from the hand (the game's: a holster, a cutscene, the wheel, mounting): no longer held "
                              "(HoldToKeep)", side, what());
                    keep[h] = 0;
                    continue;
                }
            } else {
                const bool out = dn.on && !dn.carry && dn.ctrl == h;
                if (!keep_seen[h]) {
                    if (out) {
                        keep_seen[h] = true;
                    } else if (sec_wait[h].slot < 0) {  // (refused, or its slot never ready)
                        keep[h] = 0;
                        continue;
                    }
                } else if (!out) {
                    g_hold_gone.fetch_add(1, std::memory_order_relaxed);
                    log::info("[holster] %s hand's %s no longer out: no longer held (HoldToKeep)", side, what());
                    keep[h] = 0;
                    continue;
                }
            }
            if (grip_down[h]) {  // held: the grip is the hold's (and its press again after a blip); in Dead Eye the gun hand's is the game's
                g_consumed[h] = !(deadeye_now && h == gun_h && in_z[h] < 0);
                keep_why[h] = nullptr;
                continue;
            }
            if (deadeye_now && h == gun_h && in_z[h] < 0) g_consumed[h] = false;  // (Dead Eye: its next press the game's paint)
            const double len = up_since[h] >= 0.0 ? now_ms - up_since[h] : 0.0;
            // (2026-10-31, the off-hand build step 3, R10; 53.4 "slight delay during the handover", r77 h3: 0.2 s, of it the 100 ms
            // debounce) [Holsters] HandOverLetGoMs: the two-handed gun hand's let-go, the front hand holding the foregrip, is a
            // hand-over after this shorter wait (a blip there costs nothing: the gun hand still at the grip takes it back)
            // (2026-11-01, GOAL-offhand-fixes F7, F8) no carry can start for this gun at all: the gun hand's let-go puts it
            // away as a one-handed gun's, not held while the front hand holds the foregrip. (GOAL-offhand-changes C16: F7, F8
            // misread the user, who wants the carry there: drawn into John's left hand his right carries, and riding carries;
            // only John's front hand on another controller than the front one is left)
            const bool carry_never = bp.hand_ok[0] && bp.hand_ok[1] && bp.ctrl[fj] != off_h;
            const bool to_carry = keep[h] == 1 && h == gun_h && long_gun && dual::carry_enabled() && two_hold[off_h] && grip_down[off_h] && !carry_never;
            if (len < (to_carry ? std::fmin(hold_letgo, g_handover_letgo.load(std::memory_order_relaxed)) : hold_letgo))
                continue;  // (not yet a let-go: a blip if the grip comes back in time)
            const char* wait = nullptr;
            if (!keep_seen[h]) {
                if (keep[h] == 2) {  // let go before the second gun was ready: none
                    sec_wait[h].slot = -1;
                    log::info("[holster] %s hand let go before the second gun was ready (HoldToKeep, %.0f ms): not taken", side, len);
                    keep[h] = 0;
                    continue;
                }
                wait = "the drawn gun not in the hand yet";
            } else if (busy) {
                wait = busy;
            } else if (now_ms - busy_last < kHoldBusyGraceMs) {
                wait = "just after Dead Eye, a game menu or a scene";
            } else if (now_ms - seat_ms < kHoldSeatMs) {
                wait = "mounting or dismounting";
            } else if (keep[h] == 1 && (st.weapon_flags & RDRVR_WEAPON_RELOADING)) {
                wait = "the game's reload";
            } else if (keep[h] == 1 && keep_anim[h] && now_ms - hand_since < kHoldDrawSettleMs) {
                wait = "the game's draw clip still playing";
            } else if (keep[h] == 1 && dual::carry_returning()) {
                wait = "the carried gun's take-back under way";
            } else if (up_since[h] >= 0.0 && rounds_let_ms[h] >= up_since[h] && now_ms - rounds_let_ms[h] < kHoldRoundsMs) {
                wait = "let go of its rounds: the grip again keeps the gun";
            } else if (keep[h] == 1 && dn.on && !dn.copy && !dn.carry && dn.ctrl == 1 - h && keep[1 - h] == 2 && !grip_down[1 - h] &&
                       up_since[1 - h] >= 0.0 && now_ms - up_since[1 - h] < hold_letgo) {
                wait = "the second gun's hand let go too: its debounce first";
            } else if (keep[h] == 1 && h == gun_h && long_gun && dual::carry_enabled() && !carry_never && (two_hold[off_h] || (retwo[off_h] && grip_down[off_h]))) {
                // (rule 3) two-handed: the front hand carries it, at once; until a carry can start, the gun stays in hand while
                // the front hand holds it (taken back a moment ago: the front hand two-handed again first)
                if (two_hold[off_h] && carry_can(h) && !trig_grab[off_h]) {
                    if (now_ms - keep_try_ms[h] >= 500.0) {  // (a capture not known yet: tried again, twice a second)
                        keep_try_ms[h] = now_ms;
                        if (hand_over(h, true, keep_from[h] >= 0 ? keep_from[h] : drawn_from, keep_try_n[h]++ > 0)) {
                            ho_ms[h] = now_ms;  // (a grip again within 300 ms: taken back once out of the hand)
                            g_hold_carries.fetch_add(1, std::memory_order_relaxed);
                            log::info("[holster] (HoldToKeep: the %s hand's grip let go %.0f ms, the front hand on the foregrip: the carry)", side, len);
                            keep[h] = 0;
                            continue;
                        }
                    }
                    wait = "two-handed, the hand-over not made yet";
                } else {
                    wait = !two_hold[off_h] ? "taken back, two-handed again in a moment"
                                            : "two-handed, no carry now: cover, the game's reload, an action not worked, not fully two-handed";
                }
            } else if (keep[h] == 1 && item_wait(h)) {
                wait = item_wait(h);
            } else if (zone_rounds(in_z[h]) && (cross_tgt(h) >= 0 || reload::chest_squeeze() != 0)) {
                wait = "at the chest, where its grip takes rounds";
            } else if (hand_take_at(h, jh, in_z[h])) {
                wait = "where its grip takes rounds";
            }
            if (wait) {
                if (wait != keep_why[h]) {
                    if (!keep_why[h]) g_hold_waits.fetch_add(1, std::memory_order_relaxed);  // (the let-go's waits counted once)
                    keep_why[h] = wait;
                    log::info("[holster] %s hand let go of the %s (HoldToKeep, %.0f ms): it waits (%s)", side, what(), len, wait);
                }
                continue;
            }
            if (keep[h] == 1) {
                const int kw = keep_w[h];
                if (g_hold_items.load(std::memory_order_relaxed) && offhand::kind_of(kw) == offhand::kThrown) {
                    // the game's own throw (armed by the trigger, a throw clip started) or its aim (LT: it lights dynamite and
                    // the fire bottle) has it (the review)
                    const aim::LassoProbe ap = aim::lasso_probe();
                    // (round 122: RT no longer throws the knives nor the tomahawk, so the game's aim, LT, has no throw of
                    // them to give: theirs is the hand's, the review)
                    const bool aim_has = ap.ok && (ap.aim_state == 1 || ap.aim_state == 2) && kw != 25 && kw != 29;
                    if (aim::grip_throw_busy() || aim_has) {
                        g_hold_item_game_throws.fetch_add(1, std::memory_order_relaxed);
                        log::info("[holster] %s hand let go of the %s (HoldToKeep, %.0f ms): the game's throw (armed by the trigger)", side, what(), len);
                        keep[h] = 0;
                        // (C1) its next throwable put away once the throw has launched: from the let-go's start (the review: the held
                        // throw launches inside the let-go's debounce), the same throwable only (not one the wheel gives)
                        game_throw_ms[h] = now_ms - len - 50.0;
                        game_throw_w[h] = kw;
                        continue;
                    }
                    float sp = -1.0f;
                    // (the review) the motion from 200 ms before the let-go began, however long its waits held it (at most 1 s)
                    const double win = std::fmin(1000.0, len + 200.0);
                    if (offhand::throw_in_hand(h, kw, st.clip, static_cast<uint32_t>(st.actor), &sp, win)) {
                        g_hold_item_throws.fetch_add(1, std::memory_order_relaxed);
                        log::info("[holster] %s hand let go of the %s in a throw (HoldItems, %.2f m/s): thrown unlit, put away", side, what(), sp);
                    }
                }
                const int oc = 1 - h;
                hold_put_away(h, len, keep[oc] == 2 && !grip_down[oc] && up_since[oc] >= 0.0 && now_ms - up_since[oc] >= hold_letgo && !own_wait(oc));
            } else {
                dual::end((std::string("let go (HoldToKeep): back to the ") + (keep_from[h] >= 0 && keep_from[h] < kZones ? zones[keep_from[h]].key : "holster")).c_str());
                g_hold_sec_puts.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.4f, 25);
                log::info("[holster] %s hand let go of the %s (HoldToKeep, %.0f ms): put back", side, what(), len);
                keep[h] = 0;
            }
        }
        // (2026-11-01, GOAL-offhand-changes C1; round 62 check 4, the user: after a throwable is thrown the game puts another in
        // the hand; the hand should be free) HoldItems: the game's own throw (armed by the trigger) let go by hand h, launched
        // since (aim's game_throw_launch_ms), 150 ms on: a throwable in hand then (the game's next) put away, unless h's grip
        // holds it again; looked for 4 s after the let-go
        for (int h = 0; h < 2; ++h) {
            if (game_throw_ms[h] < 0.0) continue;
            const double lm = aim::game_throw_launch_ms();
            if (now_ms - game_throw_ms[h] > 4000.0 || grip_down[h] || keep[h] != 0) {
                game_throw_ms[h] = -1e12;
                continue;
            }
            if (lm < game_throw_ms[h] || now_ms - lm < 150.0 || aim::grip_throw_busy()) continue;
            const aim::LassoProbe tp = aim::lasso_probe();  // (the review: never while the game aims it: LT lights dynamite)
            if (tp.ok && (tp.aim_state == 1 || tp.aim_state == 2)) continue;
            if (in_hand && h == gun_h && st.weapon == game_throw_w[h] && offhand::kind_of(st.weapon) == offhand::kThrown &&
                !put_in_hand_other(st.weapon)) {  // (item 4.6, the fix's review: not a draw the other hand just made)
                put_item_away(st.actor, st.weapon);
                g_puts.fetch_add(1, std::memory_order_relaxed);
                hold_put_ms = now_ms;
                log::info("[holster] the %s after the game's throw put away: the %s hand free (HoldItems)", kWeaponLabel[st.weapon], h ? "right" : "left");
                game_throw_ms[h] = -1e12;
            }
        }
        for (int h = 0; h < 2; ++h) {
            // (2026-11-05, round 87: a swing back through a holster's zone, the grip held, was refused as a put-away) in a zone too
            g_keeping[h].store(keep[h] != 0 && grip_down[h] && !round_held[h], std::memory_order_relaxed);
            g_keep_pub[h].store(keep[h], std::memory_order_relaxed);
            g_keep_w_pub[h].store(keep[h] ? keep_w[h] : -1, std::memory_order_relaxed);
            g_keep_up_pub[h].store(keep[h] && up_since[h] >= 0.0 ? static_cast<float>(now_ms - up_since[h]) : -1.0f, std::memory_order_relaxed);
        }
    }
    // (S3) a hand holds rounds for the other hand's gun: actions' wrist-flick shut of that gun waits (r79 s2: an 8 rad/s roll
    // with a round held shut it)
    g_rounds_for_sec.store((round_held[0] && round_tgt[0] == 1) || (round_held[1] && round_tgt[1] == 1), std::memory_order_relaxed);
    {  // (2026-10-31, the step 6 review) a second gun left beside nothing (the game's own throw took the last throwable, or any
       // way the hand emptied but a put-away, which hands it over itself): after 300 ms it becomes the gun in hand, as there
        static double orphan_ms = -1.0;
        const dual::State dn = dual::state();
        // (the review: only with nothing in hand, never over another item; and only once the game's own states are over)
        const bool orphan = dn.on && !dn.copy && !dn.carry && dn.ready && dn.weapon >= 0 && dn.weapon < kWeapons && dn.ctrl >= 0 && dn.ctrl <= 1 &&
                            !(in_hand && st.weapon >= 0) && !menu::visible() && hold_settled;
        if (!orphan) {
            orphan_ms = -1.0;
        } else if (orphan_ms < 0.0) {
            orphan_ms = now_ms;
        } else if (now_ms - orphan_ms >= 300.0) {
            orphan_ms = -1.0;
            const int sec_ctrl = dn.ctrl, sec_w = dn.weapon;
            dual::end("nothing in hand beside it: it becomes the gun in hand");
            const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(sec_w), 0ull};
            queue_put_in_hand(args);
            controls::set_draw_hand(sec_ctrl, sec_w, false);
            if (keep[sec_ctrl] == 2) hold_take(sec_ctrl, 1, sec_w, sec_from[sec_ctrl]);
            log::info("[holster] the second gun (the %s) beside nothing: the gun in hand now, in the %s hand", kWeaponLabel[sec_w], sec_ctrl ? "right" : "left");
        }
    }
    {  // [Hands] AimWhenRaised: the gun hand raised (the barrel near level, the hand ahead of the chest), debounced
        bool want = false;
        const bool was = g_raised.load(std::memory_order_relaxed);
        g_gun_at_zone.store(in_z[gun_h] >= 0, std::memory_order_relaxed);
        g_raise_dbg[3].store(static_cast<float>((gun ? 1 : 0) | (bp.hand_ok[gj] ? 2 : 0) | (bp.bone_ok[4] ? 4 : 0) | (in_z[gun_h] < 0 ? 8 : 0) |
                                                (!round_held[0] && !round_held[1] ? 16 : 0)),
                             std::memory_order_relaxed);
        if (gun && bp.hand_ok[gj] && bp.bone_ok[4] && in_z[gun_h] < 0 && !round_held[0] && !round_held[1]) {
            const float pitch = std::asin(u[1] < -1.0f ? -1.0f : u[1] > 1.0f ? 1.0f : u[1]) * 57.2957795f;
            const float dx = bp.hand[gj][0] - bp.bone[4][0], dz = bp.hand[gj][2] - bp.bone[4][2];
            const float reach = std::sqrt(dx * dx + dz * dz), ahead = dx * u[0] + dz * u[2];
            g_raise_dbg[0].store(pitch, std::memory_order_relaxed);
            g_raise_dbg[1].store(reach, std::memory_order_relaxed);
            g_raise_dbg[2].store(ahead, std::memory_order_relaxed);
            want = was ? pitch > g_raise_pmin - 10.0f && pitch < g_raise_pmax + 10.0f && reach > g_raise_reach - 0.07f
                       : pitch > g_raise_pmin && pitch < g_raise_pmax && reach > g_raise_reach && ahead > 0.0f;
        }
        if (want == was) {
            flip_since = 0;
        } else if (!gun || in_z[gun_h] >= 0) {
            g_raised.store(false, std::memory_order_relaxed);  // put away, or into a holster: at once
            flip_since = 0;
        } else {
            if (flip_since == 0) flip_since = now_ms;
            if (now_ms - flip_since >= (want ? 60.0 : 120.0)) {
                g_raised.store(want, std::memory_order_relaxed);
                if (want) g_raises.fetch_add(1, std::memory_order_relaxed);
                flip_since = 0;
            }
        }
    }
    {  // the two-handed weight toward its target, a tenth of a second either way
        const float target = two_hold[0] || two_hold[1] ? 1.0f : 0.0f;
        float b = g_two_blend.load(std::memory_order_relaxed);
        const float step = dt * 10.0f;
        b = b < target ? (b + step > target ? target : b + step) : (b - step < target ? target : b - step);
        g_two_blend.store(b, std::memory_order_relaxed);
    }
    {  // [Reload] RoundInHand: John's hand holding a round, for its draw (round_draw)
        int rh = -1, rn = 1, rw = st.weapon;
        int mh = -1, mn = 0, mw = -1;  // ([Reload] PistolMagazines: John's hand holding a magazine, its rounds; C7: its gun, -1 the gun in hand)
        for (int h = 0; h < 2; ++h) {
            const int jm = bp.ctrl[0] == h ? 0 : 1;
            g_mag_held[h].store(round_held[h] && round_mag[h] ? 1 : 0, std::memory_order_relaxed);
            {
                float tp[3] = {0.0f, 0.0f, 0.0f}, mdp[3] = {0.0f, 0.0f, 0.0f};
                g_mag_top_d[h].store(round_held[h] && round_mag[h] ? mag_top_d(jm, tp, mdp) : -1.0f, std::memory_order_relaxed);
                std::lock_guard lock(g_zone_mutex);
                std::memcpy(g_mag_top_pub[h], tp, sizeof(tp));
                std::memcpy(g_mag_mid_pub[h], mdp, sizeof(mdp));
            }
            if (round_held[h] && round_mag[h]) {
                mh = jm;
                mn = round_n[h];
                if (round_tgt[h] == 1) mw = ds.weapon;  // (C7) the second pistol's magazine
            }
            if (round_held[h] && !round_mag[h]) {
                rh = bp.ctrl[0] == h ? 0 : 1;
                rn = round_n[h];
                // [Hands] DualWieldCrossReload: the rounds of the gun they are for (the second gun's calibre in the gun hand)
                rw = round_tgt[h] == 1 && sec_shown >= 0 ? sec_shown : st.weapon;  // (the copy's own model's)
            }
            // ([Reload] RoundSource: the status line's: each controller's rounds and the gun drawn for them)
            g_held_n[h].store(round_held[h] ? round_n[h] : 0, std::memory_order_relaxed);
            g_held_w[h].store(round_held[h] ? (round_tgt[h] == 1 && sec_shown >= 0 ? sec_shown : st.weapon) : -1, std::memory_order_relaxed);
        }
        // RoundsPerGrab: every round held drawn; 2026-10-27: the gun itself (round_draw::kind_of: its own calibre's round) and
        // the body's forward (the new rounds' light, fixed in the world)
        round_draw::set_held(rh, rw, rn, hf);
        round_draw::set_held_mag(mh, mw >= 0 ? mw : st.weapon, mn);
        {
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_mag_well_pub, mag_pt, sizeof(g_mag_well_pub));
            g_mag_well_ok = mag_pt_ok;
        }
        // [Reload] ShowAmmo (run 6 item 6b): while a round could go in now (the gun open for it, or a gate gun with room),
        // a row of the gun's rounds at the chest: as many as could go in, up to six
        if (round_draw::show_ammo()) {
            int rz = -1;  // run 10 item 10: the zone holding the rounds (the chest first)
            if (zone_rounds(kAmmoZone)) rz = kAmmoZone;
            for (int z = 0; z < kZones && rz < 0; ++z)
                if (zone_rounds(z)) rz = z;
            const bool room = gun && reload::hand_reload() && st.clip + 0.5f <= st.clip_max && st.spare >= 1.0f && actions::can_load(st.weapon) &&
                              !(st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING)) && rz >= 0 && zok[rz];
            // (2026-10-31, the off-hand build step 7, R12; the user: "Ammo in chest holster should disappear when it is taken by
            // the hand and return if ammo is dropped"; r80 C1: the row stayed at 2 with a round in hand) less the rounds a hand
            // holds for this gun (dropped, they are back by themselves: nothing left the spare)
            int held_n = 0;
            for (int hh = 0; hh < 2; ++hh)
                if (round_held[hh] && !round_mag[hh] && round_tgt[hh] == 0) held_n += round_n[hh];
            const float can = std::fmax(0.0f, std::fmin(std::fmin(st.spare, st.clip_max - st.clip) - static_cast<float>(held_n), 6.0f));
            // (2026-11-01, step 7, R12: a row for the cross-reload target) no room in the gun in hand, the other gun's (the
            // second gun, the copy, the carried gun) as its take would load it: its calibre (the copy's the gun in hand's), less
            // the rounds held for it
            const bool sec_room = !room && sec_loadable && rz >= 0 && zok[rz];
            int held_s = 0;
            for (int hh = 0; hh < 2; ++hh)
                if (round_held[hh] && !round_mag[hh] && round_tgt[hh] == 1) held_s += round_n[hh];
            const float can_s = std::fmax(0.0f, std::fmin(std::fmin(sec_spare, ds.clip_max - ds.clip) - static_cast<float>(held_s), 6.0f));
            const int row_w = room ? st.weapon : sec_room ? (ds.copy ? st.weapon : ds.weapon) : -1;
            round_draw::set_row(row_w, room ? static_cast<int>(can + 0.01f) : sec_room ? static_cast<int>(can_s + 0.01f) : 0, zp[rz >= 0 ? rz : kAmmoZone],
                                hr, up, hf);
            g_row_cross.store(sec_room, std::memory_order_relaxed);
            // (2026-11-01, step 7, R12: the magazine and clip row) a magazine pistol that takes one now (mag_loadable: its
            // magazine out, the Mauser open for its clip; spare rounds): its magazines (or clips) standing at the chest, as
            // many as the spare fills (up to 2; C6: one), none while a hand holds one
            bool mag_held = false;
            for (int hh = 0; hh < 2; ++hh) mag_held = mag_held || (round_held[hh] && round_mag[hh]);
            const int mcap = st.clip_max >= 1.0f ? static_cast<int>(st.clip_max + 0.01f) : 0;
            const bool mrow = mag_loadable && !mag_held && mcap > 0 && st.spare >= 1.0f && rz >= 0 && zok[rz];
            // (2026-11-01, GOAL-offhand-changes C6; round 63 check 12, the user: one magazine is enough at the chest, only one is
            // ever put in) one, not up to two
            const int mn = mrow ? 1 : 0;
            round_draw::set_mag_row(mrow ? st.weapon : -1, mn, mrow ? std::min(mcap, static_cast<int>(st.spare + 0.01f)) : 0, zp[rz >= 0 ? rz : kAmmoZone], hr,
                                    up, hf);
        } else {
            g_row_cross.store(false, std::memory_order_relaxed);  // (the review: never a stale "row cross 1")
        }
        // [Reload] RoundInHand = 2: the game's own shotgun shell (held_prop; each draw moved onto its pose). 2026-10-27 (the
        // user: "Use the games shotgun shells"; research/run27/rounds.md 5): everywhere a shell is drawn, the handful and the
        // row at the chest, from a pool made while a shotgun is in hand (not one shell made and destroyed each grab): as
        // many objects as a grab and the row can want (RoundsPerGrab and the row's six, each no more than the gun holds;
        // grown, never shrunk, while that gun is in hand), spare ones hidden; round_draw draws its own shell for any pose
        // the pool does not draw yet (the first 2 s of a session, a creation that failed). Slot 0's single shell retired
        held_prop::want(0, nullptr, nullptr);
        {
            static int pool_w = -1, pool_n = 0;
            const bool pool = gun && round_draw::mode() == 2 && round_draw::kind_of(st.weapon) == round_draw::kKShell;
            if (!pool || st.weapon != pool_w) pool_n = 0;
            pool_w = pool ? st.weapon : -1;
            if (pool) {
                const int cm = st.clip_max >= 1.0f ? static_cast<int>(st.clip_max + 0.01f) : 8;
                const int grab = std::min(g_per_grab.load(std::memory_order_relaxed), cm);
                const int want_n = std::min(grab + (round_draw::show_ammo() ? std::min(6, cm) : 0), held_prop::kPoolMax);
                pool_n = std::max(pool_n, want_n);
            }
            float sp[held_prop::kPoolMax * 12];
            int n_hand = 0;
            const int np = pool ? round_draw::shell_poses(bp, sp, held_prop::kPoolMax, &n_hand) : 0;
            const int off = bp.gun == 0 ? 1 : 0;  // John's hand that takes the rounds: the spares wait there
            const float* base = rh >= 0 && bp.hand_ok[rh] ? bp.hand[rh] : bp.hand_ok[off] ? bp.hand[off] : bp.root;
            const float* pool_anc = bp.bone_ok[body::kPelvisPoint] ? bp.bone[body::kPelvisPoint] : bp.root;  // (the row walks with John)
            held_prop::want_pool(pool ? "p_gen_shellshotgun01x" : nullptr, pool_n, base, sp, np, n_hand, pool_anc);
        }
        // the copy's own model (body.cpp wants it at each placement while the copy is out with another model)
        if (!dual::copy_W() || !dual::sidearm_fragment(dual::copy_model())) held_prop::want(1, nullptr, nullptr);
        // [Hands] OffHandItems: the off hand's item at its hand (held_prop kOffhandSlot); (O4) beside an item in hand too
        offhand::frame(&bp, &st, gb, gun_h, gun || (in_hand && hold_item_kind(st.weapon) && st.weapon != 21));
        // [Holsters] ShowGuns: each gun zone's gun at it (the gun it draws; automatic: its slot's last gun in hand, else
        // the highest owned of its slot), none while that gun is in a hand (the gun in hand, the second gun)
        const bool show = g_show_guns.load(std::memory_order_relaxed);
        const bool show_back = g_show_back_guns.load(std::memory_order_relaxed);
        ShowDiag sd;
        {
            std::lock_guard lock(g_zone_mutex);
            sd = g_show_diag;
        }
        int shown_w[4] = {-1, -1, -1, -1};
        // the props' anchor: the pelvis as drawn (body.cpp publishes the same point to held_prop for the draws)
        const float* anc = bp.bone_ok[body::kPelvisPoint] ? bp.bone[body::kPelvisPoint] : bp.root;
        for (int i = 0; i < 4; ++i) {
            const int zi = kShowZones[i];
            int gw = -1;
            if (show && zok[zi] && !zone_rounds(zi) && (show_back || (i != 1 && i != 3))) {  // 1, 3: the back, the left shoulder
                gw = draw_weapon(zi);
                const int s0 = zones[zi].slots[0];
                if (gw < 0 && s0 >= 0 && s0 < 8) gw = slot_weapon[s0];
                if (gw < 0 && choosing)
                    for (int w = kWeapons - 1; w >= 0 && gw < 0; --w)
                        if (owns(w) && st.equip_slot[w] == s0) gw = w;
            }
            shown_w[i] = gw;
        }
        // run 7 item 1b: twins never show the same gun (one sidearm owned: one hip shows it). Before, only a twin's
        // automatic gun was checked against its partner's, so a twin's own last gun (draw_weapon) could be the gun its
        // partner shows by its slot's fallback (one sidearm, last drawn from the left hip: both hips showed it). The
        // hip it was last put away in keeps it, else the one that has it by choice or by its own last draw, else the
        // first; the other shows nothing. (The table names the partner on the left twin only.)
        // (2026-10-26: any two holsters, not only twins: the Sawed-off fits a hip and a back; and two holsters that both chose
        // the gun show it at both, "two of the same gun in each holster")
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j) {
                const int zi = kShowZones[i], zj = kShowZones[j], gw = shown_w[i];
                if (gw < 0 || gw != shown_w[j]) continue;
                if (owns(zones[zi].weapon) && zones[zi].weapon == gw && owns(zones[zj].weapon) && zones[zj].weapon == gw) continue;
                auto has = [&](int z) { return zones[z].weapon == gw || zone_last[z] == gw; };
                const int put = gw < kWeapons ? put_zone_of[gw] : -1;
                const bool keep_i = put == zi ? true : put == zj ? false : has(zi) || !has(zj);
                shown_w[keep_i ? j : i] = -1;
                g_twin_hidden.fetch_add(1, std::memory_order_relaxed);
            }
        // the guns out (in hand, the second gun, the copy's model) hidden at their holsters: as many holsters as copies are
        // out, the one each came from first (2026-10-26: two holsters that chose the same gun show it twice; one drawn,
        // the other still shows it). Before, every holster showing a gun out hid it.
        {
            int out_w[2] = {-1, -1}, out_z[2] = {-1, -1}, n_out = 0;
            if (in_hand && st.weapon >= 0) {
                out_w[n_out] = st.weapon;
                out_z[n_out++] = drawn_from;
            }
            if (ds.on) {
                // (copy -1: the gun in hand's model; a carry before its first holstered update: its weapon from the hand-over)
                const int sw = ds.copy ? (dual::copy_model() >= 0 ? dual::copy_model() : st.weapon) : ds.weapon >= 0 ? ds.weapon : ds.carry_weapon;
                const int sz = ds.ctrl >= 0 && ds.ctrl < 2 ? sec_from[ds.ctrl] : -1;
                if (sw >= 0) {
                    out_w[n_out] = sw;
                    out_z[n_out++] = sz;
                }
            }
            for (int k = 0; k < n_out; ++k) {
                int hide = -1;
                for (int i = 0; i < 4 && hide < 0; ++i)
                    if (shown_w[i] == out_w[k] && kShowZones[i] == out_z[k]) hide = i;  // its own holster
                for (int i = 0; i < 4 && hide < 0; ++i)
                    if (shown_w[i] == out_w[k]) hide = i;  // else the first showing it
                if (hide >= 0) shown_w[hide] = -1;
            }
        }
        for (int i = 0; i < 4; ++i) {
            const int zi = kShowZones[i];
            const int gw = shown_w[i];
            const char* frag = gw >= 0 ? gun_fragment(gw) : nullptr;
            float gp[12];
            if (frag) {
                show_pose(zi, zp[zi], hr, up, hf, bp.root, gp);
                adjust_pose(zones[zi].gun_turn, zones[zi].gun_off, zp[zi], hr, up, hf, gp);
                float dp[3], rel[3];
                double dms = 0;
                uint64_t gated = 0;
                // the error of the frame drawn last, on the body: its drawn place less the anchor that draw used, against
                // this frame's wanted pose less its anchor (a frame's walk is not an error)
                const bool got = held_prop::last_drawn(held_prop::kHolsterSlot + i, dp, &dms, rel, &gated);
                sd.gated[i] = gated;
                if (sd.weapon[i] == gw && got && now_ms - dms < 100.0) {
                    const float ex = rel[0] - (gp[9] - anc[0]), ey = rel[1] - (gp[10] - anc[1]), ez = rel[2] - (gp[11] - anc[2]);
                    sd.err[i] = std::sqrt(ex * ex + ey * ey + ez * ez);
                    std::memcpy(sd.rel[i], rel, sizeof(rel));
                    sd.worst[i] = std::fmax(sd.worst[i], sd.err[i]);
                    if (sd.err[i] > 0.02f) ++sd.over[i];
                } else if (sd.weapon[i] == gw && got) {
                    ++sd.stale[i];
                }
            }
            sd.weapon[i] = frag ? gw : -1;
            g_shown_last[i] = frag ? gw : -1;
            g_shown_ms = log::now_ms();
            held_prop::want(held_prop::kHolsterSlot + i, frag, frag ? gp : nullptr, anc);
        }
        {  // [Holsters] ShowModels: the holster models at the hips (each enabled hip's, drawn or not)
            float mo[3], mr[3], raw[3];
            bool raw_on;
            {
                std::lock_guard lock(g_zone_mutex);
                std::memcpy(mo, g_model_off, sizeof(mo));
                std::memcpy(mr, g_model_rot, sizeof(mr));
                std::memcpy(raw, g_model_raw, sizeof(raw));
                raw_on = g_model_raw_on;
            }
            const bool models = g_show_models.load(std::memory_order_relaxed);
            for (int i = 0; i < 2; ++i) {
                const int zi = kModelZones[i];
                float mp[12];
                const bool want = models && zok[zi] && g_model_name[0];
                const float zero[3] = {0, 0, 0};
                if (want) model_pose(lefty ? 1 - i : i, zp[zi], hr, up, hf, mo, zero, mp);  // its place (the turn is the object's)
                held_prop::want(held_prop::kModelSlot + i, want ? g_model_name : nullptr, want ? mp : nullptr, anc);
                // its turn: SET_OBJECT_ORIENTATION's three angles (the model's tilt, its heading, its roll) from ModelTurn,
                // the heading plus the holsters' own (their frame's forward), the left hip's heading offset and roll reversed
                const float side = (lefty ? 1 - i : i) == 0 ? 1.0f : -1.0f;
                const float heading = std::atan2(-hf[0], -hf[2]) * 57.29578f;
                const float ang[3] = {mr[0], heading + side * mr[1], side * mr[2]};
                held_prop::want_angles(held_prop::kModelSlot + i, raw_on ? raw : ang);
            }
        }
        if (body::left_holster()) body::left_holster_refresh();  // run 12 item 5: the holster's geometry for its left copy
        if (body::own_pieces()) body::pieces_geom_refresh();    // [Holsters] OwnBeltRigid: the pieces' geometries
        {  // [Holsters] OwnBeltProps (item 2): the knife, the satchel and the lasso's coil on an outfit without its own (after
           // the scan's refresh: the review)
            const float* anc2 = bp.bone_ok[body::kPelvisPoint] ? bp.bone[body::kPelvisPoint] : bp.root;
            // (the review) none at the belt while that item is in a hand: the knife (22) in hand or the off hand's, the lasso (21)
            // (round 102) the knife and the lasso only once owned (the weapon manager's list; before its first read, taken as owned)
            auto owned_item = [&](int w) { return !st.owned_tick || (st.owned >> w & 1); };
            // (round 142) Undead Nightmare: the torch (33) at the knife's place when the knife's holster is set to the torch,
            // or no knife is owned and the torch is (the review: not by the melee slot's item, which is the torch while it is
            // drawn: the knife would leave the belt then)
            int knife_w = 22;
            {
                int kc = -1;
                for (int zi = 0; zi < kZones; ++zi)
                    if (!std::strcmp(zones[zi].key, "Knife")) kc = zones[zi].weapon;
                if (st.owned_tick && (st.owned >> 33 & 1) && (kc == 33 || !(st.owned >> 22 & 1))) knife_w = 33;
            }
            const bool knife_out = (in_hand && st.weapon == knife_w) || offhand::held_weapon(0) == knife_w || offhand::held_weapon(1) == knife_w;
            const bool lasso_out = in_hand && st.weapon == 21;
            {
                std::lock_guard lock(g_bp_mutex);
                for (int i = 0; i < 3; ++i) g_bp_frame[i] = hr[i], g_bp_frame[3 + i] = up[i], g_bp_frame[6 + i] = hf[i], g_bp_frame[9 + i] = anc2[i];
                g_bp_frame_ok = true;
            }
            const bool on = g_belt_props.load(std::memory_order_relaxed) && body::own_pieces();
            const bool force = g_belt_props_force.load(std::memory_order_relaxed);
            // (round 103) the pieces the outfit lacks (the sheath goes with the knife's piece, the knife itself only once owned
            // and not in a hand); the default model draws the sheath and the satchel
            const bool sheath = on && (force || !body::piece_present(1)), satchel = on && (force || !body::piece_present(2));
            const bool model = g_bp_model.load(std::memory_order_relaxed);
            uint32_t bm_mask = model ? (sheath ? 2u : 0u) | (satchel ? 4u : 0u) : 0u;
            // (the review) while the default model's object still lives (its destroy queued, a remake on the way), all of it
            // collapsed (1: no piece kept), never drawn whole as the game has it
            if (!bm_mask && held_prop::slot_holds(held_prop::kBeltPropSlot + 1, "player_default")) bm_mask = 1u;
            body::set_belt_model_pieces(bm_mask);
            // (round 117) [Holsters] OwnBeltThrowable: the thrown slot's item at the satchel's place, unless it is in a hand
            int tw = -1;
            const char* tfrag = nullptr;
            const bool thrown_on = g_bp_throwable.load(std::memory_order_relaxed);
            // (round 122, the user: "The default outfit has the model on the hip, but when you are holding the throwable, the
            // satchel reappears"; round 123: "when you run out of ammo with a throwing weapon, the satchel appears again") the
            // outfit's satchel hidden whenever the belt's item is an owned one of the four, in a hand, at the hip or none left
            bool thr_in_hand = false, thr_owned = false;
            // (round 121, the user: "apply this to all outfits"; "the model does not change when you change it in the mod
            // menu, it changes only when you unholster it afterwards") on every outfit (its own satchel hidden while the
            // throwable is shown); the item the satchel's holster draws: its menu choice, else the thrown slot's
            if (thrown_on && on) {
                // (the review) exactly what the satchel's holster draws (its choice, WeaponChoice's test, the item last drawn from
                // it, else the thrown slot's); not one of the four measured: none shown
                int dw = -1;
                for (int zi = 0; zi < kZones; ++zi)
                    if (!std::strcmp(zones[zi].key, "Belt")) dw = draw_weapon(zi);
                const int w = dw >= 0 ? dw : slot_gun(2);
                const bool in_a_hand = (in_hand && st.weapon == w) || offhand::held_weapon(0) == w || offhand::held_weapon(1) == w;
                // (the review) the measured ones only (kThrownOnBone; round 132: the Undead Nightmare's three with them, made in
                // cycle UN1), and one left at least (the last one thrown leaves the item in the slot: not shown while the count
                // is not known)
                const bool measured = throwable_index(w) >= 0;
                const char* wfrag = measured && owned_item(w) ? offhand::item_fragment(w) : nullptr;
                thr_owned = wfrag != nullptr;
                thr_in_hand = wfrag && in_a_hand;
                if (wfrag && !in_a_hand && offhand::thrown_left(w, static_cast<uint32_t>(st.actor)) == 1) tfrag = wfrag, tw = w;
            }
            for (int k = 0; k < kBeltProps; ++k) {
                const bool torch = k == 0 && knife_w == 33;
                const char* torch_frag = torch ? offhand::item_fragment(33) : nullptr;
                const bool out = (k == 0 && (knife_out || !owned_item(knife_w))) || (k == 2 && (lasso_out || !owned_item(21))) || (torch && !torch_frag);
                // (round 142) the torch whatever the outfit's own knife: it is not the outfit's
                bool want = k == 1 ? (model ? sheath || satchel : (satchel && (!thrown_on || tw >= 0)) || (thrown_on && tw >= 0))
                                   : on && !out && (force || torch || !body::piece_present(k + 1));
                float pz[12], an[3] = {anc2[0], anc2[1], anc2[2]};  // (the review: a bone's prop anchored by the pelvis its set drew)
                if (want) want = belt_prop_pose(k, hr, up, hf, k == 1 && model ? bp.root : anc2, pz, an, model, k == 1 ? tw : torch ? 33 : -1);
                const char* frag = k == 1 && model ? "player_default" : k == 1 && tw >= 0 ? tfrag : torch ? torch_frag : kBeltPropFrag[k];
                if (k == 1) {  // (round 120) the throwable shown, for placing it by hand
                    g_thr_shown_w = want && tw >= 0 ? tw : -1;
                    // (round 121) the outfit's own satchel, once the throwable's object is made (the review: not while it is made;
                    // cycle TB1d: held_prop::shown, by its matched draws, stayed false on the default outfit after an outfit change)
                    body::set_hide_satchel(body::piece_present(2) && thr_owned);
                    if (thr_in_hand && body::piece_present(2)) g_thr_hand_hides.fetch_add(1, std::memory_order_relaxed);
                    if (g_thr_shown_w >= 0) {
                        g_thr_shown_ms = log::now_ms();
                        std::lock_guard lock(g_bp_mutex);
                        for (int i = 0; i < 3; ++i) g_thr_shown_pos[i] = pz[9 + i];
                    }
                }
                held_prop::want(held_prop::kBeltPropSlot + k, want ? frag : nullptr, want ? pz : nullptr, an);
            }
        }
        ++sd.frames;
        {  // (round 107; the user: both holstered guns "lag behind and catch up", round 106's motion vectors no help) every
           // 10 s while a gun is shown: each one's drawn place on the body against the place wanted the tick after, the
           // worst and the ticks over 2 cm, and the ticks with no draw of it in 100 ms (its draws not moved)
            static double log_ms = 0.0;
            static float lw[4] = {};
            static uint64_t ln[4] = {}, lo0[4] = {}, ls0[4] = {};
            static const char* const kHip[4] = {"right hip", "back", "left hip", "left shoulder"};
            bool any_shown = false;
            for (int i = 0; i < 4; ++i)
                if (sd.weapon[i] >= 0) {
                    any_shown = true;
                    ++ln[i];
                    lw[i] = std::fmax(lw[i], sd.err[i]);
                }
            if (now_ms - log_ms >= 10000.0) {
                log_ms = now_ms;
                std::string o;
                char b[200];
                for (int i = 0; i < 4; ++i) {
                    if (ln[i])
                        std::snprintf(b, sizeof(b), " | %s (weapon %d) worst %.1f mm, over 2 cm %llu of %llu ticks, no draw in 100 ms %llu", kHip[i], sd.weapon[i],
                                      lw[i] * 1000.0f, static_cast<unsigned long long>(sd.over[i] - lo0[i]), static_cast<unsigned long long>(ln[i]),
                                      static_cast<unsigned long long>(sd.stale[i] - ls0[i]));
                    if (ln[i]) o += b;
                    lw[i] = 0.0f;
                    ln[i] = 0;
                    lo0[i] = sd.over[i];
                    ls0[i] = sd.stale[i];
                }
                if (any_shown && !o.empty()) log::info("[holster] 10 s: the shown guns drawn against their wanted place%s", o.c_str());
            }
        }
        {
            std::lock_guard lock(g_zone_mutex);
            g_show_diag = sd;
        }
        held_prop::frame(static_cast<uint32_t>(st.actor));
    }
    std::lock_guard lock(g_hdiag_mutex);
    g_hdiag.valid = true;
    std::memcpy(g_hdiag.zone, zp, sizeof(zp));
    std::memcpy(g_hdiag.zone_ok, zok, sizeof(zok));
    std::memcpy(g_hdiag.base, bases, sizeof(bases));
    std::memcpy(g_hdiag.root, bp.bone_ok[body::kPelvisPoint] ? bp.bone[body::kPelvisPoint] : bp.root, sizeof(g_hdiag.root));  // (the mirror's)
    std::memcpy(g_hdiag.right, right, sizeof(g_hdiag.right));
    for (int h = 0; h < 2; ++h) {
        const int jh = bp.ctrl[0] == h ? 0 : 1;
        g_hdiag.hand_ok[h] = bp.hand_ok[jh];
        std::memcpy(g_hdiag.hand[h], gb[jh], sizeof(g_hdiag.hand[h]));  // the interaction spot (the tests steer it)
        g_hdiag.in_zone[h] = in_z[h];
    }
    g_hdiag.cur_slot = cur;
    g_hdiag.in_hand = in_hand != 0;
    g_hdiag.cam_ok = bp.cam_ok;
    if (bp.cam_ok) std::memcpy(g_hdiag.cam, bp.cam + 12, sizeof(g_hdiag.cam));
    g_markers_valid = false;
    g_hl_valid = false;
    float hint_pos[3] = {};
    const int ahint = gun ? actions::hint(hint_pos) : 0;  // [Reload] ActionHints
    // (2026-10-30, round 52: "Glow points disappear when gun is being held by only the foregrip") the carried gun's want:
    // its bolt to work by the free gun hand ([Reload] CarriedBolt), its pump by the carrying hand ([Reload] CarriedPump)
    float chint_pos[3] = {};
    const int chint = ds.carry ? actions::carried_hint(chint_pos) : 0;
    uint32_t carried_ids = 0;  // (published once, after the highlights; 0 when they do not run)
    const bool placing = g_place.load(std::memory_order_relaxed);
    // [Holsters] InteractionHighlights=highlights: the places' rings give way to the highlights (placing keeps them: they
    // are what is moved); the hand dots stay as their switch says
    const int hl_mode = g_hl_mode.load(std::memory_order_relaxed);
    const bool ring_places = (hl_mode != 1 && hl_mode != 3) || placing;  // (parts: as highlights, the gun's places on its surface)
    const bool zones_shown = (g_show_zones.load(std::memory_order_relaxed) && ring_places) || placing;
    const bool dots_shown = g_show_dots.load(std::memory_order_relaxed),
               points_shown = g_show_points.load(std::memory_order_relaxed) && ring_places;
    float fan_pos[3], fan_r = 0.0f;
    int fan_st = 0;
    const bool fan_shown = actions::fan_zone(fan_pos, &fan_r, &fan_st);  // [Reload] ShowFanZone (2026-11-06)
    if ((zones_shown || dots_shown || points_shown || fan_shown) && bp.cam_ok) {
        Markers& mk = g_markers;
        std::memcpy(mk.cam, bp.cam, sizeof(mk.cam));
        mk.n = 0;
        auto add = [&](const float* p, float r, MarkerKind k, MarkerState s, int id) {
            if (mk.n >= kMaxMarkers) return;
            Marker& m = mk.m[mk.n++];
            std::memcpy(m.pos, p, sizeof(m.pos));
            m.radius = r;
            m.kind = k;
            m.state = s;
            m.id = static_cast<int8_t>(id);
        };
        const bool near_only = g_zones_near.load(std::memory_order_relaxed) && !placing;
        const float near_d = g_near_dist.load(std::memory_order_relaxed);
        for (int z = 0; z < kZones && zones_shown; ++z) {  // the zones first (the dots are dropped first when layers run short)
            if (!zok[z]) continue;
            MarkerState ms = kIdle;
            for (int h = 0; h < 2; ++h) {
                if (g_consumed[h].load(std::memory_order_relaxed) && press_zone[h] == z)
                    ms = kHeld;
                else if (in_z[h] == z && ms == kIdle)
                    ms = kHandIn;
            }
            if (near_only && ms == kIdle) {  // [Holsters] ZonesNear: shown faint while a hand is near, else not at all
                bool close_by = false;
                for (int jh = 0; jh < 2; ++jh) {
                    if (!bp.hand_ok[jh]) continue;
                    const float dx = gb[jh][0] - zp[z][0], dy = gb[jh][1] - zp[z][1], dz = gb[jh][2] - zp[z][2];
                    close_by = close_by || std::sqrt(dx * dx + dy * dy + dz * dz) < zones[z].radius + near_d;
                }
                if (!close_by) continue;
                ms = kNear;
            }
            add(zp[z], zones[z].radius, kRing, ms, z);
        }
        if (points_shown && long_gun && reload::two_handed() && bp.hand_ok[0] && bp.hand_ok[1]) {
            const MarkerState fs = two_hold[off_h] ? kHeld : fore_ok ? kHandIn : kGunIdle;
            if (bp.fore_ok) {
                add(fore_zone, fore_r, kRing, fs, kZones);  // the foregrip ring: where the front hand engages, its radius
            } else {
                // before the game's grip is known: the barrel line's point nearest the front hand, within the span
                // that engages (0.12 - 0.85 m ahead of the gun hand's wrist), as wide as the distance that does
                const float al = fore_along < 0.12f ? 0.12f : fore_along > 0.85f ? 0.85f : fore_along;
                float c[3];
                for (int k = 0; k < 3; ++k) c[k] = bp.hand[gj][k] + u[k] * al;
                add(c, 0.18f, kRing, fs, kZones);
            }
        }
        if (points_shown && round_held[off_h] && round_mag[off_h]) {  // ([Reload] PistolMagazines) where the magazine goes in
            add(mag_pt, kMagRing, kRing, kGunIdle, kZones + 1);
        } else if (points_shown && round_held[off_h]) {  // where the round goes in (DualWieldCrossReload: the gun it is for)
            add(round_tgt[off_h] == 1 ? load_sec : load_pt, round_tgt[off_h] == 1 ? lr_sec : lr, kRing, kGunIdle, kZones + 1);
        } else if (points_shown && round_held[gun_h] && round_tgt[gun_h] == 1) {
            add(load_sec, lr_sec, kRing, kGunIdle, kZones + 1);
        }
        // the action's want: the revolver open (where rounds go in), the bolt's zone, the lever or pump gun
        // the fanning zone: bright while a sweep would cock it, the hand's colour in it, faint while the hand must leave it
        // first, the gun's while the hammer is back (its own ring then, not the hint's 5 cm one)
        // (the review: the chain's head, so no hint ring of the same id beside it)
        if (fan_shown)
            add(fan_pos, fan_r, kRing, fan_st == 0 ? kHeld : fan_st == 1 ? kHandIn : fan_st == 2 ? kNear : kGunIdle, kZones + 4);
        else if (points_shown && ahint == 2) add(hint_pos, 0.05f, kRing, kHeld, kZones + 4);
        else if (points_shown && ahint && ahint != 5) add(load_pt, ahint == 1 ? 0.06f : 0.04f, kRing, kHeld, kZones + 4);
        else if (points_shown && chint == 2) add(chint_pos, 0.05f, kRing, kHeld, kZones + 4);  // (round 52) the carried bolt
        for (int h = 0; h < 2 && dots_shown; ++h) {
            const int jh = bp.ctrl[0] == h ? 0 : 1;
            if (bp.hand_ok[jh]) add(gb[jh], 0.025f, kDot, in_z[h] >= 0 || (h == off_h && fore_ok) ? kHandIn : kIdle, kZones + 2 + h);
        }
        g_markers_ms = now_ms;
        g_markers_valid = true;
    }
    // [Holsters] InteractionHighlights (2026-10-29, GOAL-weapons-handling.md item 5 and its amendment: "a part glows only
    // while using it would do something now"): each place a ring marks, lit only while a press (or the stroke) there would
    // do something now, by the hand that would do it, full at reach; a zone fades in from ZonesNearDistance past its reach,
    // a gun's place (round 55) is lit at a base level at any distance; none over the sights. Nothing of this runs with the
    // mode rings (the default).
    if (hl_mode != 0 && bp.cam_ok) {
        Highlights hl{};
        HlDiag dg[kHlCount];
        std::memcpy(hl.cam, bp.cam, sizeof(hl.cam));
        const float fade_d = g_near_dist.load(std::memory_order_relaxed);
        const bool deadeye_hl = (st.weapon_flags & RDRVR_WEAPON_DEADEYE) != 0;
        const bool idle_hl = menu::visible() || placing || g_piece_place.load(std::memory_order_relaxed);  // no press acts
        auto dist = [](const float* a, const float* b) {
            const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
            return std::sqrt(x * x + y * y + z * z);
        };
        // near: 1 at reach, else up to 0.65 fading to 0 at fade_d past it
        auto near_t = [&](float d, float reach) { return d <= reach ? 1.0f : 0.65f * std::fmax(0.0f, 1.0f - (d - reach) / fade_d); };
        // (2026-10-30, round 55: "Glow on weapon interaction points only visible when hand is near, should always be
        // visible") a gun's place: kHlGunBase at any distance (no hand known too), full at reach
        constexpr float kHlGunBase = 0.6f;
        auto gun_t = [&](float d, float reach) { return d >= 0.0f && d <= reach ? 1.0f : kHlGunBase; };
        // (round 55: "Glow on holstered items needs to be disabled") parts: the holster zones glow nowhere
        const bool zone_glow = hl_mode != 3;
        auto put = [&](const float* p, float r, uint8_t shape, int id, bool usable, float t, float d, float reach, int h, const char* tag = "") {
            if (hl.n >= kHlCount || id < 0 || id >= kHlCount || dg[id].exists) return;
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !std::isfinite(r) || !(r > 0.0f)) return;  // (never a quad there)
            Highlight& e = hl.h[hl.n++];
            std::memcpy(e.pos, p, sizeof(e.pos));
            e.radius = r;
            e.target = usable ? t : 0.0f;
            e.shape = shape;
            e.id = static_cast<int8_t>(id);
            e.alt_ok = false;
            dg[id].exists = true;
            dg[id].usable = usable;
            dg[id].d = d;
            dg[id].reach = reach;
            dg[id].target = e.target;
            dg[id].hand = h;
            std::memcpy(dg[id].pos, p, sizeof(dg[id].pos));
            dg[id].on = tag;
        };
        // a holster zone: what controller h's press there would do now (the press branches above, in their order, with no
        // side effect); false when it would do nothing
        auto zone_use = [&](int h, int z) -> bool {
            const int jh = bp.ctrl[0] == h ? 0 : 1;
            if (!zok[z] || idle_hl || !bp.hand_ok[jh] || round_held[h] || carry_near(h, jh)) return false;
            const bool sec_hand = ds.on && h == ds.ctrl;
            if (zone_rounds(z) && h == actions::ramrod_taker()) return true;  // (C15, C15h) the ramrod to take
            if (zone_rounds(z)) {  // the chest: a round (a magazine) for a gun that takes one, or the gun pressed to it
                const int xt = cross_tgt(h);
                const int sq = xt >= 0 ? 0 : reload::chest_squeeze();
                // (the copy: dual::load_copy_round's gate, its room, and the spare it loads from: the gun in hand's type's)
                if (sec_hand && sq && ds.copy) return reload::hand_reload() && !deadeye_hl && ds.clip + 0.5f <= ds.clip_max && st.spare >= 1.0f;
                if (sec_hand && sq && !ds.carry) return ds.ready && reload::hand_reload() && !deadeye_hl;
                if (h == gun_h && sq) return gun_loadable();
                if (magg && xt < 0 && h == off_h && !sec_hand) return mag_loadable;
                return xt == 1 ? sec_loadable : xt == 0 ? gun_loadable() : gun_loadable() && h == off_h && !sec_hand;
            }
            const Zone& zz = zones[z];
            if (sec_hand) {  // the second gun's hand: its gun goes back here (once armed)
                bool its = false;
                for (int s : zz.slots) its = its || (s >= 0 && (ds.copy ? s == 1 : s == ds.slot));
                if (!ds.copy && ds.weapon >= 0 && ds.weapon < kWeapons && weapon_fits(zz, ds.weapon, ds.slot)) its = true;
                if (ds.copy && ds.weapon == 15 && (z == sec_from[h] || zz.weapon == 15 || draw_weapon(z) == 15)) its = true;  // (P2)
                return its && sec_armed[h];
            }
            if (dual::copy_enabled() && !ds.on && gun && h == off_h && st.weapon == 15 && (draw_weapon(z) == 15 || zz.weapon == 15))
                return true;  // (P2) the Sawed-off's copy
            if (dual::copy_enabled() && !ds.on && gun && h == off_h && cur == 1 && st.weapon >= 0 && st.weapon <= 7 && draw_weapon(z) < 8)
                for (int s : zz.slots)
                    if (s == 1) return true;  // DualWieldCopy: the copy
            if (dual::enabled() && !ds.on && gun && h == off_h && !holds_slot(z)) {
                bool cond = false;
                for (int s : zz.slots) cond = cond || ((s == 1 || (s >= 4 && s <= 6)) && s != cur && slot_filled(s));
                const int hw = owns(zz.weapon) ? zz.weapon : -1;
                cond = cond || (hw == kSawedOff && st.equip_slot[hw] != cur && slot_filled(st.equip_slot[hw]));
                if (cond) {  // DualWield: a second gun, unless it shares the gun in hand's slot
                    const int dw = draw_weapon(z);
                    const int ws = dw >= 0 && dw < kWeapons ? st.equip_slot[dw] : -1;
                    const bool listed = dw >= 0 && dw < kWeapons && weapon_fits(zz, dw, ws);
                    return !(dw >= 0 && listed && ws == cur);
                }
            }
            const int dw = draw_weapon(z);
            const bool mine = h == gun_h || !controls::draw_any_active();
            int first_slot = -1;
            for (int s : zz.slots)
                if (first_slot < 0 && slot_filled(s)) first_slot = s;
            const bool carried = ds.carry && (dw >= 0 && dw < kWeapons ? st.equip_slot[dw] == ds.slot : first_slot == ds.slot);
            if (carried && !(holds(z) && mine)) return false;
            if (holds(z)) return true;  // put away here, the gun into this hand, or (another hand's) its draw of dw
            return dw >= 0 || first_slot >= 0;    // a draw
        };
        // (the review) a grip held at a zone is "in use" only if its press acted: what a press there would have done in the
        // frame before it (zone_use, the press chain's), latched when the press is first seen; a refused press (a full gun's
        // chest take, a full copy's squeeze, the carried gun's slot, the second gun's hand at another zone) is not
        static bool use_prev[2][kZones] = {};
        static double use_prev_ms = 0.0;
        static int latch_z[2] = {-1, -1};
        static bool latch_use[2] = {false, false};
        // (the block did not run just before: the mode was rings, or no body points) nothing of then is kept, a latch neither
        const bool prev_ok = now_ms - use_prev_ms <= 100.0;
        if (!prev_ok) {
            std::memset(use_prev, 0, sizeof(use_prev));
            latch_z[0] = latch_z[1] = -1;
            latch_use[0] = latch_use[1] = false;
        }
        bool use_now[2][kZones] = {};
        for (int h = 0; h < 2; ++h) {
            const bool gripping = g_consumed[h].load(std::memory_order_relaxed) && press_zone[h] >= 0 && press_zone[h] < kZones;
            if (!gripping) {
                latch_z[h] = -1;
                latch_use[h] = false;
            } else if (latch_z[h] != press_zone[h]) {
                latch_z[h] = press_zone[h];
                // (no frame before it to judge by: this frame's, which fails closed after a press that acted, e.g. a round
                // now held, never open)
                latch_use[h] = prev_ok ? use_prev[h][press_zone[h]] : zone_use(h, press_zone[h]);
            }
        }
        for (int z = 0; z < kZones; ++z) {  // the zones: an area glow
            if (!zok[z]) continue;
            float best = 0.0f, bd = -1.0f;
            int bh = -1;
            bool any = false;
            for (int h = 0; h < 2; ++h) {
                const int jh = bp.ctrl[0] == h ? 0 : 1;
                if (!bp.hand_ok[jh]) continue;
                const float d = dist(gb[jh], zp[z]);
                // gripped there now, the hand still in it (or within the release margin, where a let-go still counts there):
                // full; carried away (a round taken, a gun drawn), the zone is judged as any other
                const bool use = zone_use(h, z);
                use_now[h][z] = use;
                const bool held = !idle_hl && g_consumed[h].load(std::memory_order_relaxed) && press_zone[h] == z && latch_z[h] == z &&
                                  latch_use[h] && (in_z[h] == z || d < zones[z].radius * g_release_margin.load(std::memory_order_relaxed));
                if (!held && !use) continue;
                const float t = held || in_z[h] == z ? 1.0f : std::fmin(0.65f, near_t(d, zones[z].radius));
                any = true;
                if (t > best || bh < 0) best = t, bd = d, bh = h;
            }
            put(zp[z], zones[z].radius, kHlShapeArea, kHlZone + z, any && zone_glow, best, bd, zones[z].radius, bh, zone_glow ? "zone" : "zone (parts: none)");
        }
        std::memcpy(use_prev, use_now, sizeof(use_prev));
        use_prev_ms = now_ms;
        if (!in_hand) hold_take_ms = now_ms;  // (C17, the review) nothing in hand: a put-away over (a redraw by any path lights it)
        const int oj = bp.ctrl[0] == off_h ? 0 : 1;  // John's hand on the off controller (the parts' hand)
        if (long_gun && reload::two_handed() && bp.hand_ok[0] && bp.hand_ok[1]) {  // the foregrip (the pump's fore-end)
            float c[3];
            if (bp.fore_ok) {
                std::memcpy(c, fore_zone, sizeof(c));
            } else {  // (the ring's place before the game's grip is known)
                const float al = fore_along < 0.12f ? 0.12f : fore_along > 0.85f ? 0.85f : fore_along;
                for (int k = 0; k < 3; ++k) c[k] = bp.hand[gj][k] + u[k] * al;
            }
            const float reach = bp.fore_ok ? fore_r : 0.18f, d = dist(gb[fj], c);
            if (ahint == 4) {  // the pump to work: its fore-end, by the front hand
                put(c, 0.05f, kHlShapePart, kHlPump, !idle_hl, two_hold[off_h] ? 1.0f : gun_t(d, reach), d, reach, off_h, bp.fore_ok ? "fore" : "line");
            } else {
                // (2026-11-01, GOAL-offhand-changes C17; round 64, the user: the off hand near the foregrip as the gun is let go and
                // holstered lit the ring there) none while the gun is on its way out: from a hold's put-away until it has left the
                // hand or is taken again (cycle H1: 700 ms was shorter than the game's holstering; 3 s at most)
                // (cycle H2: 5 frames in the let-go's own wait, the grip open and the gun not yet put away: dark then too)
                const bool going_now = (hold_put_ms > hold_take_ms && now_ms - hold_put_ms < 3000.0) || (hold_on && keep[gun_h] == 1 && !grip_down[gun_h]);
                // (C17b, cycle I2: 16 frames still lit after the let-go, c12/c13) kept 0.5 s after it was last so, unless a hold
                // took the gun since: the holstering's frames with nothing in hand ended the put-away (hold_take_ms), the gun
                // still drawn after them
                static double going_last = -1e12;
                if (going_now) going_last = now_ms;
                const bool going = going_now || (now_ms - going_last < 500.0 && hold_take_ms_hold < going_last);
                // (C39; round 66, the user: "Remove the foregrip glow colour": only the part to be worked next coloured) none in
                // parts mode
                const bool usable = !ds.on && !broken && !two_hold[off_h] && !round_held[off_h] && !actions::grip_wanted(off_h) && !idle_hl &&
                                    in_z[off_h] < 0 && !going && g_hl_mode.load(std::memory_order_relaxed) != 3;  // (the press's: a hand in a holster's zone is the holster's)
                // (r298: not the foregrip: a rifle in one hand would have it lit all the time; it fades in as before)
                put(c, 0.05f, kHlShapePart, kHlFore, usable, near_t(d, reach), d, reach, off_h, bp.fore_ok ? "fore" : "line");
                // (2026-11-01, GOAL-offhand-changes C17b; round 65 check 2: the glow still showed; cycle I1, r84 c12/c13: 30 frames
                // after the let-go, the 0.2 s fade out) going: off at once (zone_rings: a target below 0 is no fade)
                if (going && hl.n > 0 && hl.h[hl.n - 1].id == kHlFore) hl.h[hl.n - 1].target = -1.0f;
            }
        }
        // the loading points: a hand holding rounds (a magazine) for a gun that takes them now, or ([Reload] RoundSource=gun)
        // an empty hand that would take them there now
        const int src = g_round_src.load(std::memory_order_relaxed);
        struct Cand {
            bool usable = false;
            float t = 0.0f, d = -1.0f, reach = 0.0f;
            int h = -1;
        } cand[3];  // kHlLoad, kHlMag, kHlLoadSec
        // (r298) always lit only for rounds (a magazine) in hand for it; an empty hand's take there (RoundSource=gun) fades in
        auto offer = [&](int k, bool usable, float d, float reach, int h, bool always = true) {
            Cand& c = cand[k];
            const float t = usable ? (always ? gun_t(d, reach) : near_t(d, reach)) : 0.0f;
            if (c.h < 0 || (usable && !c.usable) || (usable == c.usable && t > c.t)) c = Cand{usable, t, d, reach, h};
        };
        for (int h = 0; h < 2; ++h) {
            const int jh = bp.ctrl[0] == h ? 0 : 1;
            if (!bp.hand_ok[jh] || idle_hl) continue;
            if (round_held[h] && round_mag[h] && mag_pt_ok) {
                offer(1, mag_loadable, dist(gb[jh], mag_pt), kMagRing + 0.08f, h);  // (the grab point sits below the magazine's top)
            } else if (round_held[h] && !round_mag[h]) {
                const bool to_sec = round_tgt[h] == 1;
                if (to_sec ? sec_pt : gun) offer(to_sec ? 2 : 0, to_sec ? sec_loadable : gun_loadable(), dist(gb[jh], to_sec ? load_sec : load_pt), to_sec ? lr_sec : lr, h);
            } else if (src == 1 && !round_held[h]) {
                float ring = 0.0f;
                const float dd = take_dist(h, jh, in_z[h], &ring);  // (-1: this hand takes none here now)
                if (dd >= 0.0f) {
                    const int xt = cross_tgt(h);
                    offer(xt == 1 ? 2 : xt < 0 && magg ? 1 : 0, true, dd, ring + kNearShell, h, false);
                }
            }
        }
        if (gun) {
            const int n0 = hl.n;
            put(load_pt, std::fmin(lr, 0.04f), kHlShapePart, kHlLoad, cand[0].usable, cand[0].t, cand[0].d, cand[0].reach, cand[0].h, "point");
            if (hl.n > n0 && load_alt_ok) {
                std::memcpy(hl.h[hl.n - 1].alt, load_alt, sizeof(load_alt));
                hl.h[hl.n - 1].alt_ok = true;
            }
        }
        if (mag_pt_ok) put(mag_pt, 0.035f, kHlShapePart, kHlMag, cand[1].usable, cand[1].t, cand[1].d, cand[1].reach, cand[1].h, "point");
        if (sec_pt) put(load_sec, std::fmin(lr_sec, 0.04f), kHlShapePart, kHlLoadSec, cand[2].usable, cand[2].t, cand[2].d, cand[2].reach, cand[2].h, "point");
        g_hl_dbg_hint.store(ahint, std::memory_order_relaxed);
        g_hl_dbg_frame.store(bp.gun_frame_ok ? 1 : bp.gun_mframe_ok ? 2 : 0, std::memory_order_relaxed);
        if (ahint != 5) g_hl_dbg_shut.store(0, std::memory_order_relaxed);
        // (2026-11-01, step 7; r80 G4: the Winchester drawn by the left lit no Lever place) a part's point in the drawn gun's
        // frame, or in a long gun's drawn in John's left hand (the transplant: its frame a rotation, the mesh drawn rigidly by
        // it, so the model's point unmirrored; only a learned hand pose is mirrored: the review)
        const bool gframe = bp.gun_frame_ok || bp.gun_mframe_ok;
        auto gun_pt = [&](const float* pm, float* p) {
            const bool m = !bp.gun_frame_ok;
            const float* R = m ? bp.gun_mframe_R : bp.gun_frame_R;
            const float* o = m ? bp.gun_mframe_o : bp.gun_frame_o;
            for (int k = 0; k < 3; ++k) p[k] = o[k] + R[k * 3] * pm[0] + R[k * 3 + 1] * pm[1] + R[k * 3 + 2] * pm[2];
        };
        bool shut_put = false;  // (the review) shut it on its part, else at the loading point below
        if (ahint == 2 && !(mag_pt_ok && dist(hint_pos, mag_pt) < 0.01f)) {  // the action's part (the well's hint is kHlMag's)
            const float d = bp.hand_ok[oj] ? dist(gb[oj], hint_pos) : -1.0f;
            put(hint_pos, 0.035f, kHlShapePart, kHlPart, !idle_hl, gun_t(d, 0.06f), d, 0.06f, off_h, "zone");
        } else if (ahint == 5 && gframe) {
            g_hl_dbg_shut.store(2, std::memory_order_relaxed);
            // (2026-10-31, the off-hand build step 7, R11) shut it: the part that closes (the breech's block, the bolt, a
            // revolver's or a break-open's opening), in the drawn gun's frame, lit as the lever's is
            float pm[3], p[3];
            const char* part_on = body::gun_part_point(st.weapon, actions::kDrvBreech, pm)     ? "breech"
                                  : body::gun_part_point(st.weapon, actions::kDrvBoltLift, pm)  ? "bolt"  // (r313 B6: the slide has no hinge point)
                                  : body::gun_part_point(st.weapon, actions::kDrvBoltSlide, pm) ? "bolt"
                                  : body::gun_part_point(st.weapon, actions::kDrvOpen, pm)      ? "open"
                                                                                               : nullptr;
            if (part_on) {
                gun_pt(pm, p);
                put(p, 0.035f, kHlShapePart, kHlShut, !idle_hl, 1.0f, 0.0f, 0.0f, gun_h, part_on);
                g_hl_dbg_shut.store(1, std::memory_order_relaxed);
                shut_put = true;
            }
        } else if (ahint == 3 && gframe) {
            // a flick works it: on the part the flick works (the lever, else a breech's block, else the hammer: a single action
            // cocked by the stick), in the drawn gun's frame, held by the gun hand. Without the drawn gun's frame or such a part
            // nothing is drawn (never a glow at the loading point after every shot)
            float pm[3], p[3];
            const char* part_on = body::gun_part_point(st.weapon, actions::kDrvLever, pm)    ? "lever"
                             : body::gun_part_point(st.weapon, actions::kDrvBreech, pm) ? "breech"
                             : body::gun_part_point(st.weapon, actions::kDrvHammer, pm) ? "hammer"
                                                                                       : nullptr;
            if (part_on) {
                gun_pt(pm, p);
                put(p, 0.035f, kHlShapePart, kHlLever, !idle_hl, 1.0f, 0.0f, 0.0f, gun_h, part_on);
            }
        }
        if (ahint == 5 && gun && !shut_put) {
            g_hl_dbg_shut.store(3, std::memory_order_relaxed);
            // (r313 B8: a sidearm, or a long gun with no grip pose learned, has no drawn gun's frame; the Buffalo's and the
            // Schofield's shut it lit nothing; the review: nor a frame with no part found) at the loading point: the breech,
            // the cylinder (parts mode glows the part's bones)
            put(load_pt, 0.035f, kHlShapePart, kHlShut, !idle_hl, 1.0f, 0.0f, 0.0f, gun_h, "point");
        }
        // (round 52) the carried gun's want, on the carried gun (drawn as the second: highlight_gun gives these ids role 1
        // this update): its bolt for the free gun hand, as the in-hand bolt's for the off hand; its pump at the carrying
        // hand, lit as the in-hand pump's while held (the jerk works it there)
        // (the review: an id marked only when its put landed: the in-hand gun's place of that id, put first, stays its own)
        if (chint == 2) {
            const int cj = bp.ctrl[0] == gun_h ? 0 : 1;
            const float d = bp.hand_ok[cj] ? dist(gb[cj], chint_pos) : -1.0f;
            const bool free_id = !dg[kHlPart].exists;
            put(chint_pos, 0.035f, kHlShapePart, kHlPart, !idle_hl, gun_t(d, 0.06f), d, 0.06f, gun_h, "carried");
            if (free_id && dg[kHlPart].exists) carried_ids |= 1u << kHlPart;
        } else if (chint == 4 && ds.ctrl >= 0 && ds.ctrl <= 1) {
            const int cj = bp.ctrl[0] == ds.ctrl ? 0 : 1;
            const bool free_id = !dg[kHlPump].exists;
            if (bp.hand_ok[cj]) put(gb[cj], 0.05f, kHlShapePart, kHlPump, !idle_hl, 1.0f, 0.0f, 0.0f, ds.ctrl, "carried");
            if (free_id && dg[kHlPump].exists) carried_ids |= 1u << kHlPump;
        }
        // never over the sights: a glow within 3 degrees of the barrel's line from the eyes, plus a part glow's own angular
        // size (capped at 10 degrees; an area glow's is not added: a hip zone's would black out about 27 degrees)
        if (gun) {
            const float* eye = bp.cam + 12;
            for (int i = 0; i < hl.n; ++i) {
                Highlight& e = hl.h[i];
                const float v[3] = {e.pos[0] - eye[0], e.pos[1] - eye[1], e.pos[2] - eye[2]};
                const float L = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (L < 1e-3f || e.target <= 0.0f) continue;
                const float c = std::fmax(-1.0f, std::fmin(1.0f, (v[0] * u[0] + v[1] * u[1] + v[2] * u[2]) / L));
                const float own = e.shape == kHlShapePart ? std::fmin(std::atan(2.0f * e.radius / L), 10.0f * 0.0174532925f) : 0.0f;
                if (std::acos(c) < own + 3.0f * 0.0174532925f) {
                    e.target = 0.0f;
                    dg[e.id].sights = true;
                    dg[e.id].target = 0.0f;
                }
            }
        }
        g_hl = hl;
        for (int i = 0; i < kHlCount; ++i) g_hl_diag[i] = dg[i];
        g_hl_ms = now_ms;
        g_hl_valid = true;
    }
    // (round 52; the review: once, after the highlights) the places on the carried gun, and a carry with no gun in hand
    g_hl_carried.store(carried_ids, std::memory_order_relaxed);
    g_hl_carry_free.store(ds.carry && !gun, std::memory_order_relaxed);
}

Zone g_zone_defaults[kZones];  // the table as built in (before any ini): a ring's default without a shipped key
bool g_zone_defaults_ok = false;
void load_zones() {
    std::lock_guard lock(g_zone_mutex);
    if (!g_zone_defaults_ok) {
        for (int i = 0; i < kZones; ++i) g_zone_defaults[i] = g_zones[i];
        g_zone_defaults_ok = true;
    }
    for (Zone& z : g_zones) {
        if (z.mirror && z.partner >= 0) {  // a twin: the partner's offset and size mirrored (the partner is loaded first)
            const Zone& p = g_zones[z.partner];
            z.off[0] = -p.off[0];
            z.off[1] = p.off[1];
            z.off[2] = p.off[2];
            z.radius = p.radius;
            z.gun_turn[0] = p.gun_turn[0];  // the shown gun's: the turn and the lean reversed, the offset's right
            z.gun_turn[1] = -p.gun_turn[1];
            z.gun_turn[2] = -p.gun_turn[2];
            z.gun_off[0] = -p.gun_off[0];
            z.gun_off[1] = p.gun_off[1];
            z.gun_off[2] = p.gun_off[2];
        }
        std::string o = config::get_string("Holsters", (std::string(z.key) + "Offset").c_str(), "");
        float v[3];
        if (!o.empty() && sscanf_s(o.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3) std::memcpy(z.off, v, sizeof(v));
        if (z.mirror && z.bone == 0 && z.partner >= 0) {  // LeftHipRingOffset: unset, the right hip's mirrored (as the copy is)
            const Zone& p = g_zones[z.partner];
            z.off_copy[0] = -p.off[0];
            z.off_copy[1] = p.off[1];
            z.off_copy[2] = p.off[2];
            o = config::get_string("Holsters", (std::string(z.key) + "RingOffset").c_str(), "");
            if (!o.empty() && sscanf_s(o.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
                for (int k = 0; k < 3; ++k) z.off_copy[k] = std::isfinite(v[k]) ? std::fmin(0.8f, std::fmax(-0.8f, v[k])) : 0.0f;
        }
        o = config::get_string("Holsters", (std::string(z.key) + "GunTurn").c_str(), "");
        if (!o.empty() && sscanf_s(o.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int k = 0; k < 3; ++k) z.gun_turn[k] = std::isfinite(v[k]) ? std::fmin(180.0f, std::fmax(-180.0f, v[k])) : 0.0f;
        o = config::get_string("Holsters", (std::string(z.key) + "GunOffset").c_str(), "");
        if (!o.empty() && sscanf_s(o.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int k = 0; k < 3; ++k) z.gun_off[k] = std::isfinite(v[k]) ? std::fmin(0.3f, std::fmax(-0.3f, v[k])) : 0.0f;
        z.radius = config::get_float("Holsters", (std::string(z.key) + "Radius").c_str(), z.radius);
        if (!std::strcmp(z.key, "Ammo")) {  // run 10 item 10: the chest's choice (automatic: the rounds), used with AnyWeapon on
            z.weapon = weapon_from_token(config::get_string("Holsters", "AmmoWeapon", "auto"));
        } else if (z.slots[0] >= 0) {
            // the lower back takes the old knife holster's choice unless it has its own
            z.weapon = weapon_from_token(config::get_string("Holsters", (std::string(z.key) + "Weapon").c_str(), "auto"));
            z.enabled = config::get_bool("Holsters", (std::string(z.key) + "Enabled").c_str(), z.enabled);
        }
    }
}

void save_zone(int i) {
    Zone z;
    {
        std::lock_guard lock(g_zone_mutex);
        z = g_zones[i];
    }
    char b[64];
    // (2026-10-13: the LeftHip on the mirrored left holster, its own offset unset, is the right hip's mirrored: kept
    // where it is before the right one moves, or the settings' reload moved it with it)
    for (const Zone& t : g_zones)
        if (t.mirror && t.bone == 0 && t.partner == i && zone_on_copy(t) &&
            config::get_string("Holsters", (std::string(t.key) + "RingOffset").c_str(), "").empty()) {
            float tc[3];
            {
                std::lock_guard lock(g_zone_mutex);
                std::memcpy(tc, t.off_copy, sizeof(tc));
            }
            std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", tc[0], tc[1], tc[2]);
            config::set("Holsters", (std::string(t.key) + "RingOffset").c_str(), b);
        }
    const float* o = zone_off(z);
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", o[0], o[1], o[2]);
    config::set("Holsters", (std::string(z.key) + (zone_on_copy(z) ? "RingOffset" : "Offset")).c_str(), b);
    std::snprintf(b, sizeof(b), "%.3f", z.radius);
    config::set("Holsters", (std::string(z.key) + "Radius").c_str(), b);
}
// 2026-10-13 (the user: "Can you add an option to reset the belt to game default positions?"): every ring's place and
// size back to the shipped RDRVR.ini's (else the built-in table's; a left twin without its own, its partner's mirrored),
// written to the user ini as the menu writes them (the LeftHip's on the mirrored left holster too)
void reset_rings_now() {
    auto shipped3 = [](const std::string& key, float* v) {
        const std::string o = config::get_shipped("Holsters", key.c_str(), "");
        float t[3];
        if (o.empty() || sscanf_s(o.c_str(), "%f %f %f", &t[0], &t[1], &t[2]) != 3) return false;
        std::memcpy(v, t, sizeof(t));
        return true;
    };
    float off[kZones][3], copy[kZones][3], rad[kZones];
    for (int i = 0; i < kZones; ++i) {
        const Zone& d = g_zone_defaults_ok ? g_zone_defaults[i] : g_zones[i];
        std::memcpy(off[i], d.off, sizeof(off[i]));
        if (!shipped3(std::string(d.key) + "Offset", off[i]) && d.mirror && d.partner >= 0 && d.partner < i) {
            off[i][0] = -off[d.partner][0];
            off[i][1] = off[d.partner][1];
            off[i][2] = off[d.partner][2];
        }
        const std::string r = config::get_shipped("Holsters", (std::string(d.key) + "Radius").c_str(), "");
        rad[i] = r.empty() ? (d.mirror && d.partner >= 0 && d.partner < i ? rad[d.partner] : d.radius) : static_cast<float>(std::atof(r.c_str()));
        std::memcpy(copy[i], off[i], sizeof(copy[i]));
        if (d.mirror && d.bone == 0 && d.partner >= 0 && d.partner < i && !shipped3(std::string(d.key) + "RingOffset", copy[i])) {
            copy[i][0] = -off[d.partner][0];  // (on the copy: the right hip's mirrored, as an unset LeftHipRingOffset)
            copy[i][1] = off[d.partner][1];
            copy[i][2] = off[d.partner][2];
        }
    }
    {
        std::lock_guard lock(g_zone_mutex);
        for (int i = 0; i < kZones; ++i) {
            std::memcpy(g_zones[i].off, off[i], sizeof(off[i]));
            std::memcpy(g_zones[i].off_copy, copy[i], sizeof(copy[i]));
            g_zones[i].radius = rad[i];
        }
    }
    char b[64];
    for (int i = 0; i < kZones; ++i) {
        const std::string k = g_zones[i].key;
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[i][0], off[i][1], off[i][2]);
        config::set("Holsters", (k + "Offset").c_str(), b);
        if (g_zones[i].mirror && g_zones[i].bone == 0) {
            std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", copy[i][0], copy[i][1], copy[i][2]);
            config::set("Holsters", (k + "RingOffset").c_str(), b);
        }
        std::snprintf(b, sizeof(b), "%.3f", rad[i]);
        config::set("Holsters", (k + "Radius").c_str(), b);
    }
    log::info("[holster] every ring back to its default place and size");
}
void save_zone_gun(int i) {
    Zone z;
    {
        std::lock_guard lock(g_zone_mutex);
        z = g_zones[i];
    }
    char b[64];
    std::snprintf(b, sizeof(b), "%.1f %.1f %.1f", z.gun_turn[0], z.gun_turn[1], z.gun_turn[2]);
    config::set("Holsters", (std::string(z.key) + "GunTurn").c_str(), b);
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", z.gun_off[0], z.gun_off[1], z.gun_off[2]);
    config::set("Holsters", (std::string(z.key) + "GunOffset").c_str(), b);
}

// ---- trace ("holster trace"): the player's transitions and weapon ops, to the log
std::atomic<bool> g_trace{false};
std::atomic<int> g_trace_n{0};

uintptr_t rva_of(uintptr_t a) {
    const uintptr_t base = anchors::base();
    return a >= base && a < base + 0x4000000 ? a - base + 0x140000000 : a;
}

uintptr_t call_get(uintptr_t fn, uintptr_t obj) {
    __try {
        return reinterpret_cast<uintptr_t (*)(uintptr_t)>(fn)(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void log_trans(const char* what, uintptr_t op, uintptr_t node, float t, const char* note) {
    uintptr_t vt = 0, def = 0, tgtdef = 0, tgt = 0;
    float start = 0, timeout = 0;
    uint32_t prio = 0;
    uint8_t trig = 0;
    rd(op, &vt);
    rd(op + 0x30, &def);
    rd(op + 0x18, &tgt);
    rd(op + 0x24, &trig);
    if (def) {
        rd(def + 8, &start);
        rd(def + 0x18, &tgtdef);
        rd(def + 0x20, &timeout);
        rd(def + 0x28, &prio);
    }
    char conds[300] = {};
    int k = 0;
    uintptr_t dvt = 0, getter = 0;
    const uintptr_t base = anchors::base();
    if (def && rd(def, &dvt) && rd(dvt + 0x30, &getter) && getter >= base && getter < base + 0x4000000) {
        uintptr_t list = call_get(getter, def), arr = 0;
        uint16_t n = 0;
        if (list && rd(list, &arr) && rd(list + 8, &n)) {
            k += std::snprintf(conds + k, sizeof(conds) - k, "%u:", n);
            for (uint16_t i = 0; i < n && i < 6 && k < static_cast<int>(sizeof(conds)) - 32; ++i) {
                uintptr_t c = 0, cvt = 0;
                if (!rd(arr + i * 8u, &c) || !rd(c, &cvt)) break;
                k += std::snprintf(conds + k, sizeof(conds) - k, " %llx", static_cast<unsigned long long>(rva_of(cvt)));
            }
        }
    }
    log::info("[holster] %s node %llx t=%.2f%s def %llx start %.2f prio %u tgtdef %llx tgt %llx timeout %.2f trig %u "
              "aim-node %d | conds %s%s",
              what, static_cast<unsigned long long>(node), t, "", static_cast<unsigned long long>(def), start, prio & 0xffff,
              static_cast<unsigned long long>(tgtdef), static_cast<unsigned long long>(tgt), timeout, trig, has_aim_op(node) ? 1 : 0,
              conds, note);
    (void)vt;
}

// The transition op (vtables 0x141ccfa80, 0x141ccfda0), FUN_140599d50: each update it evaluates its definition's
// condition list (def +0x30, def vtable +0x30 the list {conditions*, u16 count}); once it passes it takes the target
// (op +0x18) and sets op +0x24, waits until op +0x20, and ends; its exit (FUN_140599e40) sets the node's +0xe1 and
// queues the target with def +0x28's priority, and the node ends (FUN_14058e3a0) whatever its other ops say.
// def +8 is the op's start time in the node (FUN_14058ddd0 starts each op when the node's time reaches it).
// (The holster transitions test ConditionNOT + ConditionWeaponDrawn: they follow the weapon controller's put-away,
// they do not decide it.) Traced only.
using TransOp_t = uint64_t (*)(uintptr_t op);
TransOp_t o_tupdate = nullptr, o_texit = nullptr;

uint64_t hk_tupdate(uintptr_t op) {
    if (!g_trace.load(std::memory_order_relaxed)) return o_tupdate(op);
    uint8_t trig0 = 0, trig1 = 0;
    rd(op + 0x24, &trig0);
    uint64_t r = o_tupdate(op);
    uintptr_t node = 0;
    float t = 0;
    if (trig0 || !rd(op + 0x24, &trig1) || !trig1 || !player_op(op, &node, &t)) return r;
    g_player_triggers.fetch_add(1, std::memory_order_relaxed);
    if (g_trace_n.fetch_add(1) < 200) log_trans("triggered", op, node, t, "");
    return r;
}

uint64_t hk_texit(uintptr_t op) {
    if (g_trace.load(std::memory_order_relaxed)) {
        uintptr_t node = 0;
        float t = 0;
        uint8_t trig = 0;
        if (rd(op + 0x24, &trig) && trig && player_op(op, &node, &t) && g_trace_n.fetch_add(1) < 200) log_trans("exit", op, node, t, "");
    }
    return o_texit(op);
}

// The weapon op (vtable 0x141d4be40, instant): its start, FUN_140d46e20, draws (def +0x2c == 0) or holsters
// (def +0x2c != 0) the gun. Traced only.
using WeaponOp_t = uint64_t (*)(uintptr_t op);
WeaponOp_t o_weapon_start = nullptr;

uint64_t hk_weapon_start(uintptr_t op) {
    if (g_trace.load(std::memory_order_relaxed)) {
        uintptr_t node = 0, def = 0;
        float t = 0, start = 0;
        uint8_t holster = 0;
        if (player_op(op, &node, &t) && rd(op + 0x20, &def) && rd(def + 8, &start) && rd(def + 0x2c, &holster) && g_trace_n.fetch_add(1) < 200)
            log::info("[holster] weapon op %s: node %llx t=%.2f def %llx start %.2f gun state %d", holster ? "holster" : "draw",
                      static_cast<unsigned long long>(node), t, static_cast<unsigned long long>(def), start,
                      gun_state(g_player_ped.load(std::memory_order_relaxed)));
    }
    return o_weapon_start(op);
}

// Research: the player's weapon and gun-animation state to a file, for diffing over time ("holster dump <file>").
std::string dump(const std::string& path) {
    uintptr_t actor = player_actor(), ped = 0, anim = 0, gun = 0, wmgr = 0;
    if (!actor) return "ERROR no player actor";
    rd(actor + 0x38, &ped);
    rd(actor + 0x70, &wmgr);
    if (ped) rd(ped + 0xaa8, &anim);
    if (anim) rd(anim + 8, &gun);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") || !f) return "ERROR open " + path;
    struct Region {
        const char* name;
        uintptr_t a;
        uint32_t n;
    } regions[] = {{"actor", actor, 0x400}, {"ped", ped, 0x1000}, {"anim", anim, 0x100}, {"gun", gun, 0x800}, {"wmgr", wmgr, 0x500}};
    static uint8_t buf[0x1000];
    for (const Region& r : regions) {
        std::memset(buf, 0, sizeof(buf));
        for (uint32_t o = 0; r.a && o < r.n; o += 16) raw(r.a + o, buf + o, 16);
        char hdr[16] = {};
        strncpy_s(hdr, r.name, _TRUNCATE);
        std::fwrite(hdr, 1, 16, f);
        uint64_t a = r.a;
        std::fwrite(&a, 8, 1, f);
        std::fwrite(&r.n, 4, 1, f);
        std::fwrite(buf, 1, r.n, f);
    }
    std::fclose(f);
    char b[200];
    std::snprintf(b, sizeof(b), "dumped actor %llx ped %llx gun %llx wmgr %llx", static_cast<unsigned long long>(actor),
                  static_cast<unsigned long long>(ped), static_cast<unsigned long long>(gun), static_cast<unsigned long long>(wmgr));
    return b;
}

}  // namespace

void init() {
    g_keep = config::get_bool("Hands", "KeepGunDrawn", true);
    g_holsters = config::get_bool("Holsters", "Enabled", true);
    g_instant = config::get_bool("Holsters", "InstantDraw", true);
    g_grip_on = config::get_float("Controls", "GripThreshold", 0.6f);
    load_zones();
    log::info("[holster] keep the gun drawn %d, body holsters %d (instant draw %d, grip at %.2f)", g_keep.load() ? 1 : 0,
              g_holsters.load() ? 1 : 0, g_instant.load() ? 1 : 0, g_grip_on);
    // the player's ped for the transition hook (a frame-end listener: off the game's hot paths)
    g_unarmed = config::get_bool("Holsters", "UnarmedAfterHolster", true);
    g_hold = config::get_bool("Holsters", "HoldToKeep", false);  // (2026-10-30)
    g_hold_items = config::get_bool("Holsters", "HoldItems", true);  // (2026-10-31)
    g_instant_put = config::get_bool("Holsters", "InstantPutAway", false);  // (2026-10-31; off 2026-11-03: it steps John)
    g_handover_letgo = std::clamp(static_cast<double>(config::get_float("Holsters", "HandOverLetGoMs", 40.0f)), 20.0, 1000.0);
    g_carry_rounds = config::get_bool("Reload", "CarriedRounds", true);
    g_torch_swap = config::get_bool("Hands", "TorchSwap", true);
    g_knife_swap = config::get_bool("Hands", "KnifeSwap", true);
    if (g_hold.load()) log::info("[holster] hold the grip to keep the gun (HoldToKeep): on");
    g_raise_pmin = config::get_float("Hands", "RaisedPitchMin", -35.0f);
    g_raise_pmax = config::get_float("Hands", "RaisedPitchMax", 60.0f);
    g_raise_reach = config::get_float("Hands", "RaisedReach", 0.25f);
    g_show_zones = config::get_bool("Holsters", "ShowZones", false);
    g_zones_near = config::get_bool("Holsters", "ZonesNear", false);
    {  // [Holsters] InteractionHighlights (2026-10-29): rings (the default), highlights, both; (run 29 item 2) parts
        const std::string m = config::get_string("Holsters", "InteractionHighlights", "rings");
        g_hl_mode = m == "highlights" ? 1 : m == "both" ? 2 : m == "parts" ? 3 : 0;
        float c[3] = {1.0f, 0.84f, 0.52f};
        const std::string cs = config::get_string("Holsters", "PartGlowColor", "1.0 0.84 0.52");
        float r = 0, g = 0, b = 0;
        if (sscanf_s(cs.c_str(), "%f %f %f", &r, &g, &b) == 3 && std::isfinite(r) && std::isfinite(g) && std::isfinite(b)) c[0] = r, c[1] = g, c[2] = b;
        set_part_glow_color(c, false);
        set_part_glow_strength(config::get_float("Holsters", "PartGlowStrength", 1.0f), false);
        pso::glow::want(g_hl_mode.load() == 3);  // (the weapon pipelines' glow twins made at their creation from now)
    }
    g_near_dist = std::fmin(1.0f, std::fmax(0.05f, config::get_float("Holsters", "ZonesNearDistance", 0.25f)));
    g_steady_zones = config::get_bool("Holsters", "SteadyZones", true);  // (2026-10-13: on by default, its menu row gone)
    g_show_dots = config::get_bool("Holsters", "ShowHandDots", g_show_zones.load());
    g_ring_gun_axes = config::get_bool("Reload", "RingInGunAxes", true);
    g_steady_ring = config::get_bool("Reload", "SteadyRing", true);
    g_show_points = config::get_bool("Holsters", "ShowWeaponPoints", g_show_zones.load());
    g_release_margin = config::get_float("Holsters", "ReleaseMargin", 1.3f);
    g_weapon_choice = config::get_bool("Holsters", "WeaponChoice", true);
    g_any_weapon = config::get_bool("Holsters", "AnyWeapon", false);
    g_wheel_hol = config::get_bool("Holsters", "WheelAtHolster", false);  // (round 143)
    {
        char b[200];
        int o = std::snprintf(b, sizeof(b), "[holster] weapons: choice %d, any %d:", g_weapon_choice.load() ? 1 : 0, g_any_weapon.load() ? 1 : 0);
        for (int z = 0; z < kZones && o > 0 && o < static_cast<int>(sizeof(b)); ++z)
            if (g_zones[z].slots[0] >= 0)
                o += std::snprintf(b + o, sizeof(b) - o, " %s=%s", g_zones[z].key, token_of(g_zones[z].weapon));
        log::info("%s", b);
    }
    {
        float off[3] = {0, 0, 0};
        for (int w = 0; w < kAdjGuns; ++w) {  // round 13 item 8: each gun's own adjustments
            const std::string sec = std::string("Weapon.") + kWeaponToken[w];
            for (int what = 0; what < kAdjCount; ++what) {
                for (int hand = 0; hand < 2; ++hand) {  // (C8) the left hand's (...Left), then the right's
                    const std::string k0 = std::string(kAdjKey[what][0]) + (hand == 0 ? "Left" : "");
                    const std::string v = config::get_string(sec.c_str(), k0.c_str(), "");
                    float o[3];
                    if (v.empty() || sscanf_s(v.c_str(), "%f %f %f", &o[0], &o[1], &o[2]) != 3) continue;
                    const std::string k1 = kAdjKey[what][1] ? std::string(kAdjKey[what][1]) + (hand == 0 ? "Left" : "") : std::string();
                    const float r = kAdjKey[what][1] ? config::get_float(sec.c_str(), k1.c_str(), what == kAdjForeRing ? 0.20f : 0.12f) : 0.0f;
                    set_gun_adjust(w, what, o, r, false, hand);
                }
            }
        }
        float go[3] = {0, 0, 0};
        const std::string gs = config::get_string("Hands", "InteractOffset", "0 0 0");
        sscanf_s(gs.c_str(), "%f %f %f", &go[0], &go[1], &go[2]);
        set_interact_offset(go, false);
        const std::string fo = config::get_string("Reload", "ForegripOffset", "0 0 0");
        sscanf_s(fo.c_str(), "%f %f %f", &off[0], &off[1], &off[2]);
        set_foregrip(off, config::get_float("Reload", "ForegripRadius", 0.20f), false);
        float zo[3] = {0, 0, 0};
        const std::string fz = config::get_string("Reload", "ForegripZoneOffset", "0 0 0");
        sscanf_s(fz.c_str(), "%f %f %f", &zo[0], &zo[1], &zo[2]);
        set_foregrip_zone(zo, config::get_float("Reload", "ForegripZoneRadius", g_fore_radius), false);
        log::info("[holster] the foregrip ring: moved (%.3f %.3f %.3f) m from the grip, radius %.2f m", zo[0], zo[1], zo[2], g_fzone_radius);
        g_fore_snap = config::get_bool("Reload", "TwoHandedSnap", true);
        g_stock = config::get_bool("Reload", "PhysicalStock", false);
        g_trig_round = config::get_bool("Reload", "TriggerTakesRound", false);
        {
            const int n = config::get_int("Reload", "RoundsPerGrab", 1);
            g_per_grab = n < 1 ? 1 : n > 8 ? 8 : n;
            g_all_in = config::get_string("Reload", "RoundsGoIn", "one") == "all";
            const float gap = config::get_float("Reload", "RoundInsertMs", 300.0f);
            g_insert_gap_ms = gap >= 150.0f && gap <= 2000.0f ? gap : 300.0f;  // (the game's count follows a round in within 150 ms)
            {
                const std::string rs = config::get_string("Reload", "RoundSource", "chest");
                g_round_src = rs == "gun" ? 1 : rs == "palm" ? 2 : 0;
            }
            if (g_round_src.load()) log::info("[holster] the rounds come from: %s (and the chest)", g_round_src.load() == 1 ? "a grip near the gun" : "the hand turned palm up");
        }
        {
            const std::string s = config::get_string("Reload", "StockTurn", "0 0 0");
            std::lock_guard lock(g_stock_mutex);
            if (sscanf_s(s.c_str(), "%f %f %f", g_stock_turn, g_stock_turn + 1, g_stock_turn + 2) != 3 || !std::isfinite(g_stock_turn[0]) ||
                !std::isfinite(g_stock_turn[1]) || !std::isfinite(g_stock_turn[2]) || std::fabs(g_stock_turn[0]) > 360.0f ||
                std::fabs(g_stock_turn[1]) > 360.0f || std::fabs(g_stock_turn[2]) > 360.0f)
                std::memset(g_stock_turn, 0, sizeof(g_stock_turn));  // (the review: a NaN turn would reach the IK)
        }
        float lo[3] = {0, 0, 0};
        const std::string lp = config::get_string("Reload", "LoadPointOffset", "0 0 0");
        sscanf_s(lp.c_str(), "%f %f %f", &lo[0], &lo[1], &lo[2]);
        set_load_point(lo, config::get_float("Reload", "LoadPointRadius", 0.12f), config::get_bool("Reload", "InsertOnTouch", true), false);
        g_show_guns = config::get_bool("Holsters", "ShowGuns", false);
        held_prop::set_gate(config::get_float("Holsters", "PropGate", 0.30f));  // run 10 item 2
        g_show_back_guns = config::get_bool("Holsters", "ShowBackGuns", true);
        g_anchor = config::get_string("Holsters", "Anchor", "headset") == "body" ? 0 : 1;
        g_turn_head = config::get_bool("Holsters", "TurnWithHead", false);
        g_turn_hands = config::get_bool("Holsters", "TurnByHands", false);
        g_lean_steady = config::get_bool("Holsters", "LeanSteady", false);
        g_rings_follow = config::get_bool("Holsters", "RingsFollowHolsters", true);
        g_torso_len = std::fmin(0.9f, std::fmax(0.3f, config::get_float("Holsters", "TorsoLength", 0.55f)));
        g_show_models = config::get_bool("Holsters", "ShowModels", false);
        g_belt_props = config::get_bool("Holsters", "OwnBeltProps", false);  // (GOAL-cattleman-belts item 2)
        // (GOAL-after-0.8.13.2 item 7) never on (round 115): CREATE_PROP_IN_LAYOUT faults inside the game making player_default
        // (RDR.exe+0x51bb72, every time, in the simulator's cycles since 11-07 too), RedHook catches it and answers with the
        // layout: a T-posed John left in the world, the layout destroyed as the object (the belt's props gone), and the object
        // manager's lock left taken (the game froze at the poker table's exit, a worker waiting on it)
        g_bp_model = false;
        g_bp_throwable = config::get_bool("Holsters", "OwnBeltThrowable", false);  // (round 117)
        if (config::get_bool("Holsters", "OwnBeltPropsDefault", false))
            log::warn("[holster] OwnBeltPropsDefault ignored: the game faults making the default outfit's model as a prop (the game's satchel shown instead)");
        {
            std::lock_guard lock(g_bp_mutex);
            for (int k = 0; k < kBeltProps; ++k) {
                const std::string key = body::piece_key(k + 1);
                float v[3];
                if (std::sscanf(config::get_string("Holsters", ("OwnBeltProp" + key + "Home").c_str(), "").c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
                    for (int i = 0; i < 3; ++i) g_bp_home[k][i] = std::isfinite(v[i]) ? std::fmax(-0.6f, std::fmin(0.6f, v[i])) : g_bp_home[k][i];
                if (std::sscanf(config::get_string("Holsters", ("OwnBeltProp" + key + "Turn").c_str(), "").c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
                    for (int i = 0; i < 3; ++i) g_bp_turn[k][i] = std::isfinite(v[i]) ? v[i] : 0.0f;
                if (std::sscanf(config::get_string("Holsters", ("OwnBeltProp" + key + "Scale").c_str(), "").c_str(), "%f", &v[0]) == 1 && std::isfinite(v[0]))
                    g_bp_scale[k] = std::fmax(0.2f, std::fmin(2.0f, v[0]));
            }
            for (int i = 0; i < kThrowables; ++i) {  // (round 120) each throwable's own place
                float a[12];
                const std::string v = config::get_string("Holsters", (std::string("OwnBeltThrowable") + kThrowableKey[i] + "Pose").c_str(), "");
                if (std::sscanf(v.c_str(), "%f %f %f %f %f %f %f %f %f %f %f %f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8],
                                &a[9], &a[10], &a[11]) != 12)
                    continue;
                if (thr_adj_ok(a)) std::memcpy(g_thr_adj[i], a, sizeof(a));
                else log::warn("[holster] OwnBeltThrowable%sPose not a turn and a move: ignored", kThrowableKey[i]);
            }
        }
        {
            const std::string mn = config::get_string("Holsters", "Model", "p_gen_gunbelt01x");
            std::snprintf(g_model_name, sizeof(g_model_name), "%s", mn.c_str());
            float o[3] = {0, 0, 0}, r[3] = {0, 0, 0};
            sscanf_s(config::get_string("Holsters", "ModelOffset", "0 0 0").c_str(), "%f %f %f", &o[0], &o[1], &o[2]);
            sscanf_s(config::get_string("Holsters", "ModelTurn", "90 90 0").c_str(), "%f %f %f", &r[0], &r[1], &r[2]);
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_model_off, o, sizeof(o));
            std::memcpy(g_model_rot, r, sizeof(r));
        }
        g_turn_dead = std::fmin(90.0f, std::fmax(0.0f, config::get_float("Holsters", "TurnDeadZone", 0.0f)));
        log::info("[holster] the loading point moved (%.3f %.3f %.3f) m, radius %.2f m; a round goes in on touch %d", lo[0], lo[1], lo[2], g_load_radius,
                  g_insert_touch ? 1 : 0);
        log::info("[holster] foregrip: the game's grip moved (%.3f %.3f %.3f) m, radius %.2f m; the front hand snaps on %d", off[0], off[1],
                  off[2], g_fore_radius, g_fore_snap.load() ? 1 : 0);
    }
    log::info("[holster] a put-away selects the fists %d; shown: the holster rings %d, the hand dots %d, the weapon points %d", g_unarmed.load() ? 1 : 0,
              g_show_zones.load() ? 1 : 0, g_show_dots.load() ? 1 : 0, g_show_points.load() ? 1 : 0);
    d3d::add_frame_end_listener([](uint64_t) {
        uintptr_t actor = player_actor(), ped = 0;
        if (actor) rd(actor + 0x38, &ped);
        g_player_ped.store(ped, std::memory_order_relaxed);
        {  // the gun's aim pose (SEH reads, once a frame)
            uintptr_t anim = 0, gun = 0;
            uint8_t fl = 0;
            g_aim_pose.store(ped && rd(ped + 0xaa8, &anim) && anim && rd(anim + 8, &gun) && gun && rd(gun + 0x5d6, &fl) && (fl & 0x40),
                             std::memory_order_relaxed);
            // fire-ready (FUN_140d16400, the player's branch): the aim op up (+0x5d6 & 0xc0), not blocked, the state
            // ready (8/12 settled, 6 raised past 0.8, 11, 1/4 past 0.8, 5 out of cover) and the fire timer done
            bool ready = false;
            uint8_t b5d4 = 0, b5d5 = 0, b5d8 = 0;
            int32_t gs = -1;
            float f3c4 = 0.0f, f368 = 1.0f, f460 = 0.0f, f24 = 1.0f;
            if (gun && (fl & 0xc0) && rd(gun + 0x5d4, &b5d4) && rd(gun + 0x5d5, &b5d5) && rd(gun + 0x5d8, &b5d8) && rd(gun + 0x3bc, &gs) &&
                rd(gun + 0x3c4, &f3c4) && rd(gun + 0x368, &f368) && rd(gun + 0x460, &f460) && rd(gun + 0x24, &f24)) {
                bool cover = false;
                uintptr_t cv = 0, cb = 0;
                int32_t cst = 0;
                uint8_t cbc = 0, b5c = 0, b5e = 0;
                if (rd(anim + 0x18, &cv) && cv) {
                    rd(cv + 0x18, &cst);
                    rd(cv + 0xbc, &cbc);
                    cover = cst != 0 || (cbc & 4);
                }
                if (rd(anim + 0xb8, &cb) && cb) {
                    rd(cb + 0x5c, &b5c);
                    rd(cb + 0x5e, &b5e);
                    cover = cover || b5c || (b5e & 1);
                }
                ready = !(b5d4 & 0x40) && !(b5d5 & 2) &&
                        (((gs == 8 || gs == 12) && f368 <= 0.39f) || (gs == 6 && f3c4 >= 0.8f) || gs == 11 ||
                         ((gs == 1 || gs == 4) && f460 >= 0.8f) || (gs == 5 && !cover)) &&
                        (f24 < 0.0f || (b5d8 & 6));
            }
            g_fire_ready.store(ready, std::memory_order_relaxed);
            float ph = -1.0f;
            if (!(gun && rd(gun + 0x24, &ph)) || !(ph == ph)) ph = -1.0f;  // NaN: none
            g_fire_phase.store(ph, std::memory_order_relaxed);
        }
        holster_frame();
    });
}

bool install() {
    bool ok = hooks::install("RDR player weapon controller (holster)",
                             reinterpret_cast<void*>(anchors::addr(anchors::Id::PlayerWeaponUpdate)), hk_player_weapon, &o_player_weapon);
    // the action-tree trace (the automatic holster's research, ENGINE-NOTES item 0b): every actor's transition ops
    // run through the update each frame, so it is only hooked when asked for
    if (config::get_bool("Debug", "ActionTrace", false)) {
        ok &= hooks::install("RDR transition op update (holster trace)", reinterpret_cast<void*>(anchors::addr(anchors::Id::TransOpUpdate)),
                             hk_tupdate, &o_tupdate);
        ok &= hooks::install("RDR transition op exit (holster trace)", reinterpret_cast<void*>(anchors::addr(anchors::Id::TransOpExit)),
                             hk_texit, &o_texit);
        ok &= hooks::install("RDR weapon op start (holster trace)", reinterpret_cast<void*>(anchors::addr(anchors::Id::WeaponOpStart)),
                             hk_weapon_start, &o_weapon_start);
    }
    return ok;
}

bool keeps_drawn() { return g_keep.load(); }

bool holsters_enabled() { return g_holsters.load(); }
void set_holsters_enabled(bool on) {
    if (g_holsters.exchange(on) != on) log::info("[holster] body holsters: %s", on ? "on" : "off");
    config::set("Holsters", "Enabled", on ? "1" : "0");
}
// (round 139, the review: on a mounted gun in first person the grips are the gun's: not the game's LB and RB, nor the wheel's)
bool grip_consumed(int h) { return h >= 0 && h < 2 && (g_consumed[h].load(std::memory_order_relaxed) || g_turret_on.load(std::memory_order_relaxed)); }
float two_hand_blend() { return g_two_blend.load(std::memory_order_relaxed); }
void foregrip(float off[3], float* radius) {
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(off, g_fore_off, sizeof(g_fore_off));
    *radius = g_fore_radius;
}
void gun_adjust(int w, int what, float off[3], float* radius, bool* own, int hand, bool exact) {
    std::lock_guard lock(g_zone_mutex);
    const bool ok = w >= 0 && w < kAdjGuns && what >= 0 && what < kAdjCount;
    // (C8) the left hand's own, else (not exact) the right's own, else every gun's
    const int hs = ok && hand == 0 && g_gun_adj[w][0].own[what] ? 0 : ok && (hand != 0 || !exact) && g_gun_adj[w][1].own[what] ? 1 : -1;
    const bool mine = hs >= 0;
    if (own) *own = mine && (hs == hand || hand < 0 || !exact);
    if (mine && (!exact || hs == (hand == 0 ? 0 : 1))) {
        std::memcpy(off, g_gun_adj[w][hs].off[what], sizeof(float) * 3);
        if (radius) *radius = what == kAdjForegrip ? g_fore_radius : g_gun_adj[w][hs].radius[what];
        return;
    }
    const float* g = what == kAdjForegrip ? g_fore_off : what == kAdjForeRing ? g_fzone_off : what == kAdjLoadRing ? g_load_off : kAdjNone;
    std::memcpy(off, g, sizeof(float) * 3);
    if (radius) *radius = what == kAdjForegrip ? g_fore_radius : what == kAdjForeRing ? g_fzone_radius : what == kAdjLoadRing ? g_load_radius : 0.0f;
}
void set_gun_adjust(int w, int what, const float off[3], float radius, bool save, int hand, bool keep_other) {
    if (w < 0 || w >= kAdjGuns || what < 0 || what >= kAdjCount) return;
    const int hs = hand == 0 ? 0 : 1;
    const char* const sfx = hs == 0 ? "Left" : "";
    // (2026-11-01, GOAL-offhand-changes C8b; round 65 check 11, the user: moving one hand's foregrip and ring moved the other
    // hand's too) the right hand's set while the left has none of its own: the left keeps where it was (the right's, else
    // every gun's) as its own, so it stays
    bool pin = false;
    float pin_off[3] = {}, pin_r = 0.0f;
    {
        std::lock_guard lock(g_zone_mutex);
        if (keep_other && hs == 1 && !g_gun_adj[w][0].own[what]) {
            GunAdjSet& l = g_gun_adj[w][0];
            const GunAdjSet& r = g_gun_adj[w][1];
            const float* g = r.own[what]            ? r.off[what]
                             : what == kAdjForegrip ? g_fore_off
                             : what == kAdjForeRing ? g_fzone_off
                             : what == kAdjLoadRing ? g_load_off
                                                    : kAdjNone;
            const float gr = r.own[what]            ? r.radius[what]
                             : what == kAdjForegrip ? g_fore_radius
                             : what == kAdjForeRing ? g_fzone_radius
                             : what == kAdjLoadRing ? g_load_radius
                                                    : 0.0f;
            std::memcpy(l.off[what], g, sizeof(l.off[what]));
            l.radius[what] = gr;
            l.own[what] = pin = true;
            std::memcpy(pin_off, g, sizeof(pin_off));
            pin_r = gr;
        }
        GunAdjSet& a = g_gun_adj[w][hs];
        a.own[what] = true;
        for (int k = 0; k < 3; ++k) a.off[what][k] = off[k] < -0.4f ? -0.4f : off[k] > 0.4f ? 0.4f : off[k];
        a.radius[what] = radius < 0.03f ? 0.03f : radius > 0.5f ? 0.5f : radius;
    }
    if (save && pin) {
        const std::string sec = std::string("Weapon.") + kWeaponToken[w];
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", pin_off[0], pin_off[1], pin_off[2]);
        config::set(sec.c_str(), (std::string(kAdjKey[what][0]) + "Left").c_str(), b);
        if (kAdjKey[what][1]) {
            std::snprintf(b, sizeof(b), "%.3f", pin_r);
            config::set(sec.c_str(), (std::string(kAdjKey[what][1]) + "Left").c_str(), b);
        }
    }
    if (pin)
        log::info("[holster] %s: the left hand's %s kept where it was (%.3f %.3f %.3f) m, its own now", kWeaponLabel[w], kAdjName[what], pin_off[0],
                  pin_off[1], pin_off[2]);
    if (save) {
        const std::string sec = std::string("Weapon.") + kWeaponToken[w];
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[0], off[1], off[2]);
        config::set(sec.c_str(), (std::string(kAdjKey[what][0]) + sfx).c_str(), b);
        if (kAdjKey[what][1]) {
            std::snprintf(b, sizeof(b), "%.3f", radius);
            config::set(sec.c_str(), (std::string(kAdjKey[what][1]) + sfx).c_str(), b);
        }
    }
    log::info("[holster] %s: its own %s for the %s hand (%.3f %.3f %.3f) m%s", kWeaponLabel[w], kAdjName[what], hs == 0 ? "left" : "right", off[0], off[1],
              off[2], save ? ", saved" : "");
}
void clear_gun_adjust(int w, int what, int hand, bool keep_other) {
    if (w < 0 || w >= kAdjGuns || what < 0 || what >= kAdjCount) return;
    const int hs = hand == 0 ? 0 : 1;
    const char* const sfx = hs == 0 ? "Left" : "";
    if (keep_other && hs == 1) {  // (C8b, the review) the left with none of its own keeps the right's (which it used) as its own
        bool lown = false;
        float lo[3], lr = 0.0f;
        gun_adjust(w, what, lo, &lr, &lown, 0, true);
        if (!lown) {
            gun_adjust(w, what, lo, &lr, nullptr, 1);
            set_gun_adjust(w, what, lo, lr, true, 0);
        }
    }
    {
        std::lock_guard lock(g_zone_mutex);
        g_gun_adj[w][hs].own[what] = false;
    }
    const std::string sec = std::string("Weapon.") + kWeaponToken[w];
    config::set(sec.c_str(), (std::string(kAdjKey[what][0]) + sfx).c_str(), "");
    if (kAdjKey[what][1]) config::set(sec.c_str(), (std::string(kAdjKey[what][1]) + sfx).c_str(), "");
    log::info("[holster] %s: its %s for the %s hand back to %s", kWeaponLabel[w], kAdjName[what], hs == 0 ? "left" : "right",
              hs == 0 ? "the right hand's" : "every gun's");
}
int gun_in_hand() { return g_gun_in_hand.load(std::memory_order_relaxed); }
void interact_offset(float off[3]) {
    for (int k = 0; k < 3; ++k) off[k] = g_grab_off[k].load(std::memory_order_relaxed);
}
void set_interact_offset(const float off[3], bool save) {
    for (int k = 0; k < 3; ++k) g_grab_off[k].store(off[k] < -0.25f ? -0.25f : off[k] > 0.25f ? 0.25f : off[k], std::memory_order_relaxed);
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", g_grab_off[0].load(), g_grab_off[1].load(), g_grab_off[2].load());
        config::set("Hands", "InteractOffset", b);
    }
}
void grab_point(const body::BodyPoints& bp, int j, float out[3]) {
    // the target's axes (columns): x right (the left hand's mirrored), y up, z back (the fingers along -z)
    const float o[3] = {(j == 0 ? -1.0f : 1.0f) * g_grab_off[0].load(std::memory_order_relaxed), g_grab_off[1].load(std::memory_order_relaxed),
                        -g_grab_off[2].load(std::memory_order_relaxed)};
    const float* R = bp.target_rot[j];
    for (int k = 0; k < 3; ++k) out[k] = bp.hand[j][k] + R[k * 3] * o[0] + R[k * 3 + 1] * o[1] + R[k * 3 + 2] * o[2];
}
void set_foregrip(const float off[3], float radius, bool save) {
    {
        std::lock_guard lock(g_zone_mutex);
        for (int k = 0; k < 3; ++k) g_fore_off[k] = off[k] < -0.5f ? -0.5f : off[k] > 0.5f ? 0.5f : off[k];
        g_fore_radius = radius < 0.03f ? 0.03f : radius > 0.5f ? 0.5f : radius;
    }
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[0], off[1], off[2]);
        config::set("Reload", "ForegripOffset", b);
        std::snprintf(b, sizeof(b), "%.3f", radius);
        config::set("Reload", "ForegripRadius", b);
    }
}
void load_point(float off[3], float* radius, bool* touch) {
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(off, g_load_off, sizeof(g_load_off));
    *radius = g_load_radius;
    *touch = g_insert_touch;
}
void set_load_point(const float off[3], float radius, bool touch, bool save) {
    {
        std::lock_guard lock(g_zone_mutex);
        for (int k = 0; k < 3; ++k) g_load_off[k] = off[k] < -0.3f ? -0.3f : off[k] > 0.3f ? 0.3f : off[k];
        g_load_radius = radius < 0.03f ? 0.03f : radius > 0.4f ? 0.4f : radius;
        g_insert_touch = touch;
    }
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[0], off[1], off[2]);
        config::set("Reload", "LoadPointOffset", b);
        std::snprintf(b, sizeof(b), "%.3f", radius);
        config::set("Reload", "LoadPointRadius", b);
        config::set("Reload", "InsertOnTouch", touch ? "1" : "0");
    }
}
void foregrip_zone(float off[3], float* radius) {
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(off, g_fzone_off, sizeof(g_fzone_off));
    *radius = g_fzone_radius;
}
void set_foregrip_zone(const float off[3], float radius, bool save) {
    {
        std::lock_guard lock(g_zone_mutex);
        for (int k = 0; k < 3; ++k) g_fzone_off[k] = off[k] < -0.5f ? -0.5f : off[k] > 0.5f ? 0.5f : off[k];
        g_fzone_radius = radius < 0.03f ? 0.03f : radius > 0.5f ? 0.5f : radius;
    }
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[0], off[1], off[2]);
        config::set("Reload", "ForegripZoneOffset", b);
        std::snprintf(b, sizeof(b), "%.3f", radius);
        config::set("Reload", "ForegripZoneRadius", b);
    }
}
bool foregrip_snap() { return g_fore_snap.load(); }
bool physical_stock() { return g_stock.load(std::memory_order_relaxed); }
bool trigger_takes_round() { return g_trig_round.load(std::memory_order_relaxed); }
void set_trigger_takes_round(bool on) {
    if (g_trig_round.exchange(on) != on) log::info("[holster] the trigger takes a round at the chest: %s", on ? "on" : "off");
    config::set("Reload", "TriggerTakesRound", on ? "1" : "0");
}
bool trigger_for_round(int h) { return h >= 0 && h < 2 && g_trig_for_round[h].load(std::memory_order_relaxed); }
bool cross_holds_fire(int h) { return h >= 0 && h < 2 && g_cross_fire_hold[h].load(std::memory_order_relaxed); }
int rounds_per_grab() { return g_per_grab.load(std::memory_order_relaxed); }
void set_rounds_per_grab(int n) {
    n = n < 1 ? 1 : n > 8 ? 8 : n;
    if (g_per_grab.exchange(n) != n) log::info("[holster] rounds taken at the chest at once: %d", n);
    config::set("Reload", "RoundsPerGrab", std::to_string(n).c_str());
}
int round_source() { return g_round_src.load(std::memory_order_relaxed); }
void set_round_source(int src, bool save) {
    src = src == 1 || src == 2 ? src : 0;
    if (g_round_src.exchange(src) != src)
        log::info("[holster] the rounds come from: %s", src == 1 ? "a grip near the gun (and the chest)" : src == 2 ? "the hand turned palm up (and the chest)" : "the chest");
    if (save) config::set("Reload", "RoundSource", src == 1 ? "gun" : src == 2 ? "palm" : "chest");
}
bool rounds_all_at_once() { return g_all_in.load(std::memory_order_relaxed); }
void set_rounds_all_at_once(bool on) {
    if (g_all_in.exchange(on) != on) log::info("[holster] the rounds held go in: %s", on ? "all at once" : "one at a time");
    config::set("Reload", "RoundsGoIn", on ? "all" : "one");
}
void set_physical_stock(bool on) {
    if (g_stock.exchange(on) != on) log::info("[holster] physical stock: %s", on ? "on" : "off");
    config::set("Reload", "PhysicalStock", on ? "1" : "0");
}
void stock_calibrate(double delay_ms) {
    g_stock_cal_ms = log::now_ms() + delay_ms;
    log::info("[holster] stock calibration in %.0f ms: hold a long gun on the stock with both hands, aiming down the sights", delay_ms);
}
double stock_calibrate_in() {
    const double t = g_stock_cal_ms.load(std::memory_order_relaxed);
    return t > 0.0 ? t - log::now_ms() : -1.0;
}
bool stock_calibrate_due() {
    double t = g_stock_cal_ms.load(std::memory_order_relaxed);
    if (t <= 0.0 || log::now_ms() < t || !g_stock_cal_ms.compare_exchange_strong(t, 0.0)) return false;
    if (log::now_ms() > t + 1000.0) {  // (the review: not sampled in time, e.g. not in first person: dropped, not kept armed)
        log::warn("[holster] stock calibration: not taken within a second of its time (not in first person?), nothing learned");
        return false;
    }
    return true;
}
void stock_calibrate_expire() {  // the body's frame end: a request past its time and a second is dropped (holster.h)
    double t = g_stock_cal_ms.load(std::memory_order_relaxed);
    if (t > 0.0 && log::now_ms() > t + 1000.0 && g_stock_cal_ms.compare_exchange_strong(t, 0.0))
        log::warn("[holster] stock calibration: not taken (not in first person, or no long gun in hand), nothing learned");
}
void stock_turn(float deg[3]) {
    std::lock_guard lock(g_stock_mutex);
    std::memcpy(deg, g_stock_turn, sizeof(g_stock_turn));
}
void set_stock_turn(const float deg[3]) {
    if (!std::isfinite(deg[0]) || !std::isfinite(deg[1]) || !std::isfinite(deg[2])) {
        log::warn("[holster] stock turn not finite: not kept");
        return;
    }
    {
        std::lock_guard lock(g_stock_mutex);
        std::memcpy(g_stock_turn, deg, sizeof(g_stock_turn));
    }
    char b[80];
    std::snprintf(b, sizeof(b), "%.2f %.2f %.2f", deg[0], deg[1], deg[2]);
    config::set("Reload", "StockTurn", b);
}
void set_foregrip_snap(bool on, bool save) {
    if (g_fore_snap.exchange(on) != on) log::info("[holster] the front hand snaps onto the gun %d", on ? 1 : 0);
    if (save) config::set("Reload", "TwoHandedSnap", on ? "1" : "0");
}
const char* weapon_label(int w) { return w >= 0 && w < kWeapons ? kWeaponLabel[w] : ""; }
const char* weapon_token(int w) { return w >= 0 && w < kWeapons ? kWeaponToken[w] : ""; }
bool turret_ready() { return turret_ready_now(); }  // [Hands] MountedGunFirstPerson: the mounted gun read within 0.5 s
bool turret_hand_free(int h) {  // (round 141; any thread) h: the controller (0 left, 1 right)
    return h >= 0 && h < 2 && g_turret_on.load(std::memory_order_relaxed) && g_turret_arms_free.load(std::memory_order_relaxed) &&
           !g_turret_gun_axis.load(std::memory_order_relaxed) && !g_turret_held[h].load(std::memory_order_relaxed);
}
bool weapon_choice() { return g_weapon_choice.load(); }
void set_weapon_choice(bool on) {
    g_weapon_choice = on;
    config::set("Holsters", "WeaponChoice", on ? "1" : "0");
}
bool any_weapon() { return g_any_weapon.load(); }
void set_any_weapon(bool on) {
    g_any_weapon = on;
    config::set("Holsters", "AnyWeapon", on ? "1" : "0");
}
int zone_weapon(int z) {
    if (z < 0 || z >= kZones) return -1;
    std::lock_guard lock(g_zone_mutex);
    return g_zones[z].weapon;
}
void set_zone_weapon(int z, int w, bool save) {
    if (z < 0 || z >= kZones || !zone_takes_weapons(z)) return;
    if (w < -1 || w >= kRounds) w = -1;  // (2026-10-13: the rounds the chest's only)
    const char* key = nullptr;
    {
        std::lock_guard lock(g_zone_mutex);
        g_zones[z].weapon = w;
        key = g_zones[z].key;
    }
    log::info("[holster] the %s draws %s", key, w == kRounds ? "the rounds (a round to reload)" : w >= 0 ? kWeaponLabel[w] : "its slots (automatic)");
    if (save) config::set("Holsters", (std::string(key) + "Weapon").c_str(), token_of(w));
}
bool zone_takes_weapons(int z) { return z >= 0 && z < kZones && g_zones[z].slots[0] >= 0; }
bool wheel_at_holster() { return g_wheel_hol.load(std::memory_order_relaxed); }
void set_wheel_at_holster(bool on, bool save) {
    if (g_wheel_hol.exchange(on) != on) log::info("[holster] a trigger held at a holster opens the weapon wheel (WheelAtHolster): %s", on ? "on" : "off");
    if (save) config::set("Holsters", "WheelAtHolster", on ? "1" : "0");
}
int wheel_zone(int c) {
    if (c < 0 || c >= 2 || log::now_ms() - g_wheel_zone_ms.load(std::memory_order_relaxed) > 100.0) return -1;
    return g_wheel_zone[c].load(std::memory_order_relaxed);
}
bool zone_any(int z) { return z >= 0 && z < kZones && gun_zone(g_zones[z]); }
// the belt piece a holster is on (RingsFollowHolsters): its body::Piece index, body::kPieces the mirrored left holster
// (while drawn), -1 none
int zone_piece(int z) {
    static const int kBonePieceZ[body::kHolsterBones] = {0, 4, 2, 1, -1, -1, 3};
    if (z < 0 || z >= kZones) return -1;
    const Zone& zz = g_zones[z];
    if (zz.bone < 0 || zz.bone >= body::kHolsterBones) return -1;
    if (zz.mirror && zz.bone == 0) return body::left_holster() ? body::kPieces : -1;
    return kBonePieceZ[zz.bone];
}
// [Holsters] RingsFollowHolsters (2026-10-13, the user: "they hold their position relative to eachother. Moving one
// should move the other the same way"): a piece and the rings on it shifted together by every move of either (the hand
// placings, the menu's arrows); a turn of a piece leaves its rings
void shift_zone_world(int z, const float dw[3]);  // (below)
// a piece as the shipped RDRVR.ini places it (the release's belt; the game's place with its OwnBelt off)
body::Piece shipped_piece(int pc) {
    body::Piece p;
    const bool own = config::get_shipped("Holsters", "OwnBelt", "0") == "1";
    if (!own || (pc == body::kPieces && config::get_shipped("Holsters", "OwnBeltLeftHolster", "0") != "1")) return p;
    const std::string k = pc == body::kPieces ? std::string("LeftHolster") : std::string(body::piece_key(pc));
    const std::string o = config::get_shipped("Holsters", ("OwnBelt" + k + "Offset").c_str(), "");
    const std::string t = config::get_shipped("Holsters", ("OwnBelt" + k + "Turn").c_str(), "");
    if (!o.empty()) sscanf_s(o.c_str(), "%f %f %f", &p.d[0], &p.d[1], &p.d[2]);
    if (!t.empty()) sscanf_s(t.c_str(), "%f %f %f", &p.turn[0], &p.turn[1], &p.turn[2]);
    return p;
}
// 2026-10-13 (the release review: the shipped rings are placed for the shipped belt's moved pieces): with
// RingsFollowHolsters, each linked ring's default is the shipped one moved as its piece now differs from the shipped
// piece (the belt off: the game's places), so a reset ring sits on its piece as shipped
void reset_rings() {
    reset_rings_now();
    if (!g_rings_follow.load(std::memory_order_relaxed)) return;
    for (int z = 0; z < kZones; ++z) {
        const int pc = zone_piece(z);
        if (pc < 0) continue;
        const body::Piece cur = !body::own_pieces() ? body::Piece{} : pc == body::kPieces ? body::left_holster_piece() : body::piece(pc);
        float dw[3];
        if (!body::piece_shift_world(shipped_piece(pc), cur, dw)) continue;
        {
            std::lock_guard lock(g_zone_mutex);
            shift_zone_world(z, dw);
        }
        save_zone(z);
    }
}
// 2026-10-13 (the release review: "Customize John's belt" turned off left the rings where the moved pieces were): with
// RingsFollowHolsters, the belt turned off moves each linked ring back as its piece goes to the game's place, and on
// again out with it (the left holster's ring: its own offset, used only while the copy is drawn)
void set_own_pieces_linked(bool on, bool save) {
    if (on != body::own_pieces() && g_rings_follow.load(std::memory_order_relaxed)) {
        for (int z = 0; z < kZones; ++z) {
            const int pc = zone_piece(z);
            if (pc < 0 || pc >= body::kPieces) continue;
            const body::Piece p = body::piece(pc);
            float dw[3];
            if (!(on ? body::piece_shift_world(body::Piece{}, p, dw) : body::piece_shift_world(p, body::Piece{}, dw))) continue;
            {
                std::lock_guard lock(g_zone_mutex);
                shift_zone_world(z, dw);
            }
            if (save) save_zone(z);
        }
    }
    body::set_own_pieces(on, save);
}
int link_piece(int z) {
    if (!g_rings_follow.load(std::memory_order_relaxed) || !body::own_pieces()) return -1;
    return zone_piece(z);
}
void shift_zone_world(int z, const float dw[3]) {  // (under g_zone_mutex)
    float loc[3] = {dw[0] * g_link_rgt[0] + dw[2] * g_link_rgt[2], dw[1], dw[0] * g_link_fwd[0] + dw[2] * g_link_fwd[2]};
    if (g_link_lefty) loc[0] = -loc[0];
    float* o = zone_off(g_zones[z]);
    for (int k = 0; k < 3; ++k) o[k] = std::fmin(0.8f, std::fmax(-0.8f, o[k] + loc[k]));
}
void link_piece_moved(int pc, const float dw[3], int except) {
    if (pc < 0) return;
    std::lock_guard lock(g_zone_mutex);
    for (int z = 0; z < kZones; ++z)
        if (z != except && link_piece(z) == pc) shift_zone_world(z, dw);
}
void link_zone_moved(int z, const float dw[3]) {
    const int pc = link_piece(z);
    if (pc < 0) return;
    const body::Piece cur = pc == body::kPieces ? body::left_holster_piece() : body::piece(pc);
    body::Piece np;
    if (!body::piece_shifted(cur, dw, &np)) return;
    if (pc == body::kPieces) body::set_left_holster_piece(np, false);
    else body::set_piece(pc, np, false);
    link_piece_moved(pc, dw, z);
}
void link_save(int pc, int except) {  // (the rings, then the piece)
    if (pc < 0) return;
    for (int z = 0; z < kZones; ++z)
        if (z != except && link_piece(z) == pc) save_zone(z);
    if (pc == body::kPieces) body::save_left_holster_piece();
    else body::save_piece(pc);
}
void set_piece_linked(int pc, const body::Piece& np, bool save) {
    const body::Piece cur = pc == body::kPieces ? body::left_holster_piece() : body::piece(pc);
    float dw[3];
    const bool linked = g_rings_follow.load(std::memory_order_relaxed) && body::own_pieces() && body::piece_shift_world(cur, np, dw);
    if (linked) link_piece_moved(pc, dw, -1);
    if (linked && save)
        for (int z = 0; z < kZones; ++z)
            if (link_piece(z) == pc) save_zone(z);
    if (pc == body::kPieces) body::set_left_holster_piece(np, save);
    else body::set_piece(pc, np, save);
}
void set_zone_linked(int z, const float off[3], float radius, bool save) {
    if (z < 0 || z >= kZones) return;
    float o0[3], r0 = 0.0f, rg[3], fw[3];
    bool lefty = false;
    zone(z, o0, &r0);
    {
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(rg, g_link_rgt, sizeof(rg));
        std::memcpy(fw, g_link_fwd, sizeof(fw));
        lefty = g_link_lefty;
    }
    set_zone(z, off, radius, save);
    const int pc = link_piece(z);
    if (pc < 0) return;
    const float dl[3] = {lefty ? -(off[0] - o0[0]) : off[0] - o0[0], off[1] - o0[1], off[2] - o0[2]};
    const float dw[3] = {rg[0] * dl[0] + fw[0] * dl[2], dl[1], rg[2] * dl[0] + fw[2] * dl[2]};
    if (dw[0] == 0.0f && dw[1] == 0.0f && dw[2] == 0.0f) return;
    link_zone_moved(z, dw);
    if (save) link_save(pc, z);
}
bool zone_enabled(int z) {
    std::lock_guard lock(g_zone_mutex);
    return z >= 0 && z < kZones && g_zones[z].enabled;
}
void set_zone_enabled(int z, bool on, bool save) {
    if (z < 0 || z >= kZones || g_zones[z].slots[0] < 0) return;  // the chest is always on
    {
        std::lock_guard lock(g_zone_mutex);
        g_zones[z].enabled = on;
    }
    log::info("[holster] the %s holster %s", g_zones[z].key, on ? "on" : "off");
    if (save) config::set("Holsters", (std::string(g_zones[z].key) + "Enabled").c_str(), on ? "1" : "0");
}
bool zone_fits(int z, int equip_slot) {
    if (z < 0 || z >= kZones) return false;
    for (int s : g_zones[z].slots)
        if (s >= 0 && s == equip_slot) return true;
    return false;
}
bool zone_fits_weapon(int z, int weapon, int equip_slot) { return z >= 0 && z < kZones && weapon_fits(g_zones[z], weapon, equip_slot); }
bool arsenal(Arsenal* out) {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.owned_tick) {
        out->valid = false;
        return false;
    }
    out->valid = true;
    out->owned = st.owned;
    std::memcpy(out->equip_slot, st.equip_slot, sizeof(out->equip_slot));
    return true;
}
bool placing() { return g_place.load(std::memory_order_relaxed); }
bool rings_follow() { return g_rings_follow.load(std::memory_order_relaxed); }
void set_rings_follow(bool on, bool save) {
    if (g_rings_follow.exchange(on) != on)
        log::info("[holster] the rings %s", on ? "go with their moved holsters (RingsFollowHolsters)" : "and the holsters moved on their own");
    if (save) config::set("Holsters", "RingsFollowHolsters", on ? "1" : "0");
}
bool placing_pieces() { return g_piece_place.load(std::memory_order_relaxed); }
void set_placing_pieces(bool on) {
    if (on) set_own_pieces_linked(true, true);  // (the pieces moved at all: OwnBelt; the linked rings with them)
    if (g_piece_place.exchange(on) != on) log::info("[holster] placing John's belt's pieces by hand: %s", on ? "on" : "off");
}
std::string pieces_status() {
    char b[160];
    std::snprintf(b, sizeof(b), "pieces placing %d, placed %llu, held L %d R %d, own pieces %d", g_piece_place.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_pieces_placed.load()), g_piece_held[0].load(), g_piece_held[1].load(), body::own_pieces() ? 1 : 0);
    return b;
}
void set_placing(bool on) {
    if (g_place.exchange(on) != on) log::info("[holster] placing the holsters by hand: %s", on ? "on" : "off");
}
bool zones_near() { return g_zones_near.load(std::memory_order_relaxed); }
void set_zones_near(bool on, bool save) {
    if (g_zones_near.exchange(on) != on) log::info("[holster] the rings only near a hand: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "ZonesNear", on ? "1" : "0");
}
bool steady_zones() { return g_steady_zones.load(std::memory_order_relaxed); }
void set_steady_zones(bool on, bool save) {
    if (g_steady_zones.exchange(on) != on) log::info("[holster] the holsters' edges steadied: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "SteadyZones", on ? "1" : "0");
}
bool show_zones() { return g_show_zones.load(); }
void set_show_zones(bool on) {
    set_show_zones_session(on);
    config::set("Holsters", "ShowZones", on ? "1" : "0");
    // the other two written as they are, so they no longer follow ShowZones at the next start (the review's finding)
    config::set("Holsters", "ShowHandDots", g_show_dots.load() ? "1" : "0");
    config::set("Holsters", "ShowWeaponPoints", g_show_points.load() ? "1" : "0");
}
void set_show_zones_session(bool on) {
    if (g_show_zones.exchange(on) != on) log::info("[holster] rings %s", on ? "shown" : "hidden");
}
bool show_hand_dots() { return g_show_dots.load(); }
bool show_weapon_points() { return g_show_points.load(); }
void set_show_hand_dots(bool on, bool save) {
    if (g_show_dots.exchange(on) != on) log::info("[holster] the hand dots %s", on ? "shown" : "hidden");
    if (save) config::set("Holsters", "ShowHandDots", on ? "1" : "0");
}
void set_show_weapon_points(bool on, bool save) {
    if (g_show_points.exchange(on) != on) log::info("[holster] the weapon points %s", on ? "shown" : "hidden");
    if (save) config::set("Holsters", "ShowWeaponPoints", on ? "1" : "0");
}
bool markers(Markers* out) {
    if (!g_show_zones.load(std::memory_order_relaxed) && !g_show_dots.load(std::memory_order_relaxed) && !g_show_points.load(std::memory_order_relaxed) &&
        !actions::show_fan_zone())
        return false;
    std::lock_guard lock(g_hdiag_mutex);
    if (!g_markers_valid || log::now_ms() - g_markers_ms > 100.0) return false;
    *out = g_markers;
    return true;
}
int interaction_highlights() { return g_hl_mode.load(std::memory_order_relaxed); }
bool rounds_for_second() { return g_rounds_for_sec.load(std::memory_order_relaxed); }
int highlight_gun(int id) {
    if (id < kHlFore || id >= kHlCount) return -1;
    if (id == kHlLoadSec) return 1;
    if ((id == kHlPart || id == kHlPump) && g_hl_carry_free.load(std::memory_order_relaxed)) return 1;
    return (g_hl_carried.load(std::memory_order_relaxed) >> id) & 1u ? 1 : 0;
}
void set_interaction_highlights(int mode, bool save) {
    mode = mode < 0 ? 0 : mode > 3 ? 3 : mode;
    if (g_hl_mode.exchange(mode) != mode) log::info("[holster] the interaction points shown as %s", kHlModes[mode]);
    if (mode == 3) pso::glow::want(true);  // (run 29 item 2: once wanted the twins stay; leaving parts only stops the glow's draws)
    if (save) config::set("Holsters", "InteractionHighlights", kHlModes[mode]);
}
void part_glow_color(float rgb[3]) {
    std::lock_guard lock(g_glow_mx);
    std::memcpy(rgb, g_glow_rgb, sizeof(g_glow_rgb));
}
void set_part_glow_color(const float rgb[3], bool save) {
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = std::isfinite(rgb[k]) ? std::fmin(1.0f, std::fmax(0.0f, rgb[k])) : 0.0f;
    {
        std::lock_guard lock(g_glow_mx);
        std::memcpy(g_glow_rgb, c, sizeof(c));
    }
    if (save) {
        char v[48];
        std::snprintf(v, sizeof(v), "%.2f %.2f %.2f", c[0], c[1], c[2]);
        config::set("Holsters", "PartGlowColor", v);
    }
}
float part_glow_strength() { return g_glow_strength.load(std::memory_order_relaxed); }
void set_part_glow_strength(float s, bool save) {
    s = std::isfinite(s) ? std::fmin(8.0f, std::fmax(0.0f, s)) : 1.0f;
    g_glow_strength = s;
    if (save) {
        char v[16];
        std::snprintf(v, sizeof(v), "%.2f", s);
        config::set("Holsters", "PartGlowStrength", v);
    }
}
bool highlights(Highlights* out) {
    if (g_hl_mode.load(std::memory_order_relaxed) == 0) return false;
    std::lock_guard lock(g_hdiag_mutex);
    if (!g_hl_valid || log::now_ms() - g_hl_ms > 100.0) return false;
    *out = g_hl;
    return true;
}
const char* highlight_name(int id) {
    static const char* const kPart[kHlCount - kZones] = {"Foregrip", "LoadPoint", "MagWell", "LoadPoint2", "Part", "Lever", "Pump", "Shut"};
    return id >= 0 && id < kZones ? g_zones[id].key : id >= kZones && id < kHlCount ? kPart[id - kZones] : "?";  // (the keys are constant)
}
bool aim_pose() { return g_aim_pose.load(std::memory_order_relaxed); }
bool gun_raised() { return g_raised.load(std::memory_order_relaxed); }
bool fire_ready() { return g_fire_ready.load(std::memory_order_relaxed); }
float fire_clip_phase() { return g_fire_phase.load(std::memory_order_relaxed); }
bool show_models() { return g_show_models.load(std::memory_order_relaxed); }
bool belt_props() { return g_belt_props.load(std::memory_order_relaxed); }
bool belt_props_default() { return g_bp_model.load(std::memory_order_relaxed); }
bool belt_throwable() { return g_bp_throwable.load(std::memory_order_relaxed); }
std::atomic<int> g_game_gate_force{-1};  // (GOAL-doorway-and-poker item 3) "holster gate force <n|off>": the tests' gate
int game_gate() {  // (the re-review: holster_frame stopped, the Holsters off or the body off, a gate stale for 2 s is open)
    const int f = g_game_gate_force.load(std::memory_order_relaxed);
    if (f >= 0) return f;
    return log::now_ms() - g_game_gate_ms.load(std::memory_order_relaxed) > 2000.0 ? 0 : g_game_gate.load(std::memory_order_relaxed);
}
bool table_game() { return gate_table(game_gate()); }
int minigame() {
    const int f = g_minigame_force.load(std::memory_order_relaxed);
    return f >= 0 ? f : g_minigame.load(std::memory_order_relaxed);
}
// (the reviews) walked within 2 s: with no script tick (a load started at the table) the last answer is not one to refuse
// the game's hides by; the pause menu keeps it for the arms (the gate stands there too)
bool minigame_fresh() {
    return g_minigame_force.load(std::memory_order_relaxed) >= 0 || log::now_ms() - g_script_walk_ms.load(std::memory_order_relaxed) < 2000.0;
}
bool minigame_known() { return g_minigame_force.load(std::memory_order_relaxed) >= 0 || g_script_list_state.load(std::memory_order_relaxed) == 1; }
bool seated_view() { return gate_seated(game_gate()); }
void set_belt_throwable(bool on, bool save) {  // (round 117) [Holsters] OwnBeltThrowable
    if (g_bp_throwable.exchange(on) != on) log::info("[holster] the equipped throwable at the satchel's place: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "OwnBeltThrowable", on ? "1" : "0");
}
void set_belt_props_default(bool on, bool save) {  // (GOAL-after-0.8.13.2 item 7) [Holsters] OwnBeltPropsDefault: never on (round 115)
    if (on) log::warn("[holster] the default outfit's own satchel refused: the game faults making its model as a prop");
    g_bp_model = false;
    if (save) config::set("Holsters", "OwnBeltPropsDefault", "0");
}
void set_belt_props(bool on, bool save) {
    if (g_belt_props.exchange(on) != on) log::info("[holster] the belt's props on outfits without the pieces: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "OwnBeltProps", on ? "1" : "0");
}
// the prop's pose (round 103): the knife and the lasso on their piece bones as drawn (the piece's own move is in the drawn
// bone), in its frame as the default outfit hangs them (kBpOnBone); the default model (k 1) at John's root in the body's
// frame (its model's axes x right, y up, z back: its draws are rewritten onto the bones, its place only where the game
// draws and culls it). Then the user's moves: turned by OwnBeltProp<Piece>Turn about the holsters' axes (body.cpp
// piece_world's order: about forward first, then right, then up), moved by Home (right, up, forward). False: no bone
namespace {  // (the declaration's, above)
bool belt_prop_pose(int k, const float* hr, const float* up, const float* hf, const float* origin, float* pz, float* anchor, bool model,
                    int thrown, bool thrown_adj) {
    float home[3], base[3], sc, adj[12];
    const int ti = thrown >= 0 && thrown_adj ? throwable_index(thrown) : -1;
    {
        std::lock_guard lock(g_bp_mutex);
        if (ti >= 0) std::memcpy(adj, g_thr_adj[ti], sizeof(adj));
        std::memcpy(home, g_bp_home[k], sizeof(home));
        std::memcpy(base, g_bp_turn[k], sizeof(base));
        sc = thrown >= 0 ? 1.0f : g_bp_scale[k];  // (round 117: a throwable at its own size)
    }
    float X[3], Y[3], Z[3], at[3];
    if (k == 1 && model) {  // (the review: the flag read once a tick, by the caller)
        for (int i = 0; i < 3; ++i) X[i] = hr[i], Y[i] = up[i], Z[i] = -hf[i], at[i] = origin[i];
        sc = 1.0f;
    } else {
        float W[12];
        if (!body::piece_bone_drawn(k + 1, W, anchor)) return false;  // (the melee, thrower and rope bones; their set's pelvis)
        const float* q = kBpOnBone[k];
        for (int i = 0; i < 3; ++i) {
            X[i] = W[i * 3] * q[0] + W[i * 3 + 1] * q[3] + W[i * 3 + 2] * q[6];
            Y[i] = W[i * 3] * q[1] + W[i * 3 + 1] * q[4] + W[i * 3 + 2] * q[7];
            Z[i] = W[i * 3] * q[2] + W[i * 3 + 1] * q[5] + W[i * 3 + 2] * q[8];
            at[i] = W[i * 3] * q[9] + W[i * 3 + 1] * q[10] + W[i * 3 + 2] * q[11] + W[9 + i];
        }
    }
    const float r = 0.0174532925f;
    if (thrown >= 0) {  // (round 117) the throwable's own roll and drop (kThrownOnBone)
        const ThrownOnBone& t = thrown_on_bone(thrown);
        for (float* v : {X, Y, Z}) turn_about(v, hf, -t.roll * r);
        for (int i = 0; i < 3; ++i) at[i] += up[i] * t.drop;
    }
    for (float* v : {X, Y, Z}) turn_about(v, hf, -base[2] * r);
    for (float* v : {X, Y, Z}) turn_about(v, hr, base[0] * r);
    for (float* v : {X, Y, Z}) turn_about(v, up, base[1] * r);
    for (int i = 0; i < 3; ++i) {
        pz[i] = X[i] * sc;
        pz[3 + i] = Y[i] * sc;
        pz[6 + i] = Z[i] * sc;
        pz[9 + i] = at[i] + hr[i] * home[0] + up[i] * home[1] + hf[i] * home[2];
    }
    if (ti >= 0) {  // (round 120) the throwable's own place, set by hand: its turn (F Rl F^T, F its own axes as placed) and move
        const float F0[3] = {pz[0], pz[1], pz[2]}, F1[3] = {pz[3], pz[4], pz[5]}, F2[3] = {pz[6], pz[7], pz[8]};
        const float* F[3] = {F0, F1, F2};
        float Rw[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                float v = 0.0f;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) v += F[a][r] * adj[a * 3 + b] * F[b][c];
                Rw[r * 3 + c] = v;
            }
        for (int ax = 0; ax < 3; ++ax) {
            const float x[3] = {pz[ax * 3], pz[ax * 3 + 1], pz[ax * 3 + 2]};
            for (int r = 0; r < 3; ++r) pz[ax * 3 + r] = Rw[r * 3] * x[0] + Rw[r * 3 + 1] * x[1] + Rw[r * 3 + 2] * x[2];
        }
        for (int i = 0; i < 3; ++i) pz[9 + i] += F0[i] * adj[9] + F1[i] * adj[10] + F2[i] * adj[11];
    }
    return true;
}
}  // namespace

void set_show_models(bool on, bool save) {
    if (g_show_models.exchange(on) != on) log::info("[holster] the holster models (%s) at the hips: %s", g_model_name, on ? "on" : "off");
    if (save) config::set("Holsters", "ShowModels", on ? "1" : "0");
}
bool show_guns() { return g_show_guns.load(std::memory_order_relaxed); }
void set_show_guns(bool on, bool save) {
    if (g_show_guns.exchange(on) != on) log::info("[holster] the guns shown at the holsters: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "ShowGuns", on ? "1" : "0");
}
int anchor() { return g_anchor.load(std::memory_order_relaxed); }
bool lean_steady() { return g_lean_steady.load(std::memory_order_relaxed); }
float torso_length() { return g_torso_len.load(std::memory_order_relaxed); }
void set_lean_steady(bool on, bool save) {
    if (g_lean_steady.exchange(on) != on) log::info("[holster] the holsters steady when you lean: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "LeanSteady", on ? "1" : "0");
}
bool turn_by_hands() { return g_turn_hands.load(std::memory_order_relaxed); }
void set_turn_by_hands(bool on, bool save) {
    if (g_turn_hands.exchange(on) != on) log::info("[holster] the holsters' turn by the head and the hands: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "TurnByHands", on ? "1" : "0");
}
bool turn_with_head() { return g_turn_head.load(std::memory_order_relaxed); }
void set_turn_with_head(bool on, bool save) {
    if (g_turn_head.exchange(on) != on) log::info("[holster] the holsters turn with the headset: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "TurnWithHead", on ? "1" : "0");
}
float turn_dead_zone() { return g_turn_dead.load(std::memory_order_relaxed); }
void set_turn_dead_zone(float deg, bool save) {
    deg = std::fmin(90.0f, std::fmax(0.0f, deg));
    g_turn_dead = deg;
    if (save) {
        char v[16];
        std::snprintf(v, sizeof(v), "%.0f", deg);
        config::set("Holsters", "TurnDeadZone", v);
    }
}
void set_anchor(int a, bool save) {
    a = a == 1 ? 1 : 0;
    if (g_anchor.exchange(a) != a) log::info("[holster] the holsters anchored to the %s", a ? "headset (the neck's offset)" : "body");
    if (save) config::set("Holsters", "Anchor", a ? "headset" : "body");
}
bool show_back_guns() { return g_show_back_guns.load(std::memory_order_relaxed); }
void set_show_back_guns(bool on, bool save) {
    if (g_show_back_guns.exchange(on) != on) log::info("[holster] the long guns shown on the back: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "ShowBackGuns", on ? "1" : "0");
}
bool gun_at_zone() { return g_gun_at_zone.load(std::memory_order_relaxed); }
bool unarmed_after_holster() { return g_unarmed.load(); }
void set_unarmed_after_holster(bool on) {
    if (g_unarmed.exchange(on) != on) log::info("[holster] a put-away selects the fists %d", on ? 1 : 0);
    config::set("Holsters", "UnarmedAfterHolster", on ? "1" : "0");
}
bool hold_to_keep() { return g_hold.load(); }
bool hold_items_on() { return g_hold_items.load(std::memory_order_relaxed); }
bool grip_holds(int h, int w) {
    return h >= 0 && h < 2 && w >= 0 && g_hold.load(std::memory_order_relaxed) && !wheel::wheel_mode() &&
           g_keep_pub[h].load(std::memory_order_relaxed) == 1 && g_keep_w_pub[h].load(std::memory_order_relaxed) == w;
}
void set_hold_to_keep(bool on, bool save) {
    // (off: the held guns loose at the next frame end, nothing put away; on: a gun already in hand is loose until its grip
    // is pressed and let go)
    if (g_hold.exchange(on) != on) log::info("[holster] hold the grip to keep the gun %d", on ? 1 : 0);
    if (save) config::set("Holsters", "HoldToKeep", on ? "1" : "0");
}
bool grip_keeps(int h) { return h >= 0 && h < 2 && g_keeping[h].load(std::memory_order_relaxed); }
bool grip_wanted(int h) { return h >= 0 && h < 2 && g_wanted[h].load(std::memory_order_relaxed); }
bool carry_take_point(float out[3], float ctl[3], bool* by_gun) {
    std::lock_guard lock(g_take_mx);
    if (!g_take_ok || log::now_ms() - g_take_ms > 100.0) return false;
    std::memcpy(out, g_take_pt, sizeof(g_take_pt));
    if (ctl) std::memcpy(ctl, g_take_ctl, sizeof(g_take_ctl));
    if (by_gun) *by_gun = g_take_gun;
    return true;
}
int zone_count() { return kZones; }
const char* zone_label(int i) { return i >= 0 && i < kZones ? g_zones[i].label : "?"; }
void zone(int i, float off[3], float* radius) {
    if (i < 0 || i >= kZones) return;
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(off, zone_off(g_zones[i]), sizeof(g_zones[i].off));
    *radius = g_zones[i].radius;
}
void set_zone(int i, const float off[3], float radius, bool save) {
    if (i < 0 || i >= kZones) return;
    {
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(zone_off(g_zones[i]), off, sizeof(g_zones[i].off));
        g_zones[i].radius = radius;
    }
    if (save) save_zone(i);
}
bool zone_shows_gun(int i) {
    for (int z : kShowZones)
        if (z == i) return true;
    return false;
}
void zone_gun(int i, float turn[3], float off[3]) {
    if (i < 0 || i >= kZones) return;
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(turn, g_zones[i].gun_turn, sizeof(g_zones[i].gun_turn));
    std::memcpy(off, g_zones[i].gun_off, sizeof(g_zones[i].gun_off));
}
void set_zone_gun(int i, const float turn[3], const float off[3], bool save) {
    if (i < 0 || i >= kZones) return;
    {
        std::lock_guard lock(g_zone_mutex);
        for (int k = 0; k < 3; ++k) {
            g_zones[i].gun_turn[k] = std::isfinite(turn[k]) ? std::fmin(180.0f, std::fmax(-180.0f, turn[k])) : 0.0f;
            g_zones[i].gun_off[k] = std::isfinite(off[k]) ? std::fmin(0.3f, std::fmax(-0.3f, off[k])) : 0.0f;
        }
    }
    if (save) save_zone_gun(i);
}

void set_keeps_drawn(bool on) {
    if (g_keep.exchange(on) != on) log::info("[holster] keep the gun drawn: %s", on ? "on" : "off");
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v;
    in >> c >> v;
    if (v == "dump") {
        std::string path;
        in >> path;
        return dump(path);
    }
    if (v == "gate") {  // (round 114) the holsters closed by the game: a minigame, the player's control held
        std::string f, n;  // (item 3) holster gate force <n|off>: game_gate() answers n (the tests; a minigame is bit 1)
        if (in >> f && f == "force" && in >> n) g_game_gate_force = n == "off" ? -1 : std::atoi(n.c_str());
        const int g = g_game_gate.load();
        return "gate " + std::to_string(g) + " (minigame " + std::to_string(g & 1) + ", control held " + std::to_string((g >> 1) & 1) + ", in a gringo " +
               std::to_string((g >> 2) & 1) + ", on a train " + std::to_string((g >> 3) & 1) + "; a table game " + std::to_string(gate_table(g) ? 1 : 0) +
               ", the seated view " + std::to_string(gate_seated(g) ? 1 : 0) + "), closed " +
               std::to_string(g_gate_closes.load()) + " times" +
               (g_game_gate_force.load() >= 0 ? " | forced " + std::to_string(g_game_gate_force.load()) + " (the tests)" : std::string());
    }
    if (v == "scripts") {  // holster scripts [force <bits>|off] [all]: (round 130) which minigame's script runs; the running scripts' names
        std::string f, n;
        bool all = false;
        if (in >> f && f == "force" && in >> n) g_minigame_force = n == "off" ? -1 : std::atoi(n.c_str());
        else if (f == "all") all = true;
        const int g = minigame();
        return "minigame " + std::to_string(g) + " (" + mini_text(g) + ")" +
               (g_minigame_force.load() >= 0 ? " forced (the tests)" : std::string()) + ", the list " + std::to_string(g_script_list_state.load()) +
               ", walks " + std::to_string(g_script_walks.load()) + " (ended early " + std::to_string(g_script_bad.load()) + "), scripts " +
               std::to_string(g_script_nodes.load()) + " | (backslashes shown as slashes) " + scripts_with(all ? nullptr : "mini", all ? 2400 : 900, true);
    }
    if (v == "highlights") {  // holster highlights [rings|highlights|both]: [Holsters] InteractionHighlights for the session; each
        // place: exists (e), usable now (u), over the sights (s), the judging hand's distance and reach, the wanted strength;
        // then the drawing's (zone_rings: each layer drawn, its strength and where each eye sees it)
        std::string x;
        in >> x;
        if (x == "rings" || x == "highlights" || x == "both" || x == "parts")
            set_interaction_highlights(x == "rings" ? 0 : x == "highlights" ? 1 : x == "both" ? 2 : 3, false);
        HlDiag dg[kHlCount];
        bool valid = false;
        double age = -1.0;
        float cam[16] = {};
        {
            std::lock_guard lock(g_hdiag_mutex);
            for (int i = 0; i < kHlCount; ++i) dg[i] = g_hl_diag[i];
            valid = g_hl_valid;
            age = log::now_ms() - g_hl_ms;
            std::memcpy(cam, g_hl.cam, sizeof(cam));
        }
        std::string r = std::string("highlights mode ") + kHlModes[g_hl_mode.load() & 3] + (valid && age < 100.0 ? " fresh" : " stale");
        r += " (hint " + std::to_string(g_hl_dbg_hint.load()) + " frame " + std::to_string(g_hl_dbg_frame.load()) + " shut " + std::to_string(g_hl_dbg_shut.load()) + ")";
        for (int i = 0; i < kHlCount; ++i) {
            if (!dg[i].exists || !valid || age >= 100.0) continue;  // (a stale list is not printed: the mode rings keeps none)
            char b[260];
            float l[3] = {};
            const bool lok = camera_lever::world_to_local(cam, dg[i].pos, l);
            std::snprintf(b, sizeof(b), " | %s e1 u%d s%d d %.3f reach %.3f t %.2f h %d at (%.3f %.3f %.3f) on %s local %s(%.3f %.3f %.3f)", highlight_name(i),
                          dg[i].usable ? 1 : 0, dg[i].sights ? 1 : 0, dg[i].d, dg[i].reach, dg[i].target, dg[i].hand, dg[i].pos[0], dg[i].pos[1], dg[i].pos[2],
                          dg[i].on && *dg[i].on ? dg[i].on : "-", lok ? "" : "none ", l[0], l[1], l[2]);
            r += b;
        }
        std::vector<char> z(4096);  // (kHlCount = 15 layers' lines and each place's drawn and culled frames: about 3 KB at most)
        zone_rings::highlights_status(z.data(), z.size());
        return r + " || " + z.data();
    }
    if (v == "glow") {  // (run 29 item 2) holster glow [mode rings|highlights|both|parts] [color r g b] [strength s] [bias n]
        // [force on|off] [passes <hex>] [reset]: the parts mode's session forms (the user ini never written: "mode" as "holster
        // highlights <mode>", color and strength [Holsters] PartGlowColor / PartGlowStrength for the session, bias the twins'
        // depth bias, force off: no glow recorded (the tests' mask frame, the rest as in the mode), passes: the entity passes
        // the glow is recorded in (bit per DrawVisEntity pass & 7), reset: the counters); then the status: the settings, the
        // twins and the playback (pso), the recording and each place's triangles (body)
        std::string w;
        while (in >> w) {
            if (w == "mode") {
                std::string x;
                in >> x;
                if (x == "rings" || x == "highlights" || x == "both" || x == "parts")
                    set_interaction_highlights(x == "rings" ? 0 : x == "highlights" ? 1 : x == "both" ? 2 : 3, false);
            } else if (w == "color") {
                float col[3] = {};
                if (in >> col[0] >> col[1] >> col[2]) set_part_glow_color(col, false);
            } else if (w == "strength") {
                float x = 1.0f;
                if (in >> x) set_part_glow_strength(x, false);
            } else if (w == "bias") {
                int n = 0;
                if (in >> n) pso::glow::set_bias(n);
            } else if (w == "force") {
                std::string x;
                in >> x;
                body::glow_force_off(x == "off");
            } else if (w == "passes") {
                std::string x;
                in >> x;
                body::glow_passes(static_cast<uint32_t>(std::strtoul(x.c_str(), nullptr, 16)));
            } else if (w == "reset") {
                body::glow_reset();
            }
        }
        float col[3];
        part_glow_color(col);
        char b[200];
        std::snprintf(b, sizeof(b), "glow mode %s color %.2f %.2f %.2f strength %.2f live %d || ", kHlModes[g_hl_mode.load() & 3], col[0], col[1], col[2],
                      g_glow_strength.load(), pso::glow::live() ? 1 : 0);
        std::vector<char> z(2048);
        zone_rings::highlights_status(z.data(), z.size());
        return b + pso::glow::status() + " || " + body::glow_status() + " || " + z.data();
    }
    if (v == "rounds") {  // holster rounds <n> [one|all] [gap ms]: [Reload] RoundsPerGrab, RoundsGoIn, RoundInsertMs for the session
        int n = 0;
        std::string m;
        float gap = 0.0f;
        if (in >> n) g_per_grab = n < 1 ? 1 : n > 8 ? 8 : n;
        if (in >> m) g_all_in = m == "all";
        if (in >> gap && gap >= 150.0f && gap <= 2000.0f) g_insert_gap_ms = gap;
        char b[320];
        // (2026-10-27, [Hands] DualWieldCrossReload: its takes and rounds in, and each hand's fire held back)
        std::snprintf(b, sizeof(b), "rounds per grab %d, go in %s, a round each %.0f ms; taken %llu, in %llu; cross taken %llu, in %llu, fire held %d %d; entry takes %llu",
                      g_per_grab.load(), g_all_in.load() ? "all at once" : "one at a time", g_insert_gap_ms.load(),
                      static_cast<unsigned long long>(g_rounds_taken.load()), static_cast<unsigned long long>(g_rounds_in.load()),
                      static_cast<unsigned long long>(g_cross_takes.load()), static_cast<unsigned long long>(g_cross_in.load()),
                      g_cross_fire_hold[0].load() ? 1 : 0, g_cross_fire_hold[1].load() ? 1 : 0, static_cast<unsigned long long>(g_auto_chest.load()));
        return b;
    }
    if (v == "source") {  // holster source [chest|gun|palm]: [Reload] RoundSource for the session (the user ini not written)
        std::string x;
        if (in >> x && (x == "chest" || x == "gun" || x == "palm")) set_round_source(x == "gun" ? 1 : x == "palm" ? 2 : 0, false);
        char b[600];
        const int w0 = g_held_w[0].load(), w1 = g_held_w[1].load();
        std::snprintf(b, sizeof(b),
                      "source %s | palm up L %.2f R %.2f (over %.2f) | near L %.3f (within %.3f) R %.3f (within %.3f) | held L %d (weapon %d, kind %d) "
                      "R %d (weapon %d, kind %d) | hand takes %llu, refused %llu | taken %llu, in %llu | wanted L %d R %d, consumed L %d R %d",
                      g_round_src.load() == 1 ? "gun" : g_round_src.load() == 2 ? "palm" : "chest", g_palm_y[0].load(), g_palm_y[1].load(), kPalmUp,
                      g_near_d[0].load(), g_near_max[0].load(), g_near_d[1].load(), g_near_max[1].load(), g_held_n[0].load(), w0,
                      w0 >= 0 ? round_draw::kind_of(w0) : -1, g_held_n[1].load(), w1, w1 >= 0 ? round_draw::kind_of(w1) : -1,
                      static_cast<unsigned long long>(g_hand_takes.load()), static_cast<unsigned long long>(g_hand_refused.load()),
                      static_cast<unsigned long long>(g_rounds_taken.load()), static_cast<unsigned long long>(g_rounds_in.load()),
                      // (r215: the press held back by the holsters or not, for the test's check that a refused take's press
                      // is the game's, whatever the ini maps the grip to: grip_wanted at the last frame end, and the press consumed)
                      g_wanted[0].load() ? 1 : 0, g_wanted[1].load() ? 1 : 0, g_consumed[0].load() ? 1 : 0, g_consumed[1].load() ? 1 : 0);
        return b;
    }
    if (v == "mag") {  // holster mag: [Reload] PistolMagazines' takes (2026-10-28, the tests')
        float wp[3], tp[2][3], mp[2][3];
        bool ok;
        {
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(wp, g_mag_well_pub, sizeof(wp));
            std::memcpy(tp, g_mag_top_pub, sizeof(tp));
            std::memcpy(mp, g_mag_mid_pub, sizeof(mp));
            ok = g_mag_well_ok;
        }
        char b[640];
        std::snprintf(b, sizeof(b),
                      "mag: well (%.3f %.3f %.3f) ok %d ring %.3f | held L %d R %d | top L %.3f at (%.3f %.3f %.3f) R %.3f at (%.3f %.3f %.3f) | takes %llu "
                      "refused %llu in %llu (last %d rounds) away %llu | middle L (%.3f %.3f %.3f) R (%.3f %.3f %.3f)",
                      wp[0], wp[1], wp[2], ok ? 1 : 0, 0.05, g_mag_held[0].load(), g_mag_held[1].load(), g_mag_top_d[0].load(), tp[0][0], tp[0][1], tp[0][2],
                      g_mag_top_d[1].load(), tp[1][0], tp[1][1], tp[1][2],
                      static_cast<unsigned long long>(g_mag_takes.load()), static_cast<unsigned long long>(g_mag_refused.load()),
                      static_cast<unsigned long long>(g_mag_ins.load()), g_mag_rounds_in.load(), static_cast<unsigned long long>(g_mag_away.load()),
                      mp[0][0], mp[0][1], mp[0][2], mp[1][0], mp[1][1], mp[1][2]);
        return b;
    }
    if (v == "stock") {  // holster stock [on|off|calibrate [ms]|clear]: [Reload] PhysicalStock for the session (run 14)
        std::string x;
        in >> x;
        if (x == "on" || x == "off") g_stock = x == "on";
        if (x == "trigger") {  // holster stock trigger on|off: [Reload] TriggerTakesRound for the session
            std::string y;
            in >> y;
            g_trig_round = y == "on";
        }
        if (x == "calibrate") {
            double ms = 0.0;
            in >> ms;
            stock_calibrate(ms);
        }
        if (x == "clear") {  // (saved, as the calibration saves: the review's leak into the test ini)
            const float z[3] = {0.0f, 0.0f, 0.0f};
            set_stock_turn(z);
        }
        float d[3];
        stock_turn(d);
        char b[160];
        std::snprintf(b, sizeof(b), "physical stock %s, turn (%.2f %.2f %.2f) deg, calibration %s, the trigger takes a round %s", g_stock.load() ? "on" : "off",
                      d[0], d[1], d[2], stock_calibrate_in() >= 0.0 ? "pending" : "none pending", g_trig_round.load() ? "on" : "off");
        return b;
    }
    if (v == "trace") {
        std::string x;
        in >> x;
        if (!o_tupdate) return "trace: not hooked ([Debug] ActionTrace=1 installs it at start)";
        g_trace_n = 0;
        g_trace = x != "off";
        return std::string("trace ") + (g_trace.load() ? "on" : "off") + "; player ped " + std::to_string(g_player_ped.load());
    }
    if (v == "any") {  // holster any on|off: [Holsters] AnyWeapon for the session (run 10 item 10)
        std::string x;
        in >> x;
        g_any_weapon = x == "on";
        log::info("[holster] anything in any holster: %s (the session)", g_any_weapon.load() ? "on" : "off");
        v = "weapons";
    }
    if (v == "beltprops") {  // holster beltprops [on|off|force on|off|home <k> x y z|turn <k> a b c|scale <k> s] (item 2; the session)
        std::string x;
        in >> x;
        if (x == "on" || x == "off") set_belt_props(x == "on", false);
        if (x == "force") {
            std::string y;
            in >> y;
            g_belt_props_force = y == "on";
        }
        if (x == "throwable") {  // holster beltprops throwable on|off (round 117, the session)
            std::string y;
            in >> y;
            set_belt_throwable(y == "on", false);
        }
        if (x == "model") {  // holster beltprops model on|off (round 103, the session): the default outfit's own satchel and sheath
            std::string y;
            in >> y;
            set_belt_props_default(y == "on", false);  // (round 115: refused)
        }
        if (x == "scale") {
            int k = -1;
            float s = 1.0f;
            if (in >> k >> s && k >= 0 && k < kBeltProps && std::isfinite(s)) {
                std::lock_guard lock(g_bp_mutex);
                g_bp_scale[k] = std::fmax(0.2f, std::fmin(2.0f, s));
            }
        }
        if (x == "throwreset") {  // holster beltprops throwreset [all|<name>]: (round 120) a throwable's own place back to none, saved
            std::string y;
            in >> y;
            for (int i = 0; i < kThrowables; ++i)
                if (y.empty() || y == "all" || y == kThrowableKey[i]) {
                    {
                        std::lock_guard lock(g_bp_mutex);
                        std::memcpy(g_thr_adj[i], kThrAdjDefault[i], sizeof(g_thr_adj[i]));  // (round 132: its own default)
                    }
                    char dv[160];
                    const float* dd = kThrAdjDefault[i];
                    std::snprintf(dv, sizeof(dv), "%g %g %g %g %g %g %g %g %g %g %g %g", dd[0], dd[1], dd[2], dd[3], dd[4], dd[5], dd[6], dd[7], dd[8],
                                  dd[9], dd[10], dd[11]);
                    config::set("Holsters", (std::string("OwnBeltThrowable") + kThrowableKey[i] + "Pose").c_str(), dv);
                }
        }
        if (x == "home" || x == "turn") {
            int k = -1;
            float a = 0, b2 = 0, c = 0;
            if (in >> k >> a >> b2 >> c && k >= 0 && k < kBeltProps) {
                std::lock_guard lock(g_bp_mutex);
                float* d = x == "home" ? g_bp_home[k] : g_bp_turn[k];
                d[0] = a, d[1] = b2, d[2] = c;
            }
        }
        // each piece's joint as drawn now, in the holsters' frame from the pelvis (right, up, forward), less its OwnBelt move:
        // where the outfit hangs it (the homes' measure, on the default outfit)
        std::string r = std::string("beltprops ") + (g_belt_props.load() ? "on" : "off") + (g_belt_props_force.load() ? " (forced)" : "");
        float fr[12];
        bool fok;
        {
            std::lock_guard lock(g_bp_mutex);
            std::memcpy(fr, g_bp_frame, sizeof(fr));
            fok = g_bp_frame_ok;
            for (int k = 0; k < kBeltProps; ++k) {
                char b[120];
                std::snprintf(b, sizeof(b), " | %s present %d home (%.3f %.3f %.3f) turn (%.0f %.0f %.0f) scale %.2f", body::piece_key(k + 1),
                              body::piece_present(k + 1) ? 1 : 0, g_bp_home[k][0], g_bp_home[k][1], g_bp_home[k][2], g_bp_turn[k][0], g_bp_turn[k][1],
                              g_bp_turn[k][2], g_bp_scale[k]);
                r += b;
            }
            {  // (round 120) the throwable shown and each one's own place (its move, m)
                char b[260];
                std::snprintf(b, sizeof(b), " | the throwable shown %d at (%.3f %.3f %.3f) %.0f ms ago; placed by hand %llu; the outfit's satchel's "
                              "draws hidden %llu, hidden with the throwable in a hand %llu; draws refused with none left %llu", g_thr_shown_w,
                              g_thr_shown_pos[0], g_thr_shown_pos[1], g_thr_shown_pos[2], log::now_ms() - g_thr_shown_ms,
                              static_cast<unsigned long long>(g_thr_placed.load()), static_cast<unsigned long long>(body::satchel_hidden_draws()),
                              static_cast<unsigned long long>(g_thr_hand_hides.load()), static_cast<unsigned long long>(g_none_left_refused.load()));
                r += b;
                for (int i = 0; i < kThrowables; ++i) {
                    std::snprintf(b, sizeof(b), " | %s own place: moved (%.3f %.3f %.3f), turned %s", kThrowableKey[i], g_thr_adj[i][9], g_thr_adj[i][10],
                                  g_thr_adj[i][11], g_thr_adj[i][0] > 0.9999f && g_thr_adj[i][4] > 0.9999f && g_thr_adj[i][8] > 0.9999f ? "no" : "yes");
                    r += b;
                }
            }
        }
        for (int i = 0; i < body::kPieces && fok; ++i) {
            float j[3];
            if (!body::piece_where(i, j)) continue;
            const float d[3] = {j[0] - fr[9], j[1] - fr[10], j[2] - fr[11]};
            const body::Piece p = body::piece(i);
            char b[120];
            std::snprintf(b, sizeof(b), " | %s's joint (%.3f %.3f %.3f), less its move (%.3f %.3f %.3f)", body::piece_key(i),
                          d[0] * fr[0] + d[1] * fr[1] + d[2] * fr[2], d[0] * fr[3] + d[1] * fr[4] + d[2] * fr[5], d[0] * fr[6] + d[1] * fr[7] + d[2] * fr[8],
                          d[0] * fr[0] + d[1] * fr[1] + d[2] * fr[2] - p.d[0], d[0] * fr[3] + d[1] * fr[4] + d[2] * fr[5] - p.d[1],
                          d[0] * fr[6] + d[1] * fr[7] + d[2] * fr[8] - p.d[2]);
            r += b;
        }
        return r;
    }
    if (v == "weapon") {  // holster weapon <Key> <token|auto|Rounds> [session] (writes the user ini, as the menu does; with
                          // "session" (2026-10-29, a test's) the session only)
        std::string key, tok, ses;
        in >> key >> tok >> ses;
        for (int z = 0; z < kZones; ++z)
            if (_stricmp(key.c_str(), g_zones[z].key) == 0) set_zone_weapon(z, weapon_from_token(tok), ses != "session");
        v = "weapons";
    }
    if (v == "slots") {  // holster slots: the weapon manager's slots, each one's gun (the game's object of that slot)
        const uintptr_t actor = player_actor();
        uintptr_t wmgr = 0, hand = 0;
        int32_t cur = -1;
        if (!actor || !rd(actor + 0x70, &wmgr) || !wmgr) return "slots: no weapon manager";
        rd(wmgr + 0x80, &hand);
        rd(wmgr + 0x448, &cur);
        std::string r = "slots: current " + std::to_string(cur) + ":";
        for (int s = 0; s < 8; ++s) {
            uintptr_t item = 0, W = 0, info = 0;
            int16_t wt = -1;
            if (rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item && rd(item + 0xa0, &W) && W && rd(W + 0x28, &info) && info)
                rd(info + 8, &wt);
            r += " " + std::to_string(s) + "=" + (wt >= 0 && wt < kWeapons ? kWeaponToken[wt] : item ? "?" : "-") + (item && item == hand ? "(in hand)" : "");
        }
        return r;
    }
    if (v == "weapons") {  // holster weapons: the owned weapons with their slots, each holster's choice
        Arsenal a{};
        const bool ok = arsenal(&a);
        std::string r = std::string("weapons: choice ") + (g_weapon_choice.load() ? "1" : "0") + ", owned" + (ok ? ":" : " (not read yet)");
        for (int w = 0; ok && w < kWeapons; ++w)
            if (a.owned >> w & 1) r += std::string(" ") + kWeaponToken[w] + "@" + std::to_string(a.equip_slot[w]);
        r += " | choices:";
        for (int z = 0; z < kZones; ++z)
            if (g_zones[z].slots[0] >= 0) {
                const int w = zone_weapon(z);
                r += std::string(" ") + g_zones[z].key + "=" + token_of(w);
                if (w >= 0 && ok && !(a.owned >> w & 1)) r += "(not owned)";
            }
        r += " | choice draws " + std::to_string(g_choice_draws.load()) + ", second-gun slot switches " + std::to_string(g_sec_switches.load()) +
             ", refused " + std::to_string(g_sec_refused.load());
        return r;
    }
    if (v == "cross") {  // holster cross: [Hands] DualWieldCrossReload's loading points, its counts and each hand's fire hold
        LoadDiag d;
        {
            std::lock_guard lock(g_zone_mutex);
            d = g_load_diag;
        }
        char b[400];
        std::snprintf(b, sizeof(b), "cross %d | the second gun's loading point (%.3f %.3f %.3f) ring %.2f ok %d | the gun's loading point (%.3f %.3f %.3f) | "
                      "taken %llu, in %llu | fire held %d %d | row cross %d | on the model %llu",
                      dual::cross_reload() ? 1 : 0, d.sec_pt[0], d.sec_pt[1], d.sec_pt[2], d.sec_r, d.sec_ok ? 1 : 0, d.pt[0], d.pt[1], d.pt[2],
                      static_cast<unsigned long long>(g_cross_takes.load()), static_cast<unsigned long long>(g_cross_in.load()),
                      g_cross_fire_hold[0].load() ? 1 : 0, g_cross_fire_hold[1].load() ? 1 : 0, g_row_cross.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_load_on_model.load()));
        return b;
    }
    if (v == "load") {  // holster load [set <right> <up> <forward> [radius] | touch on|off] (the session only): the loading point
        std::string x;
        in >> x;
        float off[3], r = 0;
        bool touch = false;
        load_point(off, &r, &touch);
        if (x == "set") {
            in >> off[0] >> off[1] >> off[2];
            float r2 = r;
            if (in >> r2) r = r2;
            set_load_point(off, r, touch, false);
        } else if (x == "touch") {
            std::string y;
            in >> y;
            set_load_point(off, r, y == "on", false);
        }
        load_point(off, &r, &touch);
        LoadDiag d;
        {
            std::lock_guard lock(g_zone_mutex);
            d = g_load_diag;
        }
        char b[640];
        std::snprintf(b, sizeof(b), "load point (%.3f %.3f %.3f): offset (%.3f %.3f %.3f) m, radius %.2f m, on touch %d | the round held: the wrist %.3f m, the round %.3f m from it "
                      "(round at (%.3f %.3f %.3f) axis (%.3f %.3f %.3f)) | inserts: on touch %llu, by the wrist %llu, let go %llu | closest since the grab: the round "
                      "%.3f m, the wrist %.3f m (the gun's ring %.3f m, can load %d)",
                      d.pt[0], d.pt[1], d.pt[2], off[0], off[1], off[2], r, touch ? 1 : 0, d.wrist_d, d.round_d, d.round[0], d.round[1], d.round[2], d.axis[0], d.axis[1], d.axis[2],
                      static_cast<unsigned long long>(d.inserts_touch), static_cast<unsigned long long>(d.inserts_wrist),
                      static_cast<unsigned long long>(d.inserts_let_go), d.min_round_d, d.min_wrist_d, d.ring_r, d.can_load);
        return b;
    }
    if (v == "anchor") {  // holster anchor [body|headset]: [Holsters] Anchor for the session; the last update's shift
        std::string x;
        in >> x;
        if (x == "body" || x == "headset") set_anchor(x == "headset" ? 1 : 0, false);
        if (x == "lean" || x == "hands") {  // holster anchor lean|hands on|off: LeanSteady, TurnByHands (the session only)
            std::string y;
            in >> y;
            if (x == "lean") set_lean_steady(y == "on", false);
            else set_turn_by_hands(y == "on", false);
        }
        if (x == "turn") {  // holster anchor turn on|off [dead zone]: the session only
            std::string y;
            float dz = -1.0f;
            in >> y >> dz;
            if (y == "on" || y == "off") set_turn_with_head(y == "on", false);
            if (dz >= 0.0f) set_turn_dead_zone(dz, false);
        }
        float sh[3], cp[3], ph[3];
        {
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(sh, g_anchor_shift, sizeof(sh));
            std::memcpy(cp, g_anchor_cam, sizeof(cp));
            std::memcpy(ph, g_place_hip, sizeof(ph));
        }
        float no[3] = {0, 0, 0};
        const bool nk = camera_lever::neck_offset(no);
        float hy = 0.0f;
        const bool hk = camera_lever::head_yaw_deg(&hy);
        char b[600];
        std::snprintf(b, sizeof(b), "anchor %s, used %d, shift (%.3f %.3f %.3f) | neck since recentre%s (%.3f %.3f %.3f) | camera (%.3f %.3f %.3f) | "
                      "turn %d, dead zone %.0f, the holsters' yaw %.1f, the head's%s %.1f deg | lean steady %d (the lean %.3f m), by hands %d (%.1f) | "
                      "places taken %u, the right hip %.3f right %.3f below %.3f ahead, waits %u, camera elsewhere %u",
                      g_anchor.load() ? "headset" : "body", g_anchor_used.load() ? 1 : 0, sh[0], sh[1], sh[2], nk ? "" : " (none)", no[0],
                      no[1], no[2], cp[0], cp[1], cp[2], g_turn_head.load() ? 1 : 0, g_turn_dead.load(), g_zone_yaw_pub.load(), hk ? "" : " (none)", hy,
                      g_lean_steady.load() ? 1 : 0, g_lean_pub.load(), g_turn_hands.load() ? 1 : 0, g_hands_yaw_pub.load(), g_place_takes.load(), ph[0],
                      -ph[1], ph[2], g_place_waits.load(), g_place_offcam.load());
        return b;
    }
    if (v == "gunturn") {  // holster gunturn <zone index> <tilt> <turn> <lean> [<right> <up> <forward>]: a shown gun's GunTurn/GunOffset (session)
        int zi = -1;
        float t[3] = {}, o[3] = {};
        in >> zi >> t[0] >> t[1] >> t[2];
        if (!(in >> o[0] >> o[1] >> o[2])) o[0] = o[1] = o[2] = 0.0f;
        if (zi < 0 || zi >= kZones) return "holster gunturn: no such zone";
        set_zone_gun(zi, t, o, false);
        float tn[3], go[3];
        zone_gun(zi, tn, go);
        char b[200];
        std::snprintf(b, sizeof(b), "gunturn %s: turn %.1f %.1f %.1f deg, offset %.3f %.3f %.3f m", g_zones[zi].key, tn[0], tn[1], tn[2], go[0],
                      go[1], go[2]);
        return b;
    }
    if (v == "guns") {  // holster guns [on|off|reset|back on|off]: [Holsters] ShowGuns (ShowBackGuns) for the session; each shown gun, its drawn error
        std::string x;
        in >> x;
        if (x == "on" || x == "off") set_show_guns(x == "on", false);
        if (x == "back") {
            std::string y;
            in >> y;
            if (y == "on" || y == "off") set_show_back_guns(y == "on", false);
        }
        std::lock_guard lock(g_zone_mutex);
        if (x == "reset") {
            for (float& e : g_show_diag.worst) e = 0.0f;
            for (uint64_t& e : g_show_diag.stale) e = 0;
            for (uint64_t& e : g_show_diag.over) e = 0;
        }
        static const char* const kName[4] = {"right hip", "back", "left hip", "left shoulder"};
        std::string o = std::string("guns shown ") + (g_show_guns.load() ? "1" : "0") + " back " + (g_show_back_guns.load() ? "1" : "0");
        char b[260];
        for (int i = 0; i < 4; ++i) {
            std::snprintf(b, sizeof(b), " | %s %d err %.4f worst %.4f stale %llu gated %llu over %llu at (%.3f %.3f %.3f)", kName[i], g_show_diag.weapon[i],
                          g_show_diag.err[i], g_show_diag.worst[i], static_cast<unsigned long long>(g_show_diag.stale[i]),
                          static_cast<unsigned long long>(g_show_diag.gated[i]), static_cast<unsigned long long>(g_show_diag.over[i]),
                          g_show_diag.rel[i][0], g_show_diag.rel[i][1], g_show_diag.rel[i][2]);
            o += b;
        }
        std::snprintf(b, sizeof(b), " | frames %llu | twins hidden %llu", static_cast<unsigned long long>(g_show_diag.frames),
                      static_cast<unsigned long long>(g_twin_hidden.load()));
        return o + b;
    }
    if (v == "gun") {  // holster gun: the gun in hand's adjustments in effect (round 13 item 8), its own or every gun's
        const int w = gun_in_hand();
        std::string sub;
        if (in >> sub) {  // (C8, the tests) holster gun set|clear <what 0-3> <hand 0|1> [x y z [r]]: the gun in hand's, the session only
            int what = -1, hand = 1;
            float o[3] = {}, r = 0.12f;
            in >> what >> hand >> o[0] >> o[1] >> o[2] >> r;
            if (w < 0 || what < 0 || what >= kAdjCount) return "ERROR holster gun set|clear <what 0-3> <hand 0|1> [x y z [r]]: no gun in hand, or what";
            if (sub == "set") set_gun_adjust(w, what, o, r, false, hand, true);
            else if (sub == "clear") {
                std::lock_guard lock(g_zone_mutex);
                g_gun_adj[w][hand == 0 ? 0 : 1].own[what] = false;
            }
        }
        body::BodyPoints gbp;
        const int gh = body::body_points(&gbp) ? gbp.gun : 1;  // (C8: the gun hand's set)
        std::string o = std::string("gun ") + std::to_string(w) + (gh == 0 ? " (left hand)" : " (right hand)");
        for (int what = 0; what < kAdjCount; ++what) {
            float off[3], r = 0;
            bool own = false;
            gun_adjust(w, what, off, &r, &own, gh);
            char b[120];
            std::snprintf(b, sizeof(b), " | %s (%.3f %.3f %.3f) r %.3f %s", what == 0 ? "foregrip" : what == 1 ? "ring" : what == 2 ? "load" : what == 3 ? "mag" : "part", off[0], off[1],
                          off[2], r, own ? "own" : "every");
            o += b;
        }
        return o;
    }
    if (v == "grab") {  // holster grab [<right> <up> <forward>]: [Hands] InteractOffset for the session; the spots
        float o[3];
        if (in >> o[0] >> o[1] >> o[2]) set_interact_offset(o, false);
        interact_offset(o);
        body::BodyPoints bp;
        char b[300];
        float g[2][3] = {};
        const bool ok = body::body_points(&bp);
        if (ok)
            for (int j = 0; j < 2; ++j) grab_point(bp, j, g[j]);
        std::snprintf(b, sizeof(b), "interaction spot (%.3f %.3f %.3f) | left spot (%.3f %.3f %.3f) wrist (%.3f %.3f %.3f) | right spot (%.3f %.3f %.3f) wrist (%.3f %.3f %.3f)",
                      o[0], o[1], o[2], g[0][0], g[0][1], g[0][2], bp.hand[0][0], bp.hand[0][1], bp.hand[0][2], g[1][0], g[1][1], g[1][2], bp.hand[1][0],
                      bp.hand[1][1], bp.hand[1][2]);
        return b;
    }
    if (v == "fore") {  // holster fore [set <right> <up> <forward> [radius] | snap on|off | trace [reset]] (the session only)
        std::string x;
        in >> x;
        if (x == "steady") {  // holster fore steady on|off: SteadyRing for the session (the learned grips, body.cpp)
            std::string y;
            in >> y;
            g_steady_ring = y != "off";
            return body::command(std::string("skel grips steadyring ") + (g_steady_ring.load() ? "on" : "off"));
        }
        if (x == "axes") {  // holster fore axes gun|controller: the ring offset's axes (RingInGunAxes, the session)
            std::string y;
            in >> y;
            g_ring_gun_axes = y != "controller";
            return std::string("the ring offset along the ") + (g_ring_gun_axes.load() ? "drawn gun's axes" : "controller's axes");
        }
        if (x == "trace") {  // the ring in the drawn gun's frame since the reset (run 8 item 2)
            std::string y;
            in >> y;
            std::lock_guard lock(g_fore_mutex);
            if (y == "reset") g_fore_trace = ForeTrace{};
            const ForeTrace& t = g_fore_trace;
            char b[400];
            std::snprintf(b, sizeof(b),
                          "fore trace: weapon %d, %llu frames, the ring in the gun's frame now (%.3f %.3f %.3f), range x %.1f y %.1f z %.1f mm, steps over "
                          "5 mm %llu (the largest %.1f mm) | the grip from %s",
                          t.weapon, static_cast<unsigned long long>(t.frames), t.last[0], t.last[1], t.last[2],
                          t.have ? (t.hi[0] - t.lo[0]) * 1000.0f : -1.0f, t.have ? (t.hi[1] - t.lo[1]) * 1000.0f : -1.0f,
                          t.have ? (t.hi[2] - t.lo[2]) * 1000.0f : -1.0f, static_cast<unsigned long long>(t.jumps), t.max_step * 1000.0f,
                          body::grip_source_name());
            return b;
        }
        float off[3], r = 0;
        foregrip(off, &r);
        if (x == "set") {
            in >> off[0] >> off[1] >> off[2];
            float r2 = r;
            if (in >> r2) r = r2;
            set_foregrip(off, r, false);
        } else if (x == "snap") {
            std::string y;
            in >> y;
            set_foregrip_snap(y == "on", false);
        } else if (x == "zone") {  // holster fore zone <right> <up> <forward> [radius]: the ring, apart from the grip
            float zo[3], zr = 0;
            foregrip_zone(zo, &zr);
            in >> zo[0] >> zo[1] >> zo[2];
            float r2 = zr;
            if (in >> r2) zr = r2;
            set_foregrip_zone(zo, zr, false);
        }
        body::BodyPoints bp;
        const bool have = body::body_points(&bp);
        float d = -1.0f;
        if (have && bp.fore_ok) {
            float dd = 0;
            for (int k = 0; k < 3; ++k) dd += (bp.hand[1 - bp.gun][k] - bp.fore[k]) * (bp.hand[1 - bp.gun][k] - bp.fore[k]);
            d = std::sqrt(dd);
        }
        float zo[3], zr = 0;
        foregrip_zone(zo, &zr);
        char b[400];
        std::snprintf(b, sizeof(b), "foregrip: offset (%.3f %.3f %.3f) radius %.2f snap %d | point %s (%.3f %.3f %.3f), the left hand %.3f m from it | "
                      "the ring: moved (%.3f %.3f %.3f) radius %.2f",
                      off[0], off[1], off[2], r, g_fore_snap.load() ? 1 : 0, have && bp.fore_ok ? "known" : "not known", have ? bp.fore[0] : 0.0f,
                      have ? bp.fore[1] : 0.0f, have ? bp.fore[2] : 0.0f, d, zo[0], zo[1], zo[2], zr);
        return b;
    }
    if (v == "rings") {  // holster rings [on|off|<name>]: the markers (the session only), each in the world and LOCAL; <name>: only those
        std::string x;
        in >> x;
        if (x == "on" || x == "off") set_show_zones_session(x == "on");
        if (x == "near" || x == "steady" || x == "place") {  // holster rings near|steady|place on|off (the session)
            std::string y;
            in >> y;
            if (x == "near") set_zones_near(y == "on", false);
            else if (x == "steady") set_steady_zones(y == "on", false);
            else set_placing(y == "on");
        }
        if (x == "dots" || x == "points") {  // holster rings dots|points on|off: the hand dots, the weapon points (the session)
            std::string y;
            in >> y;
            if (x == "dots") set_show_hand_dots(y == "on", false);
            else set_show_weapon_points(y == "on", false);
        }
        const std::string only =
            x == "on" || x == "off" || x == "dots" || x == "points" || x == "near" || x == "steady" || x == "place" ? std::string() : x;
        Markers mk;
        char sw[96];
        std::snprintf(sw, sizeof(sw), "(holsters %d, hand dots %d, weapon points %d, near %d, steady %d, placing %d, placed %llu) ",
                      g_show_zones.load() ? 1 : 0, g_show_dots.load() ? 1 : 0, g_show_points.load() ? 1 : 0, g_zones_near.load() ? 1 : 0,
                      g_steady_zones.load() ? 1 : 0, g_place.load() ? 1 : 0, static_cast<unsigned long long>(g_placed.load()));
        if (!markers(&mk))
            return std::string("rings ") + sw +
                   (g_show_zones.load() || g_show_dots.load() || g_show_points.load() || actions::show_fan_zone() ? "on, no markers yet" : "off");
        std::string r = std::string("rings ") + sw + "on, " + std::to_string(mk.n) + " markers:";
        static const char* const kState[5] = {"idle", "hand in", "held", "gun", "near"};
        for (int i = 0; i < mk.n; ++i) {
            const Marker& m = mk.m[i];
            float l[3] = {}, w2[3] = {};
            const bool lok = camera_lever::world_to_local(mk.cam, m.pos, l);
            float err = -1.0f;
            if (lok && camera_lever::local_to_world(mk.cam, l, nullptr, w2, nullptr))
                err = std::sqrt((w2[0] - m.pos[0]) * (w2[0] - m.pos[0]) + (w2[1] - m.pos[1]) * (w2[1] - m.pos[1]) + (w2[2] - m.pos[2]) * (w2[2] - m.pos[2]));
            const char* nm = m.id < kZones ? g_zones[m.id].key : m.id == kZones ? "Foregrip" : m.id == kZones + 1 ? "LoadPoint" : m.id == kZones + 2 ? "LeftHand"
                           : m.id == kZones + 3 ? "RightHand" : "ActionHint";
            if (!only.empty() && only != nm) continue;
            char b[200];
            std::snprintf(b, sizeof(b), " | %s %s r %.2f world (%.3f %.3f %.3f) local (%.3f %.3f %.3f) back %.5f", nm, kState[m.state < 5 ? m.state : 0], m.radius,
                          m.pos[0], m.pos[1], m.pos[2], l[0], l[1], l[2], err);
            r += b;
        }
        return r;
    }
    if (v == "turret") {  // holster turret (2026-10-13, the Gatling probe): the item in hand's firing object W: its eWeapon,
        // its game gun matrix (W +0x80, the barrel -row c) and its shoot-from matrix (W +0xd0; for a turret its barrels bone)
        std::string x;
        in >> x;
        if (x == "keep") {  // holster turret keep on|off: first person kept on a mounted gun (the session)
            std::string y;
            in >> y;
            pose::set_mounted_first_person(y == "on");
        } else if (x == "arms") {  // (round 141) holster turret arms free|game: the arms free with the grips open, or the game's always
            std::string y;
            in >> y;
            g_turret_arms_free = y != "game";
        }
        const uintptr_t actor = player_actor();
        uintptr_t wmgr = 0, item = 0, W = 0, info = 0;
        int16_t wt = -1;
        if (!actor || !rd(actor + 0x70, &wmgr) || !wmgr || !rd(wmgr + 0x80, &item) || !item || !rd(item + 0xa0, &W) || !W) return "turret: no item in hand";
        if (rd(W + 0x28, &info) && info) rd(info + 8, &wt);
        float g[16] = {}, f[16] = {};
        rd(W + 0x80, &g);
        rd(W + 0xd0, &f);
        char b[900];
        std::snprintf(b, sizeof(b),
                      "turret: weapon %d | gun a (%.3f %.3f %.3f) b (%.3f %.3f %.3f) c (%.3f %.3f %.3f) at (%.2f %.2f %.2f) | shoot-from a (%.3f %.3f "
                      "%.3f) b (%.3f %.3f %.3f) c (%.3f %.3f %.3f) at (%.2f %.2f %.2f) | hands: active %d, the left on the handle %d (taken %llu), the "
                      "right on the crank %d (turn %.1f rad/s, firing %d, fired %llu) | rounds %llu (asked one by one %llu, frames with the trigger held %llu), arms free %d",
                      wt, g[0], g[1], g[2], g[4], g[5], g[6], g[8], g[9], g[10], g[12], g[13], g[14], f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9],
                      f[10], f[12], f[13], f[14], g_turret.active ? 1 : 0, g_turret.l_held ? 1 : 0, static_cast<unsigned long long>(g_turret.grabs),
                      g_turret.r_held ? 1 : 0, std::fabs(g_turret.c_rate), g_turret.firing ? 1 : 0, static_cast<unsigned long long>(g_turret.cranks),
                      static_cast<unsigned long long>(aim::mounted_shots()), static_cast<unsigned long long>(g_turret.pulses),
                      static_cast<unsigned long long>(g_turret.holds), g_turret_arms_free.load() ? 1 : 0);
        return b;
    }
    if (v == "bases") {  // holster bases: each zone's base, the root and the right (the twins' mirror)
        HolsterDiag d;
        {
            std::lock_guard lock(g_hdiag_mutex);
            d = g_hdiag;
        }
        if (!d.valid) return "bases: not in first person";
        std::string o = "bases:";
        char b[96];
        for (int z = 0; z < kZones; ++z) {
            std::snprintf(b, sizeof(b), " %s (%.3f %.3f %.3f)", g_zones[z].key, d.base[z][0], d.base[z][1], d.base[z][2]);
            o += b;
        }
        std::snprintf(b, sizeof(b), " | root (%.3f %.3f %.3f) right (%.3f %.3f %.3f)", d.root[0], d.root[1], d.root[2], d.right[0], d.right[1], d.right[2]);
        return o + b;
    }
    if (v == "zones") {
        HolsterDiag d;
        {
            std::lock_guard lock(g_hdiag_mutex);
            d = g_hdiag;
        }
        if (!d.valid) return "zones: not in first person (or no body drawn)";
        char b[800];
        int k = std::snprintf(b, sizeof(b), "slot %d in hand %d | draws %llu puts %llu entries %llu rounds %llu/%llu clicks %llu two %llu (%.2f) |",
                              d.cur_slot, d.in_hand ? 1 : 0, static_cast<unsigned long long>(g_draws.load()),
                              static_cast<unsigned long long>(g_puts.load()), static_cast<unsigned long long>(g_entries.load()),
                              static_cast<unsigned long long>(g_rounds_in.load()), static_cast<unsigned long long>(g_rounds_taken.load()),
                              static_cast<unsigned long long>(g_clicks.load()), static_cast<unsigned long long>(g_two_grips.load()),
                              g_two_blend.load());
        for (int z = 0; z < kZones && k < static_cast<int>(sizeof(b)) - 80; ++z)
            k += std::snprintf(b + k, sizeof(b) - k, " %s (%.3f %.3f %.3f)%s", g_zones[z].key, d.zone[z][0], d.zone[z][1], d.zone[z][2],
                               d.zone_ok[z] ? "" : "?");
        for (int h = 0; h < 2 && k < static_cast<int>(sizeof(b)) - 60; ++h)
            k += std::snprintf(b + k, sizeof(b) - k, " | %s hand (%.3f %.3f %.3f) in %d%s", h ? "right" : "left", d.hand[h][0], d.hand[h][1],
                               d.hand[h][2], d.in_zone[h], g_consumed[h].load() ? " grip held" : "");
        if (d.cam_ok && k < static_cast<int>(sizeof(b)) - 50)
            std::snprintf(b + k, sizeof(b) - k, " | camera (%.3f %.3f %.3f)", d.cam[0], d.cam[1], d.cam[2]);
        return b;
    }
    if (v == "pieces") {  // holster pieces [place on|off]: run 12 item 3 (the session)
        std::string x, y;
        in >> x >> y;
        if (x == "place" && (y == "on" || y == "off")) set_placing_pieces(y == "on");
        if (x == "follow" && (y == "on" || y == "off")) set_rings_follow(y == "on", false);  // holster pieces follow on|off (the session)
        if (x == "belt" && (y == "on" || y == "off")) set_own_pieces_linked(y == "on", false);  // holster pieces belt on|off: as the menu's Customize
        if (x == "ringreset") reset_rings();  // holster pieces ringreset: as the menu's Reset the rings (saved)
        if (x == "set") {  // holster pieces set <piece key|Left> dx dy dz tilt turn lean: as the menu's arrows (the rings linked)
            body::Piece p;
            int pc = -1;
            for (int i = 0; i < body::kPieces; ++i)
                if (y == body::piece_key(i)) pc = i;
            if (y == "Left") pc = body::kPieces;
            if (pc >= 0 && (in >> p.d[0] >> p.d[1] >> p.d[2] >> p.turn[0] >> p.turn[1] >> p.turn[2])) set_piece_linked(pc, p, false);
        }
        return pieces_status() + (rings_follow() ? ", the rings follow" : ", the rings on their own");
    }
    if (v == "model") {  // holster model [on|off] [offset x y z turn yaw pitch roll]: the session only; the models' state
        std::string x;
        in >> x;
        if (x == "on" || x == "off") set_show_models(x == "on", false);
        float o[3], r[3];
        if (x == "pose" && (in >> o[0] >> o[1] >> o[2] >> r[0] >> r[1] >> r[2])) {  // the offset and ModelTurn (the raw test off)
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_model_off, o, sizeof(o));
            std::memcpy(g_model_rot, r, sizeof(r));
            g_model_raw_on = false;
        }
        std::string nm;
        if (x == "name" && (in >> nm) && nm.size() < sizeof(g_model_name)) {  // the session's model (run 11 item 5's stand-ins)
            std::lock_guard lock(g_zone_mutex);
            std::snprintf(g_model_name, sizeof(g_model_name), "%s", nm.c_str());
        }
        if (x == "raw" && (in >> r[0] >> r[1] >> r[2])) {  // the native's three angles as given (the axes' test)
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_model_raw, r, sizeof(r));
            g_model_raw_on = true;
        }
        float mo[3], mr[3];
        {
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(mo, g_model_off, sizeof(mo));
            std::memcpy(mr, g_model_rot, sizeof(mr));
        }
        char b[240];
        std::snprintf(b, sizeof(b), "models %d (%s), offset (%.3f %.3f %.3f) turn (%.0f %.0f %.0f), shown right %d left %d", g_show_models.load() ? 1 : 0,
                      g_model_name, mo[0], mo[1], mo[2], mr[0], mr[1], mr[2], held_prop::shown(held_prop::kModelSlot) ? 1 : 0,
                      held_prop::shown(held_prop::kModelSlot + 1) ? 1 : 0);
        return b;
    }
    if (v == "mirror") {  // holster mirror [on|off]: [Holsters] MirrorLeftHanded for the session (the user ini never written)
        std::string x;
        if (in >> x && g_mirror_lefty.exchange(x == "on") != (x == "on"))
            log::info("[holster] left-handed: the holsters %s (the session)", x == "on" ? "mirrored" : "where they are");
        return std::string("mirror ") + (g_mirror_lefty.load() ? "1" : "0") + ", left-handed " + (controls::left_handed() ? "1" : "0");
    }
    if (v == "hold") {  // holster hold [on|off] [letgo <ms>|letgo default]: [Holsters] HoldToKeep for the session (the user ini
        // never written; letgo: the let-go's debounce, 50-1000 ms, a test's aid, kHoldLetGoMs by default); each controller's
        // held gun (none, gun, second), its eWeapon and how long its grip has been let go (-1 held), then the counters
        std::string x;
        while (in >> x) {
            if (x == "on" || x == "off") {
                set_hold_to_keep(x == "on", false);
            } else if (x == "letgo") {
                std::string y;
                in >> y;
                const double ms = y == "default" ? kHoldLetGoMs : std::atof(y.c_str());
                if (ms >= 50.0 && ms <= 1000.0) g_hold_letgo = ms;
            } else if (x == "knifeswap") {  // knifeswap on|off: [Hands] KnifeSwap for the session
                std::string y;
                in >> y;
                g_knife_swap = y != "off";
            } else if (x == "items") {  // items on|off: [Holsters] HoldItems for the session
                std::string y;
                in >> y;
                g_hold_items = y != "off";
            } else if (x == "instantput") {  // instantput on|off: [Holsters] InstantPutAway for the session
                std::string y;
                in >> y;
                g_instant_put = y != "off";
            } else if (x == "subst") {  // subst on|off: a leaving item drawn at its holstered place (the test's; off)
                std::string y;
                in >> y;
                body::set_leave_subst(y == "on");
            }
        }
        static const char* const kKind[3] = {"none", "gun", "second"};
        const int k0 = g_keep_pub[0].load() & 3, k1 = g_keep_pub[1].load() & 3, pz = g_hold_put_zone.load();
        char b[1100];
        std::snprintf(b, sizeof(b),
                      "hold %d (active %d) letgo %.0f ms | L %s w %d up %.0f | R %s w %d up %.0f | keeping L %d R %d | takes %llu (adopted %llu) puts %llu "
                      "(last let go %.0f ms, back at %s) carries %llu second puts %llu | blips %llu (last %.0f ms) waits %llu loose %llu gone %llu | drawany %d | "
                      "items %d: thrown %llu, the game's throws %llu | instant put-away %d",
                      g_hold.load() ? 1 : 0, g_hold.load() && !wheel::wheel_mode() ? 1 : 0, g_hold_letgo.load(), kKind[k0 < 3 ? k0 : 0], g_keep_w_pub[0].load(),
                      g_keep_up_pub[0].load(), kKind[k1 < 3 ? k1 : 0], g_keep_w_pub[1].load(), g_keep_up_pub[1].load(), g_keeping[0].load() ? 1 : 0,
                      g_keeping[1].load() ? 1 : 0, static_cast<unsigned long long>(g_hold_takes.load()), static_cast<unsigned long long>(g_hold_adopts.load()),
                      static_cast<unsigned long long>(g_hold_puts.load()), g_hold_put_len.load(), pz >= 0 && pz < kZones ? g_zones[pz].key : "-",
                      static_cast<unsigned long long>(g_hold_carries.load()), static_cast<unsigned long long>(g_hold_sec_puts.load()),
                      static_cast<unsigned long long>(g_hold_blips.load()), g_hold_blip_ms.load(), static_cast<unsigned long long>(g_hold_waits.load()),
                      static_cast<unsigned long long>(g_hold_loose.load()), static_cast<unsigned long long>(g_hold_gone.load()),
                      controls::draw_to_grabbing_hand() ? 1 : 0, g_hold_items.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_hold_item_throws.load()), static_cast<unsigned long long>(g_hold_item_game_throws.load()), g_instant_put.load() ? 1 : 0);
        return std::string(b) + " | " + body::put_away_leaving_status();
    }
    if (v == "instant") {  // the draw's mode for this session (not saved)
        std::string x;
        in >> x;
        g_instant = x != "off";
    }
    if (v == "wheelhere") {  // (round 143) holster wheelhere on|off: [Holsters] WheelAtHolster for the session; the zones now
        std::string x;
        in >> x;
        if (x == "on" || x == "off") set_wheel_at_holster(x == "on", false);
        return "wheel at a holster " + std::string(g_wheel_hol.load() ? "on" : "off") + ": the zones of the controllers' empty hands " +
               std::to_string(g_wheel_zone[0].load()) + " " + std::to_string(g_wheel_zone[1].load()) + ", assigned " +
               std::to_string(g_wheel_assigned.load()) + ", lost " + std::to_string(g_wheel_assign_lost.load()) + ", drawn by LT " +
               std::to_string(g_wheel_lt_draws.load());
    }
    if (v == "on" || v == "off") set_keeps_drawn(v == "on");
    char b[400];
    std::snprintf(b, sizeof(b), "keep=%d hooked=%d gun_state=%d topped=%llu draw_time=%.2f player_triggers=%llu instant=%d | raised %d "
                  "(pitch %.1f reach %.2f ahead %.2f, conditions %d of 31, raises %llu), fire-ready %d",
                  g_keep.load() ? 1 : 0, o_player_weapon ? 1 : 0, gun_state(g_player_ped.load()),
                  static_cast<unsigned long long>(g_topped.load()), g_reset_seen.load(),
                  static_cast<unsigned long long>(g_player_triggers.load()), g_instant.load() ? 1 : 0, g_raised.load() ? 1 : 0,
                  g_raise_dbg[0].load(), g_raise_dbg[1].load(), g_raise_dbg[2].load(), static_cast<int>(g_raise_dbg[3].load()),
                  static_cast<unsigned long long>(g_raises.load()), g_fire_ready.load() ? 1 : 0);
    return b;
}

bool knife_swap() { return g_knife_swap.load(std::memory_order_relaxed); }
void set_knife_swap(bool on) {
    if (g_knife_swap.exchange(on) != on) log::info("[holster] the off hand's knife beside a gun the game's weapon (KnifeSwap): %s", on ? "on" : "off");
    config::set("Hands", "KnifeSwap", on ? "1" : "0");
}
}  // namespace rdrvr::holster
