#include "core/held_prop.h"

#include <excpt.h>  // (EXCEPTION_EXECUTE_HANDLER; not windows.h: its near and far macros)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/api.h"
#include "core/log.h"

namespace rdrvr::held_prop {
namespace {

constexpr uint32_t kRequestAsset = 0x9AA02DA7;    // REQUEST_ASSET(name, ASSET_TYPE_Prop = 0) -> asset id
constexpr uint32_t kStreamProp = 0x38DC1F50;      // STREAMING_REQUEST_PROP(id, 1)
constexpr uint32_t kPropLoaded = 0xD7F80035;      // STREAMING_IS_PROP_LOADED(id)
constexpr uint32_t kCreateLayout = 0x6CA53214;    // CREATE_LAYOUT(name) -> layout
constexpr uint32_t kCreateProp = 0xE351587D;      // CREATE_PROP_IN_LAYOUT(layout, name, fragment, xy, z, rxy, rz, frozen)
constexpr uint32_t kSetPosition = 0xC5D796F8;     // SET_OBJECT_POSITION(object, xy, z)
constexpr uint32_t kSetOrientation = 0xC8A4EE74;  // SET_OBJECT_ORIENTATION(object, xy, z): Euler degrees
constexpr uint32_t kCollideWorld = 0x601FC9F4;    // SET_OBJECT_COLLIDE_WITH_WORLD(object, 0)
constexpr uint32_t kCollideMovables = 0x05D69EA6; // SET_OBJECT_COLLIDE_WITH_MOVABLES(object, 0)
constexpr uint32_t kIsValid = 0xD7E7187B;         // IS_OBJECT_VALID(object)
constexpr uint32_t kDestroy = 0x21144994;         // DESTROY_OBJECT(object)
const char kLayoutName[] = "rdrvr_held_props";
const char* const kObjName[kSlots] = {"rdrvr_round",     "rdrvr_second_gun", "rdrvr_holster_0", "rdrvr_holster_1",
                                      "rdrvr_holster_2", "rdrvr_holster_3",  "rdrvr_model_0",   "rdrvr_model_1",
                                      "rdrvr_census",    "rdrvr_belt_0",     "rdrvr_belt_1",    "rdrvr_belt_2",
                                      "rdrvr_belt_3",    "rdrvr_belt_4",     "rdrvr_belt_5",    "rdrvr_belt_6",
                                      "rdrvr_shell_0",   "rdrvr_shell_1",    "rdrvr_shell_2",   "rdrvr_shell_3",
                                      "rdrvr_shell_4",   "rdrvr_shell_5",    "rdrvr_shell_6",   "rdrvr_shell_7",
                                      "rdrvr_shell_8",   "rdrvr_shell_9",    "rdrvr_shell_10",  "rdrvr_shell_11",
                                      "rdrvr_shell_12",  "rdrvr_shell_13",   "rdrvr_free_30",   "rdrvr_free_31"};
static_assert(kPoolFirst + kPoolMax <= kSlots, "the pool's slots");
bool in_pool(int i) { return i >= kPoolFirst && i < kPoolFirst + kPoolMax; }
// the slots hogtie.cpp's clear takes away: 2026-11-04 every one but the census's, as optimumbox's (the user, 0.8.12 on
// the other PC: "Hogtie is broken again"): the holsters' guns and models, the shells' pool (the row at the chest), the
// round, the off hand's item, and the dual wield's copy (its draw then moved onto the free hand, body.cpp: still seen)
bool clearable(int i) { return i != kCensusSlot; }

uint64_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
uint64_t v2(float x, float y) { return fbits(x) | (fbits(y) << 32); }
uint64_t ptr(const char* s) { return reinterpret_cast<uint64_t>(s); }

enum State { kNone, kRequesting, kStreaming, kLoaded, kCreating, kLive, kDestroying, kChecking };
// run 10 item 2: eight puts kept (four left a walking prop's own draws 3-30 cm from all of them: cycle D4). 2026-10-12 (the
// user: a second gun at the right hip facing backwards when turning toward it, gone with ShowGuns off): turning, the
// holster guns' own draws trailed past the eight (r16_backwards5: ~20000 draws gated in four turns, none standing) and,
// gated: r16_backwards6 found them not the prop's own (those matched throughout) but the game's own holstered gun, the
// same model, 0.34-0.38 m from the puts (body.cpp now hides that one while a holster shows its gun). The count kept at 8;
// "props puts <n>" (1-64) for a session
constexpr int kPuts = 64;  // the storage; g_puts_n of them used
std::atomic<int> g_puts_n{8};
std::atomic<bool> g_clear{false};  // set_clear (hogtie.cpp: someone on the lasso)
struct Slot {
    // wanted (any thread, under g_mutex)
    const char* want = nullptr;
    float pose[12] = {};
    bool angled = false;  // want_angles: the object turned with each move
    float angles[3] = {};
    // the game thread's
    State st = kNone;
    const char* frag = nullptr;  // what is being loaded or held
    uint32_t asset = 0;
    uint32_t obj = 0;
    uint64_t pending = 0;  // the request awaited
    double since = 0;      // when it was queued
    int polls = 0;
    int fails = 0;          // creations that gave no object (retried a second apart, 10 at most)
    bool gave_up = false;   // until it is no longer wanted
    double checked = 0;  // the live object's last validity check
    uint64_t check = 0;  // that check's request
    // the draw hook's (under g_mutex)
    float put[3] = {};      // the object's position as last queued
    float puts[kPuts][3] = {};  // the last kPuts queued (the game draws the object where a script tick put it)
    int nput = 0;
    uint64_t gated = 0;     // draws of its drawable beyond the gate (the nearest put farther than 1.5 m)
    float gated_d = 0.0f;   // the last one's distance
    float rel[3] = {};      // its last draw's position less the root that draw used (the body-relative place)
    bool put_ok = false;
    uintptr_t drawable = 0;  // learned from its draws
    // run 10 item 2 (the user: "guns in holsters jitter when moving including npc guns"): the drawable is the model's,
    // shared by every gun of that model (an NPC's Cattleman too), so its own instance is learned from its first matched
    // draw and a draw of another instance is refused (an NPC's gun within the gate was moved onto John's holster)
    uintptr_t inst = 0;
    uint64_t far = 0;        // matched draws over 0.3 m from every recent put (before the instance check: an NPC's gun)
    uint64_t near_n = 0, mid_n = 0;  // matched draws within 3 cm of a recent put, 3 cm - 0.3 m
    uint64_t other = 0;      // draws of its drawable by another instance, refused
    uint64_t relearned = 0;  // its instance taken again (a draw of a new one within 3 cm of a put: the game remade it)
    double shown_ms = 0;     // its last moved draw
    float anchor[3] = {};    // the body's root its pose was placed by (want's anchor)
    bool anchored = false;
    float drawn[3] = {};     // its last drawn position
    // 2026-10-27, the shells' pool (want_pool): its object put at `putp` (its pose's place, or the spare's, a few cm off
    // along its own direction), not at the pose; a spare's draws hidden
    bool own_put = false;
    float putp[3] = {};
    bool hide = false;
    uint64_t hidden = 0;     // its draws hidden (a spare)
    // (the review of 2026-10-27) a pool object's puts start once it exists (the creation's answer), not at the request:
    // before it, a draw of anything (the gun in hand) within 3 cm of the put could be learned as its model. Its model is
    // learned within 2 s of it being placed, else not at all (learn_off: no more draws taken by position, the per-draw
    // filter back to its lock-free path; round_draw's stand-in shells cover its poses)
    float create_at[3] = {};
    double placed_ms = 0;
    bool learn_off = false;
};
float g_root_now[3] = {};  // the body's root of the frame being drawn (note_root; under g_mutex)
bool g_root_ok = false;
std::mutex g_mutex;
Slot g_slot[kSlots];
struct Shown {  // the status line's view of the game thread's state (under g_mutex)
    const char* frag = nullptr;
    uint32_t obj = 0;
    int st = 0;
};
Shown g_shown[kSlots];
uint32_t g_layout = 0;
uint64_t g_layout_req = 0;
std::atomic<int> g_live{0};
std::atomic<bool> g_any{false};  // a slot with an object placed: the draw hook's fast way out (it runs for every rigid draw)
std::atomic<uintptr_t> g_known[kPoolFirst] = {};  // each placed slot's learned drawable (0: none), the pool's apart:
std::atomic<uintptr_t> g_pool_known{0};          // (2026-10-27) its one model's (the per-draw filter scans 17, not 32)
std::atomic<bool> g_learning{false};          // a placed slot whose drawable is not learned yet (matched by position)
std::atomic<uint64_t> g_made{0}, g_destroyed{0}, g_failed{0}, g_matched{0}, g_learned{0};
std::atomic<bool> g_diag{false};  // "props diag"
struct DiagSlot {
    uint64_t own[4] = {};    // draws of its learned model within 0.5 m of its recent puts, by pass (0, 1, 2, 3 and on)
    uint64_t other[4] = {};  // draws of any other model there
    uintptr_t other_d = 0;   // the last other one's drawable
    float other_dist = 0.0f;
    uintptr_t near_d = 0;    // the nearest other one's drawable (2026-10-09: a model whose draws are never learned)
    float near_dist = 1e9f;
    float near_m[12] = {};   // its record's axes (rows 0-2 of the 4x4) and position
};
DiagSlot g_diag_slot[kSlots];  // under g_mutex
constexpr int kMoves = 256;  // run 11: 16 slots (the room check leaves 3 a slot for the creations); 2026-10-27: 32 (the pool)
uint64_t g_moves[kMoves] = {};  // requests whose results are not needed, collected (an uncollected one stays in the map)
int g_nmoves = 0;
void later(uint64_t id) {
    if (g_nmoves < kMoves) g_moves[g_nmoves++] = id;
}

uint64_t q(uint32_t h, std::initializer_list<uint64_t> a) {
    uint64_t args[12] = {};
    int n = 0;
    for (uint64_t v : a) args[n++] = v;
    return api::queue_native(h, args, static_cast<uint32_t>(n), 0, nullptr);
}

void collect_moves() {
    RdrvrNativeResult r;
    int k = 0;
    for (int i = 0; i < g_nmoves; ++i)
        if (!api::wait_native(g_moves[i], &r, 0)) g_moves[k++] = g_moves[i];
    g_nmoves = k;
}

void push_put(Slot& s, const float* p) {  // under g_mutex: the queued position, also into the last four
    std::memcpy(s.put, p, sizeof(s.put));
    std::memcpy(s.puts[s.nput % kPuts], p, sizeof(s.put));
    ++s.nput;
}
float put_d2(const Slot& s, const float* pos) {  // the squared distance to the nearest of the recent puts
    const int use = std::clamp(g_puts_n.load(std::memory_order_relaxed), 1, kPuts);
    const int n = s.nput < use ? s.nput : use;
    float best = 1e30f;
    for (int i = 0; i < (n ? n : 1); ++i) {
        const float* q = n ? s.puts[(s.nput - 1 - i + kPuts * 64) % kPuts] : s.put;  // the newest `use`
        const float dx = pos[0] - q[0], dy = pos[1] - q[1], dz = pos[2] - q[2];
        best = std::fmin(best, dx * dx + dy * dy + dz * dz);
    }
    return best;
}
void update_any() {  // under g_mutex
    bool any = false, learning = false;
    uintptr_t pool = 0;
    for (int i = 0; i < kSlots; ++i) {
        const Slot& t = g_slot[i];
        const bool placed = t.put_ok && t.want;
        any = any || placed;
        learning = learning || (placed && !t.drawable && !t.learn_off);
        if (i < kPoolFirst) g_known[i].store(placed ? t.drawable : 0, std::memory_order_relaxed);
        else if (placed && t.drawable) pool = t.drawable;
    }
    g_pool_known.store(pool, std::memory_order_relaxed);
    g_learning.store(learning, std::memory_order_relaxed);
    g_any.store(any, std::memory_order_relaxed);
}
bool known(uintptr_t drawable) {  // the lock-free filter: a placed slot's learned drawable
    bool k = g_pool_known.load(std::memory_order_relaxed) == drawable;
    for (const auto& x : g_known) k = k || x.load(std::memory_order_relaxed) == drawable;
    return k;
}
// the pool's (2026-10-27): its model's asset once any of its slots has loaded it (the others take it, not each its
// own request and 2 s of polls), the pose list's slots (pose k drawn by slot g_pool_slot[k]), how many poses
std::atomic<uint32_t> g_pool_asset{0};
std::atomic<const char*> g_pool_frag{nullptr};  // (written by the game thread)
// (the review of 2026-10-27) the pool's creations' failures, one count for all its slots (not 10 each, a new layout each:
// 14 slots made 140 tries and as many layouts): a second between tries, off after 3 until the next shotgun is drawn, at
// most 2 new layouts a draw (the game thread's; reset by want_pool when a shotgun is drawn again). The mod has no path
// that destroys a layout (no native for it is used anywhere), so an abandoned one is left to the game
std::atomic<int> g_pool_fails{0};
int g_pool_layouts = 0;
double g_pool_retry_ms = 0;
std::atomic<bool> g_pool_off{false};
std::atomic<uint64_t> g_pool_refused{0};  // pool draws refused as the model by its box (not the shell's)
// The shell's own box (research/run11/props.md: 0.020 x 0.047 x 0.019 m, its length along y; the drawable's +0x30 min
// and +0x40 max), checked before a drawable is learned as the pool's: only a shell-sized model is taken (within 30%),
// so the gun in hand or a holstered gun cannot be. The read is SEH-guarded (a game pointer the draw itself uses)
bool pool_shape_ok(uintptr_t drawable) {
    float lo[3] = {}, hi[3] = {};
    __try {
        std::memcpy(lo, reinterpret_cast<const void*>(drawable + 0x30), sizeof(lo));
        std::memcpy(hi, reinterpret_cast<const void*>(drawable + 0x40), sizeof(hi));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const float want[3] = {0.020f, 0.047f, 0.019f};
    for (int k = 0; k < 3; ++k) {
        const float d = hi[k] - lo[k];
        if (!(d > want[k] * 0.7f && d < want[k] * 1.3f)) return false;
    }
    return true;
}
int g_pool_slot[kPoolMax] = {};     // under g_mutex
int g_pool_n = 0, g_pool_objects = 0, g_pool_free = 0;

void forget(Slot& s) {
    std::lock_guard lock(g_mutex);
    s.put_ok = false;
    s.drawable = 0;
    update_any();
}

}  // namespace

void publish_status();

void want(int s, const char* fragment, const float* pose, const float* anchor) {
    if (s < 0 || s >= kSlots) return;
    std::lock_guard lock(g_mutex);
    g_slot[s].want = fragment;
    if (fragment && pose) std::memcpy(g_slot[s].pose, pose, sizeof(g_slot[s].pose));
    g_slot[s].anchored = fragment && anchor;
    if (g_slot[s].anchored) std::memcpy(g_slot[s].anchor, anchor, sizeof(g_slot[s].anchor));
    update_any();
}

void want_angles(int s, const float* deg) {
    if (s < 0 || s >= kSlots) return;
    std::lock_guard lock(g_mutex);
    g_slot[s].angled = deg != nullptr;
    if (deg) std::memcpy(g_slot[s].angles, deg, sizeof(g_slot[s].angles));
}

std::atomic<float> g_root_speed{0.0f};  // the body's speed (m/s, smoothed): the gate widens with it (the review's)
void note_root(const float* root) {
    std::lock_guard lock(g_mutex);
    static double last_ms = 0.0;
    const double now = log::now_ms();
    if (g_root_ok && now > last_ms && now - last_ms < 500.0) {
        const float dx = root[0] - g_root_now[0], dy = root[1] - g_root_now[1], dz = root[2] - g_root_now[2];
        const float sp = std::sqrt(dx * dx + dy * dy + dz * dz) / static_cast<float>((now - last_ms) * 0.001);
        if (std::isfinite(sp)) g_root_speed = g_root_speed.load() + (std::fmin(sp, 30.0f) - g_root_speed.load()) * 0.2f;
    }
    last_ms = now;
    std::memcpy(g_root_now, root, sizeof(g_root_now));
    g_root_ok = true;
}

bool last_put(int s, float pos[3]) {
    if (s < 0 || s >= kSlots) return false;
    std::lock_guard lock(g_mutex);
    if (!g_slot[s].put_ok) return false;
    std::memcpy(pos, g_slot[s].put, sizeof(g_slot[s].put));
    return true;
}
bool last_drawn(int s, float pos[3], double* ms, float rel[3], uint64_t* gated) {
    if (s < 0 || s >= kSlots) return false;
    std::lock_guard lock(g_mutex);
    if (gated) *gated = g_slot[s].gated;
    if (g_slot[s].shown_ms <= 0) return false;
    std::memcpy(pos, g_slot[s].drawn, sizeof(g_slot[s].drawn));
    if (rel) std::memcpy(rel, g_slot[s].rel, sizeof(g_slot[s].rel));
    if (ms) *ms = g_slot[s].shown_ms;
    return true;
}

void frame(uint32_t actor) {
    (void)actor;
    collect_moves();
    const double now = log::now_ms();
    RdrvrNativeResult r{};
    if (g_layout_req && api::wait_native(g_layout_req, &r, 0)) {
        g_layout = static_cast<uint32_t>(r.value);
        g_layout_req = 0;
        log::info("[prop] the layout for held props: %u", g_layout);
    }
    bool pool_created = false;  // (2026-10-27: one pool object made a frame at most, not 14 at once)
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slot[i];
        const char* want;
        float pose[12], ang[3], at[3];
        bool angled;
        {
            std::lock_guard lock(g_mutex);
            // (hogtie.cpp's clear: clearable's slots)
            want = g_clear.load(std::memory_order_relaxed) && clearable(i) ? nullptr : s.want;
            std::memcpy(pose, s.pose, sizeof(pose));
            angled = s.angled;
            std::memcpy(ang, s.angles, sizeof(ang));
            std::memcpy(at, s.own_put ? s.putp : s.pose + 9, sizeof(at));  // where the object is put
        }
        if (s.pending && !api::wait_native(s.pending, &r, 0)) {
            if (now - s.since > 5000.0) {  // no answer: given up, the object (if any) left to the destroy path
                log::warn("[prop] slot %d: no answer in state %d; given up%s", i, static_cast<int>(s.st),
                          s.st == kCreating ? " (an object may have been made unseen; a new layout)" : "");
                if (s.st == kCreating && in_pool(i)) {
                    ++g_pool_fails;
                    g_pool_retry_ms = now + 1000.0;
                    if (g_pool_layouts < 2) ++g_pool_layouts, g_layout = 0;
                    // (the re-review: a creation never answered counts toward the pool's cutoff too, or it retries forever)
                    if (g_pool_fails >= 3 && !g_pool_off.exchange(true))
                        log::warn("[prop] slot %d: the pool's creations failed %d times (no answer); off until the next shotgun is drawn "
                                  "(the mod's own shells stand in)", i, g_pool_fails.load());
                } else if (s.st == kCreating) {
                    g_layout = 0;
                }
                s.pending = 0;
                g_failed.fetch_add(1, std::memory_order_relaxed);
                s.st = s.obj ? kLive : kNone;
            }
            continue;
        }
        const bool answered = s.pending != 0;
        s.pending = 0;
        auto send = [&](uint64_t id) {
            s.pending = id;
            s.since = now;
        };
        // a different model wanted, or none: the held object goes first
        if (s.obj && (want != s.frag) && s.st == kLive && !s.check) {
            send(q(kDestroy, {s.obj}));
            s.st = kDestroying;
            forget(s);
            continue;
        }
        switch (s.st) {
            case kNone:
                if (want && in_pool(i)) {  // a pool object: the pool's asset once one has it; one slot requests it
                    if (g_pool_asset.load(std::memory_order_relaxed) && g_pool_frag == want) {
                        s.frag = want;
                        s.asset = g_pool_asset.load(std::memory_order_relaxed);
                        s.gave_up = false;
                        s.fails = 0;
                        s.st = kLoaded;
                        break;
                    }
                    bool other = false;
                    for (int j = kPoolFirst; j < kPoolFirst + kPoolMax; ++j)
                        other = other || (j != i && g_slot[j].frag == want && (g_slot[j].st == kRequesting || g_slot[j].st == kStreaming));
                    if (other) break;
                }
                if (want) {
                    s.frag = want;
                    send(q(kRequestAsset, {ptr(want), 0}));
                    s.st = kRequesting;
                }
                break;
            case kRequesting:
                if (!answered) break;
                s.asset = static_cast<uint32_t>(r.value);
                if (!s.asset) {
                    log::warn("[prop] %s: REQUEST_ASSET gave no id", s.frag);
                    g_failed.fetch_add(1, std::memory_order_relaxed);
                    s.st = kNone;
                    s.frag = nullptr;
                    break;
                }
                later(q(kStreamProp, {s.asset, 1}));
                s.polls = 0;
                s.st = kStreaming;
                s.since = now;
                break;
            case kStreaming:
                if (answered) {
                    if (r.value & 0xff) {
                        s.st = kLoaded;
                        log::info("[prop] %s loaded (asset %u, %d polls)", s.frag, s.asset, s.polls);
                        break;
                    }
                    if (++s.polls >= 10) {  // 2 s: the shotgun shell is never reported loaded, yet it is made (run 5)
                        log::info("[prop] %s not reported loaded after %d polls: made anyway", s.frag, s.polls);
                        s.st = kLoaded;
                        break;
                    }
                }
                if (now - s.since >= 200.0) send(q(kPropLoaded, {s.asset}));
                break;
            case kLoaded:  // loaded once a session; made whenever wanted
                if (in_pool(i) && s.asset && s.frag) {  // (the pool's others take it from here)
                    g_pool_asset.store(s.asset, std::memory_order_relaxed);
                    g_pool_frag = s.frag;
                }
                if (want != s.frag) {
                    s.gave_up = false;
                    s.fails = 0;
                    if (!want) break;
                    s.st = kNone;  // another model: load it
                    break;
                }
                if (s.gave_up || (s.fails && now - s.since < 1000.0)) break;
                if (in_pool(i) && (pool_created || g_pool_off.load(std::memory_order_relaxed) || now < g_pool_retry_ms)) break;
                if (!g_layout) {
                    if (!g_layout_req) g_layout_req = q(kCreateLayout, {ptr(kLayoutName)});
                    break;
                }
                send(q(kCreateProp, {g_layout, ptr(kObjName[i]), ptr(s.frag), v2(at[0], at[1]), fbits(at[2]), v2(0, 0), fbits(0), 1}));
                s.st = kCreating;
                pool_created = pool_created || in_pool(i);
                if (in_pool(i)) {  // (placed once it exists: kCreating's answer)
                    std::memcpy(s.create_at, at, sizeof(s.create_at));
                    break;
                }
                {
                    std::lock_guard lock(g_mutex);
                    s.nput = 0;
                    push_put(s, at);
                    s.put_ok = true;
                    s.drawable = 0;
                    update_any();
                }
                break;
            case kCreating:
                if (!answered) break;
                s.obj = static_cast<uint32_t>(r.value);
                if (!s.obj) {
                    forget(s);
                    s.st = kLoaded;
                    s.since = now;
                    // the game drops script layouts in a long session (run 5's soak: every creation in the old layout then
                    // gave 0, a new layout's worked): a new layout for the retry
                    if (in_pool(i)) {  // the pool's one count and backoff
                        ++g_pool_fails;
                        g_pool_retry_ms = now + 1000.0;
                        const bool fresh = g_pool_layouts < 2;
                        log::info("[prop] %s: no object made in layout %u (the pool's failure %d)%s", s.frag, g_layout, g_pool_fails.load(),
                                  fresh ? "; a new layout" : "");
                        if (fresh) ++g_pool_layouts, g_layout = 0;
                        if (g_pool_fails >= 3 && !g_pool_off.exchange(true))
                            log::warn("[prop] %s: the pool's creations failed %d times; off until the next shotgun is drawn (the mod's own shells "
                                      "stand in)", s.frag, g_pool_fails.load());
                        break;
                    }
                    log::info("[prop] %s: no object made in layout %u; a new layout", s.frag, g_layout);
                    g_layout = 0;
                    if (++s.fails >= 10) {
                        log::warn("[prop] %s: CREATE_PROP_IN_LAYOUT gave no object %d times; given up while it is wanted", s.frag, s.fails);
                        g_failed.fetch_add(1, std::memory_order_relaxed);
                        s.gave_up = true;
                    }
                    break;
                }
                s.fails = 0;
                if (in_pool(i)) {  // now it exists: placed where it was made, its model the pool's (if learned) or learned within 2 s
                    std::lock_guard lock(g_mutex);
                    s.nput = 0;
                    push_put(s, s.create_at);
                    s.put_ok = true;
                    s.placed_ms = now;
                    s.learn_off = false;
                    s.drawable = g_pool_known.load(std::memory_order_relaxed);
                    update_any();
                }
                g_made.fetch_add(1, std::memory_order_relaxed);
                g_live.fetch_add(1, std::memory_order_relaxed);
                later(q(kCollideWorld, {s.obj, 0}));
                later(q(kCollideMovables, {s.obj, 0}));
                s.checked = now;
                log::info("[prop] %s made: object %u (slot %d)", s.frag, s.obj, i);
                s.st = kLive;
                break;
            case kLive:
                if (in_pool(i)) {
                    std::lock_guard lock(g_mutex);
                    if (s.put_ok && !s.drawable && !s.learn_off && now - s.placed_ms > 2000.0) {
                        s.learn_off = true;
                        update_any();
                        log::warn("[prop] %s: object %u's draws not learned in 2 s (slot %d): its poses stand in", s.frag, s.obj, i);
                    }
                }
                if (s.check) {  // the game may remove an object itself (streaming, a cutscene): never used after
                    if (!api::wait_native(s.check, &r, 0)) {
                        if (now - s.checked > 5000.0) s.check = 0;
                        break;
                    }
                    s.check = 0;
                    if (!(r.value & 0xff)) {
                        log::warn("[prop] %s: object %u no longer valid (removed by the game); forgotten", s.frag, s.obj);
                        s.obj = 0;
                        g_live.fetch_sub(1, std::memory_order_relaxed);
                        forget(s);
                        s.st = kLoaded;
                        break;
                    }
                }
                if (now - s.checked > 1000.0) {
                    s.checked = now;
                    s.check = q(kIsValid, {s.obj});
                    break;  // no move until it is known valid
                }
                if (g_nmoves < kMoves - 3 * kSlots) {  // kept at the drawn hand: the game draws (and culls) it there (room left for creations)
                    later(q(kSetPosition, {s.obj, v2(at[0], at[1]), fbits(at[2])}));
                    if (angled) later(q(kSetOrientation, {s.obj, v2(ang[0], ang[1]), fbits(ang[2])}));
                    std::lock_guard lock(g_mutex);
                    push_put(s, at);
                    s.put_ok = true;
                    update_any();
                }
                break;
            case kDestroying:
                if (!answered) break;
                send(q(kIsValid, {s.obj}));  // checked gone
                s.st = kChecking;
                break;
            case kChecking:
                if (!answered) break;
                g_destroyed.fetch_add(1, std::memory_order_relaxed);
                g_live.fetch_sub(1, std::memory_order_relaxed);
                if (r.value & 0xff) log::warn("[prop] %s: object %u still valid after DESTROY_OBJECT", s.frag, s.obj);
                else log::info("[prop] %s destroyed: object %u (slot %d), checked gone", s.frag, s.obj, i);
                s.obj = 0;
                s.st = kLoaded;  // the model stays loaded
                break;
        }
    }
    publish_status();
}

std::atomic<bool> g_inst_check{true};  // "props inst on|off": the instance check (the control: off, as before)
// run 10 item 2, measured (cycle D3): the draws carry no instance for the props (rec[0x13] empty), and with a stranger's
// Schofield 1 m away John's right-hip slot matched ~4700 draws over 0.3 m from its puts in 7 s. The game draws the prop
// where a script tick put it, so its own draws sit at one of its last four puts: the gate is that distance
// ([Holsters] PropGate, "props gate <m>"; 0.30 m with eight puts kept, against the old 1.5 m: 0.10 m left 1 in 9 of a
// walking prop's own draws unmatched, cycle D4)
std::atomic<float> g_gate{0.30f};
void set_puts(int n) {
    n = std::clamp(n, 1, kPuts);
    if (g_puts_n.exchange(n) != n) log::info("[prop] the draws matched against the last %d puts", n);
}
void set_gate(float m) {
    if (m >= 0.01f && m <= 2.0f && g_gate.exchange(m) != m) log::info("[prop] the draws' gate %.2f m from the recent puts", m);
}
void set_inst_check(bool on) {
    if (g_inst_check.exchange(on) != on) log::info("[prop] the draws' instance check %s", on ? "on" : "off");
}

std::atomic<uint64_t> g_pool_hidden{0};
bool match(uintptr_t drawable, uintptr_t inst, const float* pos, float* pose, int* slot, bool* hide) {
    if (hide) *hide = false;
    if (!g_any.load(std::memory_order_relaxed) || !drawable) return false;
    // every placed slot's drawable known: a draw of none of them needs no lock
    if (!g_learning.load(std::memory_order_relaxed) && !known(drawable)) return false;
    std::lock_guard lock(g_mutex);
    // of the slots with this drawable (or none learned yet and put within 3 cm), the one put nearest, within 1.5 m of
    // one of its last four puts (the game draws the object where a script tick put it: walking, that trails the latest)
    Slot* best = nullptr;
    // the gate, widened by the body's speed: the prop is drawn where a script tick put it, which trails further the faster
    // John goes (0.3 m standing and walking; galloping at 8 m/s about 1.3 m), still far less than the old 1.5 m at rest
    const float gate = g_gate.load(std::memory_order_relaxed) + g_root_speed.load(std::memory_order_relaxed) * 0.125f;
    float bd = gate * gate;
    const bool by_inst = g_inst_check.load(std::memory_order_relaxed);
    for (Slot& s : g_slot) {
        if (!s.put_ok || !s.want) continue;
        // (cycle r174, the pool's perf: mode 2 cost about 0.16 ms of scene render) a slot whose model is learned and is
        // not this draw's, or that learns no more, needs no distance: of the 32 slots only the draw's own model's and the
        // learning ones are measured (the shells' 14 for a shell's draw, none of them for a gun's)
        const bool same = drawable == s.drawable;
        if (!same && (s.drawable || s.learn_off)) continue;
        const float d2 = put_d2(s, pos);
        bool ok = same || d2 < 0.03f * 0.03f;
        if (ok && by_inst && s.inst && inst && inst != s.inst) {
            if (d2 < 0.03f * 0.03f) {  // a new instance at the put itself: the game remade the object
                s.inst = inst;
                ++s.relearned;
            } else {
                ++s.other;
                ok = false;
                continue;
            }
        }
        if (ok && d2 < bd) {
            best = &s, bd = d2;
            if (d2 < 1e-10f) break;  // (at a put itself: no other can be nearer)
        } else if (drawable == s.drawable) {
            ++s.gated;
            s.gated_d = std::sqrt(d2);
        }
    }
    if (!best) return false;
    const int bi = static_cast<int>(best - g_slot);
    if (!best->drawable && in_pool(bi) && !pool_shape_ok(drawable)) {  // not the shell's box: not its model
        if (g_pool_refused.fetch_add(1, std::memory_order_relaxed) == 0)
            log::warn("[prop] a draw by the pool's put refused as its model (drawable %llx: not the shell's box)", static_cast<unsigned long long>(drawable));
        return false;
    }
    if (!best->drawable) {
        best->drawable = drawable;
        if (in_pool(bi))  // (2026-10-27: one model, one drawable: the pool's others know it at once)
            for (int j = kPoolFirst; j < kPoolFirst + kPoolMax; ++j)
                if (g_slot[j].put_ok && g_slot[j].want && !g_slot[j].drawable && !g_slot[j].learn_off) g_slot[j].drawable = drawable;
        g_learned.fetch_add(1, std::memory_order_relaxed);
        update_any();  // the lock-free filter knows it now
    }
    if (best->hide) {  // a pool object with no pose to draw (a spare): its draw hidden by the caller
        if (!hide) return false;
        *hide = true;
        ++best->hidden;
        g_pool_hidden.fetch_add(1, std::memory_order_relaxed);
        if (slot) *slot = bi;
        return true;
    }
    if (!best->inst && inst) best->inst = inst;
    if (bd > 0.3f * 0.3f) ++best->far;
    else if (bd > 0.03f * 0.03f) ++best->mid_n;
    else ++best->near_n;
    std::memcpy(pose, best->pose, sizeof(best->pose));
    if (best->anchored && g_root_ok)  // the body's walk since the pose was placed (the frame end before)
        for (int k = 0; k < 3; ++k) pose[9 + k] += g_root_now[k] - best->anchor[k];
    std::memcpy(best->drawn, pose + 9, sizeof(best->drawn));
    for (int k = 0; k < 3; ++k) best->rel[k] = pose[9 + k] - (g_root_ok ? g_root_now[k] : 0.0f);
    if (slot) *slot = bi;
    best->shown_ms = log::now_ms();
    g_matched.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool diag_on() { return g_diag.load(std::memory_order_relaxed); }

std::atomic<bool> g_twins_on{true};  // "props twins on|off": the belt's twin hidden (the session; the perf test's control)
void set_twins(bool on) { g_twins_on = on; }
bool shown_at_holster(uintptr_t drawable) {
    if (!drawable || !g_any.load(std::memory_order_relaxed) || !g_twins_on.load(std::memory_order_relaxed) ||
        g_clear.load(std::memory_order_relaxed))
        return false;
    if (!known(drawable)) return false;  // the lock-free filter first (match's): most draws are of no prop's model
    std::lock_guard lock(g_mutex);
    for (int i = kHolsterSlot; i < kHolsterSlot + 4 && i < kSlots; ++i)
        if (g_slot[i].want && g_slot[i].drawable == drawable) return true;
    // ([Hands] OffHandItems) the off hand's item: its model's own at John's belt (the knife at its sheath) hidden too
    return g_slot[kOffhandSlot].want && g_slot[kOffhandSlot].drawable == drawable;
}

void diag_draw(uintptr_t drawable, const float* pos, uint64_t pass, const float* m) {
    if (!g_any.load(std::memory_order_relaxed)) return;
    const int p = pass < 3 ? static_cast<int>(pass) : 3;
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kSlots; ++i) {
        const Slot& s = g_slot[i];
        if (!s.put_ok || !s.want) continue;
        const float d2 = put_d2(s, pos);
        if (d2 > 0.5f * 0.5f) continue;
        DiagSlot& d = g_diag_slot[i];
        if (drawable == s.drawable) {
            ++d.own[p];
        } else {
            ++d.other[p];
            d.other_d = drawable;
            d.other_dist = std::sqrt(d2);
            if (d.other_dist < d.near_dist) {
                d.near_dist = d.other_dist;
                d.near_d = drawable;
                if (m) {
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) d.near_m[r * 3 + c] = m[r * 4 + c];
                    for (int k = 0; k < 3; ++k) d.near_m[9 + k] = pos[k];
                }
            }
        }
    }
}

