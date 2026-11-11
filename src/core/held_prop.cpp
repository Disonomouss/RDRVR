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
    // (round 103; the user: the Cattleman copy "lags behind and does not catch up until you move out of the affected area"
    // at the General Store; their logs: another drawable drawn 0.000 m from the copy's puts, as often as its own) the game
    // draws the object from another copy of its model at times (item 10's: a model is loaded more than once): a draw of
    // another copy, the same shape, at a put itself (within kAltGate) taken as its own (else left where a script tick put
    // it: behind the hand). Each such draw is checked so (the review: a copy is never trusted by its address, which may be
    // freed and reused; nor beyond the put, where NPCs' and the store's guns of that copy are); `alt` only the last seen
    uintptr_t alt = 0;
    uint64_t alt_n = 0, alt_draws = 0;
    // (round 104; the user: the holstered guns "In the holster while inside, it is always lagging slightly") indoors the
    // pre-pass draws the props before the frame's body: their anchor's move was last frame's, a frame behind walking. The
    // early draws counted, and the first of a frame against that frame's first main draw (mm)
    uint64_t early_n = 0;
    float early_at[3] = {};
    uint64_t early_seq = 0;  // the root's sequence it was drawn under (0: none waiting)
    uint64_t early_cmp = 0;
    double early_sum = 0.0;
    float early_max = 0.0f;
    // run 10 item 2 (the user: "guns in holsters jitter when moving including npc guns"): the drawable is the model's,
    // shared by every gun of that model (an NPC's Cattleman too), so its own instance is learned from its first matched
    // draw and a draw of another instance is refused (an NPC's gun within the gate was moved onto John's holster)
    uintptr_t inst = 0;
    uint64_t far = 0;        // matched draws over 0.3 m from every recent put (before the instance check: an NPC's gun)
    uint64_t near_n = 0, mid_n = 0;  // matched draws within 3 cm of a recent put, 3 cm - 0.3 m
    uint64_t other = 0;      // draws of its drawable by another instance, refused
    uint64_t other_near = 0; // (GOAL-cattleman-belts item 1, the review) of those, within the gate of its puts (not an NPC's gun afar)
    uint64_t relearned = 0;  // its instance taken again (a draw of a new one within 3 cm of a put: the game remade it)
    uint64_t strict_refused = 0;  // (round 109) a holster gun's: draws of its model refused off its puts and taken by none, hidden
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
float g_root_step[3] = {};  // (round 104) its move from the frame before (zero after a jump or a gap)
uint64_t g_root_seq = 0;    // note_root's count
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
// (round 103) the learned slots' recent puts (not the pool's), for the lock-free check of another copy's draw at one:
// each slot's box first, then its points (written under g_mutex by publish_puts; a torn read only misleads this filter)
constexpr int kPutPts = 128;
std::atomic<float> g_put_pt[kPutPts][3] = {};
struct PutGroup {
    std::atomic<float> box[6] = {};
    std::atomic<int> first{0}, n{0};
};
PutGroup g_put_grp[kPoolFirst];
std::atomic<int> g_put_grps{0};
void publish_puts();  // (below)
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
    float own_m[9] = {};     // (GOAL-cattleman-belts item 1) the last draw of its own model there, as the game drew it
    bool own_ok = false;
};
// ("props diag refuse<slot>|refuseoff", the test's) that slot's draws never matched: drawn as the game has them, as the user's
// copy's were (another instance refused, or an interior's draw)
std::atomic<int> g_refuse_slot{-1};
DiagSlot g_diag_slot[kSlots];  // under g_mutex
constexpr int kMoves = 256;  // run 11: 16 slots (the room check leaves 3 a slot for the creations); 2026-10-27: 32 (the pool)
uint64_t g_moves[kMoves] = {};  // requests whose results are not needed, collected (an uncollected one stays in the map)
int g_nmoves = 0;
// (GOAL-cattleman-belts item 1, the user: the Cattleman copy drawn turned after walking out of a building and back in,
// "only if you are looking in certain directions"; its shots went where the hand pointed) a slot without its own angles
// (want_angles) is turned by its pose too, not only placed: a draw of its object the hook does not move (refused as
// another instance, an interior's draw) shows the pose's turn, not the object's creation one (a world-fixed turn).
// SET_OBJECT_ORIENTATION's angles (cycle EU1, r151: an exact fit, 0.0003): R = Rz(z) Ry(y) Rx(x), the model's axes R's
// columns, so x = atan2(y axis's z, z axis's z), y = asin(-x axis's z), z = atan2(x axis's y, x axis's x) (degrees)
std::atomic<bool> g_auto_turn{true};  // ("props turn on|off": the test's)
void pose_angles(const float* pose, float* deg) {
    // (the review) y by atan2 (well conditioned near +-90 deg, a row not quite unit no matter); at the pole (the x axis
    // straight up or down) z is 0 and the turn is all x
    const float h = std::hypot(pose[0], pose[1]);
    deg[1] = std::atan2(-pose[2], h) * 57.2957795f;
    if (h < 1e-5f) {
        deg[2] = 0.0f;
        deg[0] = std::atan2(pose[2] < 0.0f ? pose[3] : -pose[3], pose[4]) * 57.2957795f;
    } else {
        deg[0] = std::atan2(pose[5], pose[8]) * 57.2957795f;
        deg[2] = std::atan2(pose[1], pose[0]) * 57.2957795f;
    }
}
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
    publish_puts();
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
    publish_puts();
}
bool known(uintptr_t drawable) {  // the lock-free filter: a placed slot's learned drawable
    bool k = g_pool_known.load(std::memory_order_relaxed) == drawable;
    for (const auto& x : g_known) k = k || x.load(std::memory_order_relaxed) == drawable;
    return k;
}
constexpr float kAltGate = 0.005f;  // (round 103) another copy's draw: within 5 mm of a put (the object's own sit at one exactly)
bool near_a_put(const float* pos) {  // the lock-free check: within kAltGate of a learned slot's recent put
    const int ng = g_put_grps.load(std::memory_order_acquire);
    for (int g = 0; g < ng; ++g) {
        const PutGroup& pg = g_put_grp[g];
        bool in = true;
        for (int k = 0; k < 3 && in; ++k)
            in = pos[k] >= pg.box[k].load(std::memory_order_relaxed) && pos[k] <= pg.box[3 + k].load(std::memory_order_relaxed);
        if (!in) continue;
        const int f = pg.first.load(std::memory_order_relaxed), n = pg.n.load(std::memory_order_relaxed);
        for (int i = f; i < f + n && i < kPutPts; ++i) {
            const float dx = pos[0] - g_put_pt[i][0].load(std::memory_order_relaxed), dy = pos[1] - g_put_pt[i][1].load(std::memory_order_relaxed),
                        dz = pos[2] - g_put_pt[i][2].load(std::memory_order_relaxed);
            if (dx * dx + dy * dy + dz * dz < kAltGate * kAltGate) return true;
        }
    }
    return false;
}
void publish_puts() {  // under g_mutex: the learned slots' recent puts (as put_d2 uses them) for near_a_put
    const int use = std::clamp(g_puts_n.load(std::memory_order_relaxed), 1, 8);  // (at most 8 a slot: 16 slots fit kPutPts)
    int n = 0, ng = 0;
    for (int i = 0; i < kPoolFirst; ++i) {
        const Slot& t = g_slot[i];
        if (!t.put_ok || !t.want || !t.drawable) continue;
        const int m = t.nput < use ? t.nput : use;
        if (!m) continue;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        PutGroup& pg = g_put_grp[ng++];
        pg.first.store(n, std::memory_order_relaxed);
        for (int j = 0; j < m && n < kPutPts; ++j, ++n) {
            const float* q = t.puts[(t.nput - 1 - j + kPuts * 64) % kPuts];
            for (int k = 0; k < 3; ++k) {
                g_put_pt[n][k].store(q[k], std::memory_order_relaxed);
                lo[k] = std::fmin(lo[k], q[k]);
                hi[k] = std::fmax(hi[k], q[k]);
            }
        }
        pg.n.store(n - pg.first.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (int k = 0; k < 3; ++k) {
            pg.box[k].store(lo[k] - kAltGate, std::memory_order_relaxed);
            pg.box[3 + k].store(hi[k] + kAltGate, std::memory_order_relaxed);
        }
    }
    g_put_grps.store(ng, std::memory_order_release);
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
bool read_box(uintptr_t drawable, float* lo, float* hi) {  // the drawable's +0x30 min and +0x40 max, SEH-guarded
    __try {
        std::memcpy(lo, reinterpret_cast<const void*>(drawable + 0x30), 3 * sizeof(float));
        std::memcpy(hi, reinterpret_cast<const void*>(drawable + 0x40), 3 * sizeof(float));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
bool pool_shape_ok(uintptr_t drawable) {
    float lo[3] = {}, hi[3] = {};
    if (!read_box(drawable, lo, hi)) return false;
    const float want[3] = {0.020f, 0.047f, 0.019f};
    for (int k = 0; k < 3; ++k) {
        const float d = hi[k] - lo[k];
        if (!(d > want[k] * 0.7f && d < want[k] * 1.3f)) return false;
    }
    return true;
}
int g_pool_slot[kPoolMax] = {};     // under g_mutex
int g_pool_n = 0, g_pool_objects = 0, g_pool_free = 0;

// (round 115) the models whose CREATE_PROP_IN_LAYOUT faulted inside the game (api::faulted, or answered with the call's
// own layout): never made again this session (the fault may leave a lock of the game's taken: each try risks it again)
constexpr int kFaultedModels = 8;
char g_faulted_model[kFaultedModels][64] = {};
int g_faulted_models = 0;
bool model_faulted(const char* frag) {
    for (int k = 0; k < g_faulted_models; ++k)
        if (frag && std::strcmp(g_faulted_model[k], frag) == 0) return true;
    return false;
}
bool note_model_faulted(const char* frag) {  // false: the list full (the caller's name wait then never ends: the review)
    if (!frag || model_faulted(frag)) return true;
    if (g_faulted_models >= kFaultedModels) return false;
    std::snprintf(g_faulted_model[g_faulted_models++], sizeof(g_faulted_model[0]), "%s", frag);
    return true;
}

// (GOAL-doorway-and-poker item 4.3, the review: a name the game refuses was asked again every round trip, warned each
// time, while it was wanted; a refused creation's 10 tries began anew at every want) per name: a refused request waits
// 0.5 s, doubling to 30 s, warned once; given-up creations wait 30 s; an asset or a made object clears it. The names
// copied (the wanted name's text can change in place). The round trip's only
struct NameWait {
    char name[96] = {};
    int fails = 0;
    double next_ms = 0.0;
    bool create = false;  // (the review, 3) the creations' give-up: an asset id does not clear it, a made object does
};
constexpr int kNameWaits = 8;
NameWait g_name_wait[kNameWaits];
std::atomic<uint64_t> g_name_waited{0};  // round trips a wanted name waited
NameWait* name_wait(const char* n, bool add) {
    if (!n) return nullptr;
    for (NameWait& w : g_name_wait)
        if (w.name[0] && std::strcmp(w.name, n) == 0) return &w;
    if (!add) return nullptr;
    NameWait* e = &g_name_wait[0];
    for (NameWait& w : g_name_wait) {
        if (!w.name[0]) {
            e = &w;
            break;
        }
        if (w.next_ms < e->next_ms) e = &w;  // (all taken: the one whose wait ends first)
    }
    *e = NameWait{};
    std::snprintf(e->name, sizeof(e->name), "%s", n);
    return e;
}
bool name_waits(const char* n, double now) {
    const NameWait* w = name_wait(n, false);
    if (!w || now >= w->next_ms) return false;
    g_name_waited.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void name_clear(const char* n, bool made = false) {
    NameWait* w = name_wait(n, false);
    if (w && (made || !w->create)) *w = NameWait{};
}
void name_never(const char* n) {  // (the review, 1) a fault not on the faulted list: never asked again this session
    if (NameWait* w = name_wait(n, true)) {
        w->fails = 99;
        w->next_ms = 1e300;
    }
}

void forget(Slot& s) {
    std::lock_guard lock(g_mutex);
    s.put_ok = false;
    s.drawable = 0;
    s.alt = 0;
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

float put_distance(int s, const float* pos) {  // (item 4.3) m from the nearest of slot s's recent puts; -1: none
    if (s < 0 || s >= kSlots || !pos) return -1.0f;
    std::lock_guard lock(g_mutex);
    const Slot& t = g_slot[s];
    if (!t.put_ok || !t.nput) return -1.0f;
    return std::sqrt(put_d2(t, pos));
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
    // (round 104) the step from the frame before, for the next frame's early draws: none after a gap (a pause, a load)
    // or a jump (a teleport, a cutscene's cut)
    const float sx = root[0] - g_root_now[0], sy = root[1] - g_root_now[1], sz = root[2] - g_root_now[2];
    const bool step_ok = g_root_ok && now > last_ms && now - last_ms < 100.0 && sx * sx + sy * sy + sz * sz < 0.25f * 0.25f;
    g_root_step[0] = step_ok ? sx : 0.0f, g_root_step[1] = step_ok ? sy : 0.0f, g_root_step[2] = step_ok ? sz : 0.0f;
    ++g_root_seq;
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
bool released(int s) {
    if (s < 0 || s >= kSlots) return false;
    std::lock_guard lock(g_mutex);
    return !g_slot[s].want && g_shown[s].obj != 0;  // (g_shown: the game thread's view at its last frame's end)
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
            if (!angled && !in_pool(i) && g_auto_turn.load(std::memory_order_relaxed)) {  // (item 1) turned by its pose
                pose_angles(pose, ang);
                angled = std::isfinite(ang[0] + ang[1] + ang[2]);
            }
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
        const bool faulted = answered && api::faulted(s.pending);  // (round 115) its 0 no answer of the game's
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
                if (want && !model_faulted(want) && !name_waits(want, now)) {  // (item 4.3)
                    s.frag = want;
                    send(q(kRequestAsset, {ptr(want), 0}));
                    s.st = kRequesting;
                }
                break;
            case kRequesting:
                if (!answered) break;
                s.asset = static_cast<uint32_t>(r.value);
                if (!s.asset) {
                    if (faulted) {  // (item 4.3, the review) round 115's mark for a request too: never asked again this session
                        log::error("[prop] %s: REQUEST_ASSET failed in the game (a fault); this model not asked again this session", s.frag);
                        if (!note_model_faulted(s.frag)) name_never(s.frag);
                    } else if (NameWait* w = name_wait(s.frag, true)) {  // (item 4.3) asked again after 0.5 s, doubling to 30 s
                        if (w->fails && now - w->next_ms > 60000.0) w->fails = 0;  // (the review, 2) a minute past its wait: anew
                        ++w->fails;
                        w->next_ms = now + std::fmin(30000.0, 500.0 * static_cast<double>(1u << std::min(w->fails - 1, 6)));
                        if (w->fails == 1)
                            log::warn("[prop] %s: REQUEST_ASSET gave no id; asked again after 0.5 s, the wait doubling to 30 s (warned once)",
                                      s.frag);
                    }
                    g_failed.fetch_add(1, std::memory_order_relaxed);
                    s.st = kNone;
                    s.frag = nullptr;
                    break;
                }
                name_clear(s.frag);
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
                if (!s.gave_up && model_faulted(s.frag)) s.gave_up = true;  // (round 115) its creation faulted before
                if (s.gave_up || (s.fails && now - s.since < 1000.0) || name_waits(s.frag, now)) break;  // (item 4.3: the given-up's 30 s)
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
                    s.alt = 0;
                    update_any();
                }
                break;
            case kCreating:
                if (!answered) break;
                s.obj = static_cast<uint32_t>(r.value);
                if (faulted || (s.obj && s.obj == g_layout)) {  // (round 115) the call faulted in the game (the api's mark; or
                    // its own first argument back, RedHook's answer to a fault: the layout then destroyed as this object)
                    log::error("[prop] %s: CREATE_PROP_IN_LAYOUT failed in the game (%s); this model not made again this session", s.frag,
                               faulted ? "a fault" : "answered with its layout");
                    if (!note_model_faulted(s.frag)) name_never(s.frag);  // (item 4.3's review: the list full)
                    s.obj = 0;
                    forget(s);
                    s.st = kLoaded;
                    s.since = now;
                    s.gave_up = true;
                    g_failed.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
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
                        log::warn("[prop] %s: CREATE_PROP_IN_LAYOUT gave no object %d times; given up while it is wanted (and 30 s after)",
                                  s.frag, s.fails);
                        g_failed.fetch_add(1, std::memory_order_relaxed);
                        s.gave_up = true;
                        if (NameWait* w = name_wait(s.frag, true)) {  // (item 4.3) a new want of it waits too
                            ++w->fails;
                            w->next_ms = now + 30000.0;
                            w->create = true;
                        }
                    }
                    break;
                }
                s.fails = 0;
                name_clear(s.frag, true);  // (item 4.3)
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
    // (2026-11-04, GOAL-next-items item 10; the user: "Every time I load in to the game, there is another gun on top on the
    // gun in the holster, it goes away after unholstering the gun"; not in the simulator) 8 s after the holster guns are
    // made: 3 s of "props diag" (each holster slot's own draws and the other draws near its place, their drawable and
    // distance), said in the log; again after all of them were gone and made anew (a load), 5 times a session at most
    // (round 107; the user: "a double only appears after I have equipped the gun once") also after any one of them is
    // made anew (a put-away), with each slot's nearest other draw ("props diag near"), 10 times a session at most
    {
        static double twin_at = 0.0, twin_end = 0.0;
        static bool twin_done = false, twin_was = false;
        static int twin_n = 0;
        static uint32_t twin_obj[4] = {};
        bool any = false, remade = false;
        {
            std::lock_guard lock(g_mutex);  // (the review: want is g_mutex's)
            for (int i = kHolsterSlot; i < kHolsterSlot + 4 && i < kSlots; ++i) {
                if (g_slot[i].obj && g_slot[i].want) any = true;
                const uint32_t o = g_slot[i].want ? g_slot[i].obj : 0;
                if (o && twin_obj[i - kHolsterSlot] && o != twin_obj[i - kHolsterSlot]) remade = true;
                if (o) twin_obj[i - kHolsterSlot] = o;
            }
        }
        if (twin_done && ((!any && twin_n < 5) || (remade && twin_n < 10))) {  // all gone, or one made anew: armed again
            twin_done = false;
            twin_at = twin_end = 0.0;
        }
        if (!twin_done) {
            if (twin_at == 0.0) {
                if (any) twin_at = now + 8000.0;
            } else if (twin_end == 0.0 && now >= twin_at) {
                twin_was = g_diag.load(std::memory_order_relaxed);
                diag_command("reset");
                g_diag = true;
                twin_end = now + 3000.0;
            } else if (twin_end > 0.0 && now >= twin_end) {
                log::info("[prop] the holsters' draws 8 s after their guns were made (%d): %s", twin_n + 1, diag_command("").c_str());
                for (int i = kHolsterSlot; i < kHolsterSlot + 4 && i < kSlots; ++i) {  // (round 107) each one's nearest other draw
                    bool w = false;
                    {
                        std::lock_guard lock(g_mutex);
                        w = g_slot[i].want != nullptr;
                    }
                    if (w) log::info("[prop]   %s", diag_command("near" + std::to_string(i)).c_str());
                }
                if (!twin_was) g_diag = false;
                twin_done = true;
                ++twin_n;
            }
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

uint64_t shape_memo(uintptr_t d, double now);  // (below)
std::atomic<bool> g_alt_on{true};  // "props alt on|off" (the session: the perf check's control)
void set_alt(bool on) { g_alt_on = on; }
// (round 108; the user: a double at the holster "only appears after I have equipped the gun once", and the flicker "is
// just the two gun models overlapping"; their log: after a put-away another drawable of the gun's model drawn 0.000 m
// from the right hip's put, the same shape, unturned, in both eyes' passes, never taken: the left hip's prop, remade
// after the copy, had learned that drawable, so it was "known" and the round 103 check never asked) a known drawable's
// draw at a put is checked too ("props altknown on|off", the session's control)
std::atomic<bool> g_alt_known{true};
// (round 109; the user: still "double and flicker" at the holster after a draw; cycles RM1/RW1: after the user's
// sequence the right hip's slot matched one more draw of the model 3-30 cm from its puts for each of its own, John
// standing: the game's own holstered Cattleman (the same shared drawable, no instance, its axes turned like ours)
// moved onto the holster gun, the two overlapping. A holster gun's own draws sit at one of its recent puts (the game
// draws the object where a tick put it; eight kept): every cycle of them standing, walking (r166) and turning
// (r171) matched within 3 cm, none further. So a holster slot takes only those (the review: plus a frame's travel at
// the body's speed, a gallop's puts 9-13 cm apart); one it refuses within its gate, taken by no slot, is hidden (the
// review: the body's rules hid it only near an attachment bone, never on the rigid path) ("props strict on|off")
std::atomic<bool> g_strict{true};
void set_strict(bool on) { g_strict = on; }
void set_alt_known(bool on) { g_alt_known = on; }
std::atomic<bool> g_early_on{true};  // "props early on|off" (round 104; the session: the test's control)
void set_early(bool on) {  // (also the slots' early counts reset: the test's on and off apart)
    std::lock_guard lock(g_mutex);
    g_early_on = on;
    for (Slot& s : g_slot) s.early_n = s.early_cmp = s.early_seq = 0, s.early_sum = 0.0, s.early_max = 0.0f;
}
std::atomic<bool> g_census_matched{true};  // "props census world|own": the census slot's object left to the world draws' rules (item 13's test)
std::atomic<uint64_t> g_pool_hidden{0};
bool match(uintptr_t drawable, uintptr_t inst, const float* pos, float* pose, int* slot, bool* hide, bool early) {
    if (hide) *hide = false;
    if (!g_any.load(std::memory_order_relaxed) || !drawable) return false;
    // every placed slot's drawable known: a draw of none of them needs no lock
    const bool k_ = known(drawable);
    const bool near = (!k_ || g_alt_known.load(std::memory_order_relaxed)) && g_alt_on.load(std::memory_order_relaxed) &&
                      near_a_put(pos);  // (round 103) maybe another copy's at a put (round 108: a known drawable's too)
    if (!g_learning.load(std::memory_order_relaxed) && !k_ && !near) return false;
    std::lock_guard lock(g_mutex);
    const double now_ms = log::now_ms();
    // of the slots with this drawable (or none learned yet and put within 3 cm), the one put nearest, within 1.5 m of
    // one of its last four puts (the game draws the object where a script tick put it: walking, that trails the latest)
    Slot* best = nullptr;
    // the gate, widened by the body's speed: the prop is drawn where a script tick put it, which trails further the faster
    // John goes (0.3 m standing and walking; galloping at 8 m/s about 1.3 m), still far less than the old 1.5 m at rest
    const float gate = g_gate.load(std::memory_order_relaxed) + g_root_speed.load(std::memory_order_relaxed) * 0.125f;
    float bd = gate * gate;
    const bool by_inst = g_inst_check.load(std::memory_order_relaxed);
    // (round 109) a holster slot's reach off its puts (3 cm and a 90 Hz frame's travel), and the nearest one refusing
    const float strict_r = 0.03f + g_root_speed.load(std::memory_order_relaxed) * 0.012f;
    const bool strict = g_strict.load(std::memory_order_relaxed);
    Slot* refused = nullptr;
    float refused_d2 = 1e30f;
    const bool census = g_census_matched.load(std::memory_order_relaxed);
    for (Slot& s : g_slot) {
        if (!s.put_ok || !s.want) continue;
        if (!census && &s == &g_slot[kCensusSlot]) continue;  // ("props census world") drawn as any world object's draw is
        if (&s - g_slot == g_refuse_slot.load(std::memory_order_relaxed)) continue;  // (the test's refusal)
        // (cycle r174, the pool's perf: mode 2 cost about 0.16 ms of scene render) a slot whose model is learned and is
        // not this draw's, or that learns no more, needs no distance: of the 32 slots only the draw's own model's and the
        // learning ones are measured (the shells' 14 for a shell's draw, none of them for a gun's)
        bool same = drawable == s.drawable;
        if (!same && near && s.drawable && &s - g_slot < kPoolFirst && put_d2(s, pos) < kAltGate * kAltGate) {  // (round 108: never its own)
            // (round 103) another copy of its model drawn at a put itself: the same shape (each draw checked), taken as its own
            const uint64_t a = shape_memo(drawable, now_ms);
            if (a && a == shape_memo(s.drawable, now_ms)) {
                if (s.alt != drawable && s.alt_n++ < 8)
                    log::info("[prop] slot %d drawn from another copy of its model (drawable %llx, its own %llx: the same shape, at a put): "
                              "taken as its own", static_cast<int>(&s - g_slot), static_cast<unsigned long long>(drawable),
                              static_cast<unsigned long long>(s.drawable));
                s.alt = drawable;
                ++s.alt_draws;
                same = true;
            }
        }
        if (!same && (s.drawable || s.learn_off)) continue;
        const float d2 = put_d2(s, pos);
        if (strict && d2 > strict_r * strict_r && &s - g_slot >= kHolsterSlot && &s - g_slot < kHolsterSlot + 4) {
            if (d2 < gate * gate && d2 < refused_d2) refused = &s, refused_d2 = d2;  // (round 109) not its object's
            continue;
        }
        bool ok = same || d2 < 0.03f * 0.03f;
        if (ok && by_inst && s.inst && inst && inst != s.inst) {
            if (d2 < 0.03f * 0.03f) {  // a new instance at the put itself: the game remade the object
                s.inst = inst;
                ++s.relearned;
            } else {
                ++s.other;
                if (d2 < bd) ++s.other_near;
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
    if (!best) {
        if (!refused || !hide) return false;
        // (round 109) refused by the nearest holster gun and taken by no slot: hidden by the caller (as a pool spare)
        ++refused->strict_refused;
        *hide = true;
        if (slot) *slot = static_cast<int>(refused - g_slot);
        return true;
    }
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
    if (best->anchored && g_root_ok) {  // the body's walk since the pose was placed (the frame end before)
        // (round 104) an early draw (before this frame's body): its root last frame's, so its step on to this frame's too
        const bool step = early && g_early_on.load(std::memory_order_relaxed);
        for (int k = 0; k < 3; ++k) pose[9 + k] += g_root_now[k] - best->anchor[k] + (step ? g_root_step[k] : 0.0f);
        if (early) {
            ++best->early_n;
            if (best->early_seq != g_root_seq + 1) {  // the frame's first early draw
                std::memcpy(best->early_at, pose + 9, sizeof(best->early_at));
                best->early_seq = g_root_seq + 1;
            }
        } else if (best->early_seq && best->early_seq == g_root_seq) {  // the same frame's first main draw
            const float dx = pose[9] - best->early_at[0], dy = pose[10] - best->early_at[1], dz = pose[11] - best->early_at[2];
            const float mm = 1000.0f * std::sqrt(dx * dx + dy * dy + dz * dz);
            ++best->early_cmp;
            best->early_sum += mm;
            best->early_max = std::fmax(best->early_max, mm);
            best->early_seq = 0;
        }
    }
    std::memcpy(best->drawn, pose + 9, sizeof(best->drawn));
    for (int k = 0; k < 3; ++k) best->rel[k] = pose[9 + k] - (g_root_ok ? g_root_now[k] : 0.0f);
    if (slot) *slot = bi;
    best->shown_ms = log::now_ms();
    g_matched.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool diag_on() { return g_diag.load(std::memory_order_relaxed); }

void set_census_matched(bool on) { g_census_matched = on; }
std::atomic<bool> g_twins_on{true};  // "props twins on|off": the belt's twin hidden (the session; the perf test's control)
void set_twins(bool on) { g_twins_on = on; }
void set_auto_turn(bool on) { g_auto_turn = on; }
// (2026-11-05, GOAL-next-items item 10; the user: "another gun on top on the gun in the holster" after every load, "It would
// flicker between the holster position and wherever it would be on Johns normal model"; their logs: another drawable
// drawn 2.3 cm from the right hip's shown gun) after a load the game draws its holstered gun from another copy of the
// model than the shared one our holster's object draws (after a draw and a put-away it takes the shared one: hidden): a
// drawable's shape, its box (+0x30, +0x40) and each model's geometries' vertex counts (+0x50 the models' list: its
// models, +8 their count; a model's +8 geometries, +0x2e their count; a geometry's +0x18 vertex buffer, its +8 the count),
// SEH-guarded reads; 0 unreadable
uint64_t shape_of(uintptr_t d) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    __try {
        uint32_t box[6];
        std::memcpy(box, reinterpret_cast<const void*>(d + 0x30), 12);
        std::memcpy(box + 3, reinterpret_cast<const void*>(d + 0x40), 12);
        for (uint32_t v : box) mix(v);
        const uintptr_t list = *reinterpret_cast<const uintptr_t*>(d + 0x50);
        if (!list) return 0;
        const uintptr_t models = *reinterpret_cast<const uintptr_t*>(list);
        const uint16_t nm = *reinterpret_cast<const uint16_t*>(list + 8);
        if (!models || !nm || nm > 64) return 0;
        mix(nm);
        for (int mi = 0; mi < nm && mi < 16; ++mi) {
            const uintptr_t m = *reinterpret_cast<const uintptr_t*>(models + mi * 8);
            if (!m) continue;
            const uintptr_t geoms = *reinterpret_cast<const uintptr_t*>(m + 8);
            const uint16_t ng = *reinterpret_cast<const uint16_t*>(m + 0x2e);
            mix(ng);
            for (int gi = 0; geoms && gi < ng && gi < 16; ++gi) {
                const uintptr_t g = *reinterpret_cast<const uintptr_t*>(geoms + gi * 8);
                const uintptr_t vb = g ? *reinterpret_cast<const uintptr_t*>(g + 0x18) : 0;
                mix(vb ? *reinterpret_cast<const uint16_t*>(vb + 8) : 0);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return h ? h : 1;
}
struct ShapeMemo {
    uintptr_t d = 0;
    uint64_t shape = 0;
    double ms = -1e12;
};
ShapeMemo g_shape_memo[32];  // (under g_mutex) the shapes read lately, each again after 2 s (a freed drawable's address reused)
int g_shape_next = 0;
std::atomic<uint64_t> g_twin_shapes{0};  // the draws hidden as another copy of a shown gun's model
uint64_t shape_memo(uintptr_t d, double now) {  // under g_mutex
    for (ShapeMemo& m : g_shape_memo)
        if (m.d == d && now - m.ms < 2000.0) return m.shape;
    ShapeMemo& m = g_shape_memo[g_shape_next];
    g_shape_next = (g_shape_next + 1) % 32;
    m = {d, shape_of(d), now};
    return m.shape;
}

bool shown_at_holster(uintptr_t drawable, const float* pos) {
    if (!drawable || !g_any.load(std::memory_order_relaxed) || !g_twins_on.load(std::memory_order_relaxed) ||
        g_clear.load(std::memory_order_relaxed))
        return false;
    const bool k = known(drawable);  // the lock-free filter first (match's): most draws are of no prop's model
    if (!k && !pos) return false;
    std::lock_guard lock(g_mutex);
    if (k) {
        for (int i = kHolsterSlot; i < kHolsterSlot + 4 && i < kSlots; ++i)
            if (g_slot[i].want && g_slot[i].drawable == drawable) return true;
        // ([Hands] OffHandItems) the off hand's item: its model's own at John's belt (the knife at its sheath) hidden too
        if (g_slot[kOffhandSlot].want && g_slot[kOffhandSlot].drawable == drawable) return true;
    }
    // (item 10) another copy of a holster's shown gun's model, within 0.45 m of that gun (the game's own holster: 2-40 cm)
    const double now = log::now_ms();
    uint64_t mine = 0;
    for (int i = kHolsterSlot; i < kHolsterSlot + 4 && i < kSlots; ++i) {
        const Slot& s = g_slot[i];
        if (!s.want || !s.drawable || s.drawable == drawable || !s.put_ok || put_d2(s, pos) > 0.45f * 0.45f) continue;
        if (!mine) mine = shape_memo(drawable, now);
        if (mine && shape_memo(s.drawable, now) == mine) {
            if (g_twin_shapes.fetch_add(1, std::memory_order_relaxed) < 3)  // (the first three a session)
                log::info("[prop] slot %d: another copy of its model drawn %.2f m from it (drawable %llx, the slot's %llx, the same shape): "
                          "hidden as the game's holstered gun", i, std::sqrt(put_d2(s, pos)), static_cast<unsigned long long>(drawable & 0xffffff),
                          static_cast<unsigned long long>(s.drawable & 0xffffff));
            return true;
        }
    }
    return false;
}
uint64_t twin_shapes() { return g_twin_shapes.load(std::memory_order_relaxed); }

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
            if (m) {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) d.own_m[r * 3 + c] = m[r * 4 + c];
                d.own_ok = true;
            }
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
    if (arg.rfind("own", 0) == 0) {  // props diag own<slot>: its own model's last draw's axes against its pose (degrees)
        const int i = std::atoi(arg.c_str() + 3);
        if (i < 0 || i >= kSlots) return "no slot";
        std::lock_guard lock(g_mutex);
        const DiagSlot& d = g_diag_slot[i];
        const float* q = g_slot[i].pose;
        float tr = 0.0f;
        for (int k = 0; k < 9; ++k) tr += d.own_m[k] * q[k];
        const float qs = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);  // (a scaled pose: the belt's props)
        if (qs > 1e-6f) tr /= qs;
        float c = 0.5f * (tr - 1.0f);
        c = c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c;
        char b[360];
        std::snprintf(b, sizeof(b), "slot %d own draw %d: x (%.3f %.3f %.3f) y (%.3f %.3f %.3f) z (%.3f %.3f %.3f) | pose x (%.3f %.3f %.3f) y (%.3f %.3f %.3f) "
                      "z (%.3f %.3f %.3f) | %.1f deg apart", i, d.own_ok ? 1 : 0, d.own_m[0], d.own_m[1], d.own_m[2], d.own_m[3], d.own_m[4], d.own_m[5],
                      d.own_m[6], d.own_m[7], d.own_m[8], q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], d.own_ok ? std::acos(c) * 57.2957795f : -1.0f);
        return b;
    }
    if (arg.rfind("refuse", 0) == 0) {  // props diag refuse<slot> | refuseoff (or refuse-1): the test's refusal
        const char* v = arg.c_str() + 6;
        g_refuse_slot = *v >= '0' && *v <= '9' ? std::atoi(v) : -1;
        return "refuse " + std::to_string(g_refuse_slot.load());
    }
    if (arg.rfind("box", 0) == 0) {  // props diag box<slot>: its learned model's box (the drawable's +0x30 min, +0x40 max; m)
        const int i = std::atoi(arg.c_str() + 3);
        if (i < 0 || i >= kSlots) return "no slot";
        uintptr_t d = 0;
        {
            std::lock_guard lock(g_mutex);
            d = g_slot[i].drawable;
        }
        float lo[3] = {}, hi[3] = {};
        const bool ok = d && read_box(d, lo, hi);
        char b[200];
        std::snprintf(b, sizeof(b), "slot %d box %d: (%.3f %.3f %.3f)-(%.3f %.3f %.3f) size (%.3f %.3f %.3f)", i, ok ? 1 : 0, lo[0], lo[1], lo[2], hi[0], hi[1],
                      hi[2], hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
        return b;
    }
    if (arg.rfind("near", 0) == 0) {  // props diag near<slot>: that slot's nearest other draw's record axes and position
        const int i = std::atoi(arg.c_str() + 4);
        if (i < 0 || i >= kSlots) return "no slot";
        std::lock_guard lock(g_mutex);
        const DiagSlot& d = g_diag_slot[i];
        // (round 102: another drawable 3.7 cm from the copy at the General Store) the same model's shape as the slot's?
        const uint64_t sn = d.near_d ? shape_of(d.near_d) : 0, so = g_slot[i].drawable ? shape_of(g_slot[i].drawable) : 0;
        char b[340];
        std::snprintf(b, sizeof(b), "slot %d nearest %llx at %.3f: x (%.3f %.3f %.3f) y (%.3f %.3f %.3f) z (%.3f %.3f %.3f) at (%.3f %.3f %.3f) | same shape %d",
                      i, static_cast<unsigned long long>(d.near_d & 0xffffff), d.near_dist, d.near_m[0], d.near_m[1], d.near_m[2], d.near_m[3],
                      d.near_m[4], d.near_m[5], d.near_m[6], d.near_m[7], d.near_m[8], d.near_m[9], d.near_m[10], d.near_m[11], sn && sn == so ? 1 : 0);
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

// (GOAL-cattleman-belts item 1) a slot's draws of another object of its model refused (left unmoved: the game remade
// the object, or another of the same model drawn there), logged at most once in 10 s a slot: now turned by its pose
uint64_t g_other_said[kSlots] = {};
double g_other_said_ms[kSlots] = {};
void note_refused() {  // the game thread (g_mutex taken here)
    const double now = log::now_ms();
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kSlots; ++i) {
        const Slot& s = g_slot[i];
        if (s.other_near == g_other_said[i] || now - g_other_said_ms[i] < 10000.0) continue;
        if (s.other_near > g_other_said[i] && s.want)
            log::info("[prop] slot %d (%s): %llu draws of another object of its model at its place left unmoved since the last line (the "
                      "game remade it, or another is there; the slot's own object is turned by its pose now)", i, s.want,
                      static_cast<unsigned long long>(s.other_near - g_other_said[i]));
        g_other_said[i] = s.other_near;
        g_other_said_ms[i] = now;
    }
}
void publish_status() {  // the game thread, the end of each frame()
    note_refused();
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kSlots; ++i) g_shown[i] = {g_slot[i].frag, g_slot[i].obj, static_cast<int>(g_slot[i].st)};
}

uintptr_t slot_drawable(int s) { return s >= 0 && s < kPoolFirst ? g_known[s].load(std::memory_order_relaxed) : 0; }
bool slot_holds(int s, const char* frag) {  // (GOAL-after-0.8.13.2 item 7, the review)
    if (s < 0 || s >= kSlots || !frag) return false;
    std::lock_guard lock(g_mutex);
    const Slot& t = g_slot[s];
    return t.obj && t.frag && std::strcmp(t.frag, frag) == 0;
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
    std::snprintf(b, sizeof(b), "props: live %d, made %llu, destroyed %llu, failed %llu, draws matched %llu (learned %llu), a refused name's round "
                  "trips waited %llu", g_live.load(),
                  static_cast<unsigned long long>(g_made.load()), static_cast<unsigned long long>(g_destroyed.load()),
                  static_cast<unsigned long long>(g_failed.load()), static_cast<unsigned long long>(g_matched.load()),
                  static_cast<unsigned long long>(g_learned.load()), static_cast<unsigned long long>(g_name_waited.load()));
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
            if (s.alt) {  // (round 103)
                std::snprintf(b, sizeof(b), " another copy %llx (%llu draws)", static_cast<unsigned long long>(s.alt),
                              static_cast<unsigned long long>(s.alt_draws));
                o += b;
            }
            if (s.strict_refused) {  // (round 109)
                std::snprintf(b, sizeof(b), " strict refused %llu", static_cast<unsigned long long>(s.strict_refused));
                o += b;
            }
            if (s.early_n) {  // (round 104)
                std::snprintf(b, sizeof(b), " early %llu (against the frame's main draw: %llu frames, mean %.1f max %.1f mm)",
                              static_cast<unsigned long long>(s.early_n), static_cast<unsigned long long>(s.early_cmp),
                              s.early_cmp ? s.early_sum / static_cast<double>(s.early_cmp) : 0.0, s.early_max);
                o += b;
            }
        }
    }
    return o;
}

}  // namespace rdrvr::held_prop
