#include "core/held_prop.h"

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
                                      "rdrvr_holster_2", "rdrvr_holster_3",  "rdrvr_model_0",   "rdrvr_model_1"};

uint64_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
uint64_t v2(float x, float y) { return fbits(x) | (fbits(y) << 32); }
uint64_t ptr(const char* s) { return reinterpret_cast<uint64_t>(s); }

enum State { kNone, kRequesting, kStreaming, kLoaded, kCreating, kLive, kDestroying, kChecking };
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
    float puts[4][3] = {};  // the last four queued (the game draws the object where a script tick put it)
    int nput = 0;
    uint64_t gated = 0;     // draws of its drawable beyond the gate (the nearest put farther than 1.5 m)
    float gated_d = 0.0f;   // the last one's distance
    float rel[3] = {};      // its last draw's position less the root that draw used (the body-relative place)
    bool put_ok = false;
    uintptr_t drawable = 0;  // learned from its draws
    double shown_ms = 0;     // its last moved draw
    float anchor[3] = {};    // the body's root its pose was placed by (want's anchor)
    bool anchored = false;
    float drawn[3] = {};     // its last drawn position
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
std::atomic<uintptr_t> g_known[kSlots] = {};  // each placed slot's learned drawable (0: none)
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
constexpr int kMoves = 64;
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
    std::memcpy(s.puts[s.nput % 4], p, sizeof(s.put));
    ++s.nput;
}
float put_d2(const Slot& s, const float* pos) {  // the squared distance to the nearest of the recent puts
    const int n = s.nput < 4 ? s.nput : 4;
    float best = 1e30f;
    for (int i = 0; i < (n ? n : 1); ++i) {
        const float* q = n ? s.puts[i] : s.put;
        const float dx = pos[0] - q[0], dy = pos[1] - q[1], dz = pos[2] - q[2];
        best = std::fmin(best, dx * dx + dy * dy + dz * dz);
    }
    return best;
}
void update_any() {  // under g_mutex
    bool any = false, learning = false;
    for (int i = 0; i < kSlots; ++i) {
        const Slot& t = g_slot[i];
        const bool placed = t.put_ok && t.want;
        any = any || placed;
        learning = learning || (placed && !t.drawable);
        g_known[i].store(placed ? t.drawable : 0, std::memory_order_relaxed);
    }
    g_learning.store(learning, std::memory_order_relaxed);
    g_any.store(any, std::memory_order_relaxed);
}

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

void note_root(const float* root) {
    std::lock_guard lock(g_mutex);
    std::memcpy(g_root_now, root, sizeof(g_root_now));
    g_root_ok = true;
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
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slot[i];
        const char* want;
        float pose[12], ang[3];
        bool angled;
        {
            std::lock_guard lock(g_mutex);
            want = s.want;
            std::memcpy(pose, s.pose, sizeof(pose));
            angled = s.angled;
            std::memcpy(ang, s.angles, sizeof(ang));
        }
        if (s.pending && !api::wait_native(s.pending, &r, 0)) {
            if (now - s.since > 5000.0) {  // no answer: given up, the object (if any) left to the destroy path
                log::warn("[prop] slot %d: no answer in state %d; given up%s", i, static_cast<int>(s.st),
                          s.st == kCreating ? " (an object may have been made unseen; a new layout)" : "");
                if (s.st == kCreating) g_layout = 0;
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
                if (want != s.frag) {
                    s.gave_up = false;
                    s.fails = 0;
                    if (!want) break;
                    s.st = kNone;  // another model: load it
                    break;
                }
                if (s.gave_up || (s.fails && now - s.since < 1000.0)) break;
                if (!g_layout) {
                    if (!g_layout_req) g_layout_req = q(kCreateLayout, {ptr(kLayoutName)});
                    break;
                }
                send(q(kCreateProp, {g_layout, ptr(kObjName[i]), ptr(s.frag), v2(pose[9], pose[10]), fbits(pose[11]), v2(0, 0), fbits(0), 1}));
                s.st = kCreating;
                {
                    std::lock_guard lock(g_mutex);
                    s.nput = 0;
                    push_put(s, pose + 9);
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
                g_made.fetch_add(1, std::memory_order_relaxed);
                g_live.fetch_add(1, std::memory_order_relaxed);
                later(q(kCollideWorld, {s.obj, 0}));
                later(q(kCollideMovables, {s.obj, 0}));
                s.checked = now;
                log::info("[prop] %s made: object %u (slot %d)", s.frag, s.obj, i);
                s.st = kLive;
                break;
            case kLive:
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
                    later(q(kSetPosition, {s.obj, v2(pose[9], pose[10]), fbits(pose[11])}));
                    if (angled) later(q(kSetOrientation, {s.obj, v2(ang[0], ang[1]), fbits(ang[2])}));
                    std::lock_guard lock(g_mutex);
                    push_put(s, pose + 9);
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

bool match(uintptr_t drawable, const float* pos, float* pose, int* slot) {
    if (!g_any.load(std::memory_order_relaxed) || !drawable) return false;
    if (!g_learning.load(std::memory_order_relaxed)) {  // every placed slot's drawable known: a draw of none of them needs no lock
        bool known = false;
        for (const auto& k : g_known) known = known || k.load(std::memory_order_relaxed) == drawable;
        if (!known) return false;
    }
    std::lock_guard lock(g_mutex);
    // of the slots with this drawable (or none learned yet and put within 3 cm), the one put nearest, within 1.5 m of
    // one of its last four puts (the game draws the object where a script tick put it: walking, that trails the latest)
    Slot* best = nullptr;
    float bd = 1.5f * 1.5f;
    for (Slot& s : g_slot) {
        if (!s.put_ok || !s.want) continue;
        const float d2 = put_d2(s, pos);
        const bool ok = drawable == s.drawable || (!s.drawable && d2 < 0.03f * 0.03f);
        if (ok && d2 < bd) {
            best = &s, bd = d2;
        } else if (drawable == s.drawable) {
            ++s.gated;
            s.gated_d = std::sqrt(d2);
        }
    }
    if (!best) return false;
    if (!best->drawable) {
        best->drawable = drawable;
        g_learned.fetch_add(1, std::memory_order_relaxed);
        update_any();  // the lock-free filter knows it now
    }
    std::memcpy(pose, best->pose, sizeof(best->pose));
    if (best->anchored && g_root_ok)  // the body's walk since the pose was placed (the frame end before)
        for (int k = 0; k < 3; ++k) pose[9 + k] += g_root_now[k] - best->anchor[k];
    std::memcpy(best->drawn, pose + 9, sizeof(best->drawn));
    for (int k = 0; k < 3; ++k) best->rel[k] = pose[9 + k] - (g_root_ok ? g_root_now[k] : 0.0f);
    if (slot) *slot = static_cast<int>(best - g_slot);
    best->shown_ms = log::now_ms();
    g_matched.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool diag_on() { return g_diag.load(std::memory_order_relaxed); }

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

int live_objects() { return g_live.load(std::memory_order_relaxed); }

void publish_status() {  // the game thread, the end of each frame()
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kSlots; ++i) g_shown[i] = {g_slot[i].frag, g_slot[i].obj, static_cast<int>(g_slot[i].st)};
}

bool shown(int s) {
    if (s < 0 || s >= kSlots || !g_any.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_mutex);
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
    for (int i = 0; i < kSlots; ++i) {
        std::snprintf(b, sizeof(b), " | slot %d %s obj %u state %d", i, g_shown[i].frag ? g_shown[i].frag : "-", g_shown[i].obj, g_shown[i].st);
        o += b;
    }
    return o;
}

}  // namespace rdrvr::held_prop