std::string diag_command(const std::string& arg) {
    if (arg == "on" || arg == "off") g_diag = arg == "on";
    if (arg.rfind("near", 0) == 0) {  // props diag near<slot>: that slot's nearest other draw's record axes and position
        const int i = std::atoi(arg.c_str() + 4);
        if (i < 0 || i >= kSlots) return "no slot";
        std::lock_guard lock(g_mutex);
        const DiagSlot& d = g_diag_slot[i];
        char b[300];
        std::snprintf(b, sizeof(b), "slot %d nearest %llx at %.3f: x (%.3f %.3f %.3f) y (%.3f %.3f %.3f) z (%.3f %.3f %.3f) at (%.3f %.3f %.3f)", i,
                      static_cast<unsigned long long>(d.near_d & 0xffffff), d.near_dist, d.near_m[0], d.near_m[1], d.near_m[2], d.near_m[3],
                      d.near_m[4], d.near_m[5], d.near_m[6], d.near_m[7], d.near_m[8], d.near_m[9], d.near_m[10], d.near_m[11]);
        return b;
    }
    std::lock_guard lock(g_mutex);
    if (arg == "on" || arg == "reset")
        for (DiagSlot& d : g_diag_slot) d = DiagSlot{};
    std::string o = std::string("props diag ") + (g_diag.load() ? "on" : "off");
    char b[200];
    for (int i = 0; i < kSlots; ++i) {
        if (!g_slot[i].want) continue;
        const DiagSlot& d = g_diag_slot[i];
        std::snprintf(b, sizeof(b), " | slot %d own %llu/%llu/%llu/%llu other %llu/%llu/%llu/%llu (%llx at %.2f, nearest %llx at %.3f)", i,
                      static_cast<unsigned long long>(d.own[0]), static_cast<unsigned long long>(d.own[1]), static_cast<unsigned long long>(d.own[2]),
                      static_cast<unsigned long long>(d.own[3]), static_cast<unsigned long long>(d.other[0]), static_cast<unsigned long long>(d.other[1]),
                      static_cast<unsigned long long>(d.other[2]), static_cast<unsigned long long>(d.other[3]),
                      static_cast<unsigned long long>(d.other_d & 0xffffff), d.other_dist, static_cast<unsigned long long>(d.near_d & 0xffffff),
                      d.near_dist);
        o += b;
    }
    return o;
}

void set_clear(bool on) { g_clear = on; }
bool holster_wanted() {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kSlots; ++i)
        if (clearable(i) && g_slot[i].want) return true;
    return false;
}

// 2026-10-27: the pool's wants. The live objects take the poses first, in slot order (a pose is drawn by an object
// whose draws arrive now), the others the rest (none drawn yet: round_draw's stand-in covers them) and then the spares.
// Each is put at its pose's place, or the spare's at `base`, moved 3 cm along its own of 14 directions (the cube's
// corners and its faces' centres: any two 2.8 cm apart or more), so a draw (at one of its object's recent puts) has
// its own object's put nearest even where two poses' places nearly meet. Its puts follow the poses (the game culls
// it there), so a row's shells at the chest are not lost while the hand is out of sight
void want_pool(const char* fragment, int objects, const float* base, const float* poses, int n, int n_free, const float* anchor) {
    static const float kDir[kPoolMax][3] = {{0.577f, 0.577f, 0.577f},   {-0.577f, 0.577f, 0.577f},  {0.577f, -0.577f, 0.577f},
                                            {-0.577f, -0.577f, 0.577f}, {0.577f, 0.577f, -0.577f},  {-0.577f, 0.577f, -0.577f},
                                            {0.577f, -0.577f, -0.577f}, {-0.577f, -0.577f, -0.577f}, {1, 0, 0}, {-1, 0, 0},
                                            {0, 1, 0},                  {0, -1, 0},                  {0, 0, 1}, {0, 0, -1}};
    static const char* s_prev = nullptr;  // (the game thread's: a new shotgun drawn ends the last one's backoff)
    std::lock_guard lock(g_mutex);
    if (fragment && !s_prev) {
        g_pool_fails = 0;
        g_pool_layouts = 0;
        g_pool_retry_ms = 0.0;
        for (int j = 0; j < kPoolMax; ++j) g_slot[kPoolFirst + j].learn_off = false;
        if (g_pool_off.exchange(false)) log::info("[prop] the pool on again (a shotgun drawn)");
    }
    s_prev = fragment;
    objects = fragment && !g_pool_off.load(std::memory_order_relaxed) ? std::clamp(objects, 0, kPoolMax) : 0;
    n = std::clamp(n, 0, objects);
    // an object whose model was not learned in its 2 s is let go (destroyed: its draws are not moved or hidden, so it
    // would stand loose at its put) until the next shotgun draw
    auto usable = [](int j) { return !g_slot[kPoolFirst + j].learn_off; };
    auto live = [](int j) { return g_slot[kPoolFirst + j].st == kLive && g_slot[kPoolFirst + j].obj; };
    int order[kPoolMax], m = 0;
    for (int j = 0; j < objects; ++j)  // (the game thread's state: this thread's)
        if (usable(j) && live(j)) order[m++] = j;
    for (int j = 0; j < objects; ++j)
        if (usable(j) && !live(j)) order[m++] = j;
    n = std::min(n, m);
    n_free = std::clamp(n_free, 0, n);
    for (int j = 0; j < objects; ++j)
        if (!usable(j)) {
            g_slot[kPoolFirst + j].want = nullptr;
            g_slot[kPoolFirst + j].hide = true;
        }
    for (int k = 0; k < m; ++k) {
        const int j = order[k];
        Slot& s = g_slot[kPoolFirst + j];
        s.want = fragment;
        s.own_put = true;
        s.angled = false;
        s.hide = k >= n;
        const float* at = base;
        if (!s.hide) {
            std::memcpy(s.pose, poses + k * 12, sizeof(s.pose));
            s.anchored = k >= n_free && anchor;
            if (s.anchored) std::memcpy(s.anchor, anchor, sizeof(s.anchor));
            at = poses + k * 12 + 9;
        } else {
            s.anchored = false;
        }
        for (int c = 0; c < 3; ++c) s.putp[c] = (at ? at[c] : 0.0f) + 0.03f * kDir[j][c];
        g_pool_slot[k] = kPoolFirst + j;
    }
    for (int j = objects; j < kPoolMax; ++j) {
        Slot& s = g_slot[kPoolFirst + j];
        s.want = nullptr;
        s.hide = true;
        s.anchored = false;
    }
    g_pool_n = n;
    g_pool_objects = objects;
    g_pool_free = n_free;
    update_any();
}

bool pool_shown(int k) {
    if (k < 0 || k >= kPoolMax || !g_any.load(std::memory_order_relaxed) || g_clear.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_mutex);
    if (k >= g_pool_n) return false;
    const Slot& s = g_slot[g_pool_slot[k]];
    return s.want && !s.hide && log::now_ms() - s.shown_ms < 60.0;
}

int live_objects() { return g_live.load(std::memory_order_relaxed); }

void publish_status() {  // the game thread, the end of each frame()
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kSlots; ++i) g_shown[i] = {g_slot[i].frag, g_slot[i].obj, static_cast<int>(g_slot[i].st)};
}

bool shown(int s) {
    if (s < 0 || s >= kSlots || !g_any.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_mutex);
    if (g_clear.load(std::memory_order_relaxed) && clearable(s)) return false;
    return g_slot[s].want && log::now_ms() - g_slot[s].shown_ms < 100.0;
}

std::string status() {
    char b[400];
    std::lock_guard lock(g_mutex);
    std::snprintf(b, sizeof(b), "props: live %d, made %llu, destroyed %llu, failed %llu, draws matched %llu (learned %llu)", g_live.load(),
                  static_cast<unsigned long long>(g_made.load()), static_cast<unsigned long long>(g_destroyed.load()),
                  static_cast<unsigned long long>(g_failed.load()), static_cast<unsigned long long>(g_matched.load()),
                  static_cast<unsigned long long>(g_learned.load()));
    std::string o = b;
    {  // 2026-10-27: the shells' pool, one line (its slots below only while wanted or holding an object)
        int live = 0, shown_n = 0, wanted = 0;
        uint64_t hidden = 0, near_n = 0, mid_n = 0;
        const double now = log::now_ms();
        for (int j = kPoolFirst; j < kPoolFirst + kPoolMax; ++j) {
            const Slot& s = g_slot[j];
            live += g_shown[j].obj ? 1 : 0;
            wanted += s.want ? 1 : 0;
            shown_n += s.want && !s.hide && now - s.shown_ms < 60.0 ? 1 : 0;
            hidden += s.hidden;
            near_n += s.near_n;
            mid_n += s.mid_n;
        }
        int unlearned = 0;
        for (int j = kPoolFirst; j < kPoolFirst + kPoolMax; ++j) unlearned += g_slot[j].learn_off ? 1 : 0;
        std::snprintf(b, sizeof(b), " | pool %s: objects %d (wanted %d, live %d), poses %d (by the body %d), shown %d, spares' draws hidden %llu (now %llu), "
                      "matched near %llu mid %llu, failures %d%s, refused by box %llu, not learned %d", g_pool_frag.load() ? g_pool_frag.load() : "-", g_pool_objects,
                      wanted, live, g_pool_n, g_pool_n - g_pool_free, shown_n, static_cast<unsigned long long>(hidden),
                      static_cast<unsigned long long>(g_pool_hidden.load()), static_cast<unsigned long long>(near_n), static_cast<unsigned long long>(mid_n),
                      g_pool_fails.load(), g_pool_off.load() ? " (off)" : "", static_cast<unsigned long long>(g_pool_refused.load()), unlearned);
        o += b;
    }
    for (int i = 0; i < kSlots; ++i) {
        if (i >= kPoolFirst && !g_slot[i].want && !g_shown[i].obj) continue;
        std::snprintf(b, sizeof(b), " | slot %d %s obj %u state %d", i, g_shown[i].frag ? g_shown[i].frag : "-", g_shown[i].obj, g_shown[i].st);
        o += b;
        const Slot& s = g_slot[i];
        if (s.drawable) {
            std::snprintf(b, sizeof(b), " inst %llx far %llu other %llu relearned %llu near %llu mid %llu gated %llu (the last %.2f m off) drawable %llx", static_cast<unsigned long long>(s.inst),
                          static_cast<unsigned long long>(s.far), static_cast<unsigned long long>(s.other),
                          static_cast<unsigned long long>(s.relearned), static_cast<unsigned long long>(s.near_n),
                          static_cast<unsigned long long>(s.mid_n), static_cast<unsigned long long>(s.gated), s.gated_d,
                          static_cast<unsigned long long>(s.drawable));
            o += b;
        }
    }
    return o;
}

}  // namespace rdrvr::held_prop
