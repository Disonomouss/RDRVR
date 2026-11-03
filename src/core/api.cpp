#include "core/api.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "core/gun_melee.h"
#include "core/log.h"

namespace rdrvr::api {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
// v7: the plugin's tick, open from on_script_tick to end_script_tick, on the thread that reported it
std::atomic<bool> g_in_tick{false};
std::atomic<DWORD> g_tick_thread{0};
CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
std::deque<RdrvrNativeRequest> g_requests;
std::unordered_map<uint64_t, RdrvrNativeResult> g_results;
std::atomic<uint64_t> g_next_id{1};
// (2026-10-27, DualWieldCrossReload's re-review) the highest request id whose result the plugin posted, and the tick of the
// latest posted result: ids are given under g_lock as the request is queued, the plugin runs them in that order on one
// thread and posts each, so every id up to the mark has run (whether or not its result is still in g_results, which is
// cleared past 4096 unread). Watches (kWatchBit) not counted
std::atomic<uint64_t> g_posted_id{0}, g_posted_tick{0};
std::atomic<uint64_t> g_ticks{0};
std::atomic<double> g_last_tick_ms{0.0};
std::atomic<bool> g_attached{false};

// Watched natives: request ids with the top bit set, the low bits the watch index.
constexpr uint64_t kWatchBit = 1ull << 63;
// (2026-10-29) a request queued at the front (queue_native_front): its id carries this bit and stays out of the posted
// mark (result_posted's "every id up to it has run" holds only for the requests run in the order queued)
constexpr uint64_t kFrontBit = 1ull << 62;
constexpr int kWatches = 8;
std::atomic<uint32_t> g_watch_hash[kWatches];
std::atomic<uint64_t> g_watch_arg[kWatches];
std::atomic<uint32_t> g_watch_argc[kWatches];
std::atomic<int> g_watch_count{0};
std::atomic<uint64_t> g_watch_value[kWatches], g_watch_tick[kWatches];
// (2026-10-28, round 38 check 2) time_native: a request's run by the plugin timed from its pop to its result (QPC), per
// slot (the latest id given to it), under g_lock
constexpr int kTimed = 4;
uint64_t g_timed_id[kTimed] = {};
LARGE_INTEGER g_timed_t0[kTimed] = {};
uint64_t g_timed_us[kTimed] = {}, g_timed_n[kTimed] = {};
double qpc_us(LONGLONG d) {
    static const LONGLONG f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q.QuadPart > 0 ? q.QuadPart : 1;
    }();
    return static_cast<double>(d) * 1e6 / static_cast<double>(f);
}

void api_log(int level, const char* text) {
    log::write(level >= 2 ? log::Level::Error : level == 1 ? log::Level::Warn : log::Level::Info, "[red] %s", text);
}

std::atomic<void (*)()> g_tick_hook{nullptr};
std::atomic<void (*)()> g_tick_hook2{nullptr};  // (2026-10-29) a second: [Hands] OffHandItems' throw (offhand.cpp)
std::atomic<void (*)()> g_tick_hook3{nullptr};  // (2026-10-31) a third: the test-only probe commands (probe.cpp; set by a command only)
void api_on_script_tick(uint64_t tick, double /*script_ms*/) {
    if (!g_attached.exchange(true)) log::info("[api] gameplay plugin attached; first script tick %llu", tick);
    g_tick_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_in_tick.store(true, std::memory_order_release);
    g_ticks.store(tick, std::memory_order_relaxed);
    g_last_tick_ms.store(log::now_ms(), std::memory_order_relaxed);
    if (void (*h)() = g_tick_hook.load(std::memory_order_acquire)) h();  // before the lock: it may queue natives
    if (void (*h2)() = g_tick_hook2.load(std::memory_order_acquire)) h2();
    if (void (*h3)() = g_tick_hook3.load(std::memory_order_acquire)) h3();
    int n = g_watch_count.load(std::memory_order_acquire);
    if (!n) return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = n - 1; i >= 0; --i) {  // ahead of the queued requests, once per tick
        RdrvrNativeRequest r{};
        r.id = kWatchBit | static_cast<uint64_t>(i);
        r.op = RDRVR_NATIVE_RAW;
        r.hash = g_watch_hash[i].load(std::memory_order_relaxed);
        r.argc = g_watch_argc[i].load(std::memory_order_relaxed);
        r.args[0] = g_watch_arg[i].load(std::memory_order_relaxed);
        g_requests.push_front(r);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

int api_pop_native_request(RdrvrNativeRequest* out) {
    AcquireSRWLockExclusive(&g_lock);
    bool have = !g_requests.empty();
    if (have) {
        *out = g_requests.front();
        g_requests.pop_front();
        for (int s = 0; s < kTimed; ++s)
            if (g_timed_id[s] && out->id == g_timed_id[s]) QueryPerformanceCounter(&g_timed_t0[s]);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return have ? 1 : 0;
}

void api_post_native_result(const RdrvrNativeResult* r) {
    if (!(r->id & (kWatchBit | kFrontBit))) {  // (the mark: the tick first, so a reader of the id sees at least its tick)
        g_posted_tick.store(r->tick, std::memory_order_relaxed);
        if (r->id > g_posted_id.load(std::memory_order_relaxed)) g_posted_id.store(r->id, std::memory_order_release);
    }
    if (r->id & kWatchBit) {
        int i = static_cast<int>(r->id & 0xff);
        if (i < kWatches) {
            g_watch_value[i].store(r->value, std::memory_order_relaxed);
            g_watch_tick[i].store(r->tick, std::memory_order_release);
        }
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (int s = 0; s < kTimed; ++s)
        if (g_timed_id[s] && r->id == g_timed_id[s] && g_timed_t0[s].QuadPart) {
            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            g_timed_us[s] += static_cast<uint64_t>(qpc_us(t1.QuadPart - g_timed_t0[s].QuadPart));
            ++g_timed_n[s];
            g_timed_id[s] = 0;
            g_timed_t0[s].QuadPart = 0;
        }
    if (g_results.size() > 4096) g_results.clear();  // nobody is collecting; do not grow without bound
    g_results[r->id] = *r;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
}

SRWLOCK g_cam_lock = SRWLOCK_INIT;
RdrvrCameraJob g_job{};
RdrvrActorState g_actor{};

void api_get_camera_job(RdrvrCameraJob* out) {
    AcquireSRWLockShared(&g_cam_lock);
    *out = g_job;
    ReleaseSRWLockShared(&g_cam_lock);
}

void api_post_actor_state(const RdrvrActorState* st) {
    AcquireSRWLockExclusive(&g_cam_lock);
    g_actor = *st;
    ReleaseSRWLockExclusive(&g_cam_lock);
}

static_assert(sizeof(RdrvrGunMeleeArgs) == sizeof(RdrvrNativeRequest::args), "the gun melee's arguments are copied over args");
int api_gun_melee_hit(const RdrvrMeleeHit* hit) { return gun_melee::hit(hit); }

void api_end_script_tick(uint64_t /*tick*/) { g_in_tick.store(false, std::memory_order_release); }

const RdrvrApi g_api = {
    RDRVR_API_VERSION, api_log, api_on_script_tick, api_pop_native_request, api_post_native_result,
    api_get_camera_job, api_post_actor_state, api_gun_melee_hit, api_end_script_tick,
};

}  // namespace

uint64_t queue_native(uint32_t hash, const uint64_t* args, uint32_t argc, uint32_t vec_out, const float* vec_in) {
    RdrvrNativeRequest r{};
    r.op = RDRVR_NATIVE_RAW;
    r.hash = hash;
    r.argc = argc > 12 ? 12 : argc;
    r.vec_out = vec_out;
    if (args && r.argc) std::memcpy(r.args, args, r.argc * sizeof(uint64_t));
    if (vec_in) std::memcpy(r.vec_in, vec_in, 3 * sizeof(float));
    AcquireSRWLockExclusive(&g_lock);
    r.id = g_next_id.fetch_add(1);  // (under the lock: the queue's order is the ids' order, for result_posted)
    g_requests.push_back(r);
    ReleaseSRWLockExclusive(&g_lock);
    return r.id;
}

uint64_t queue_native_front(uint32_t hash, const uint64_t* args, uint32_t argc) {
    RdrvrNativeRequest r{};
    r.op = RDRVR_NATIVE_RAW;
    r.hash = hash;
    r.argc = argc > 12 ? 12 : argc;
    if (args && r.argc) std::memcpy(r.args, args, r.argc * sizeof(uint64_t));
    AcquireSRWLockExclusive(&g_lock);
    r.id = g_next_id.fetch_add(1) | kFrontBit;
    g_requests.push_front(r);
    ReleaseSRWLockExclusive(&g_lock);
    return r.id;
}

uint64_t queue_op(uint32_t op, const uint64_t* args, uint32_t argc, const float* vec_in) {
    RdrvrNativeRequest r{};
    r.op = op;
    r.argc = argc > 12 ? 12 : argc;
    if (args && r.argc) std::memcpy(r.args, args, r.argc * sizeof(uint64_t));
    if (vec_in) std::memcpy(r.vec_in, vec_in, 3 * sizeof(float));
    AcquireSRWLockExclusive(&g_lock);
    r.id = g_next_id.fetch_add(1);  // (under the lock: the queue's order is the ids' order, for result_posted)
    g_requests.push_back(r);
    ReleaseSRWLockExclusive(&g_lock);
    return r.id;
}

bool wait_native(uint64_t id, RdrvrNativeResult* out, uint32_t timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    AcquireSRWLockExclusive(&g_lock);
    for (;;) {
        auto it = g_results.find(id);
        if (it != g_results.end()) {
            *out = it->second;
            g_results.erase(it);
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        SleepConditionVariableSRW(&g_cv, &g_lock, static_cast<DWORD>(deadline - now), 0);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

bool cancel_op(uint64_t id) {
    AcquireSRWLockExclusive(&g_lock);
    bool removed = false;
    for (auto it = g_requests.begin(); it != g_requests.end(); ++it)
        if (it->id == id) {
            g_requests.erase(it);
            removed = true;
            break;
        }
    if (!removed) g_results.erase(id);  // already answered and not read: dropped too
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}

uint64_t script_ticks() { return g_ticks.load(std::memory_order_relaxed); }
unsigned long script_tick_thread() { return g_tick_thread.load(std::memory_order_relaxed); }
bool result_posted(uint64_t id, uint64_t* tick) {
    if (!id || id > g_posted_id.load(std::memory_order_acquire)) return false;
    if (tick) *tick = g_posted_tick.load(std::memory_order_relaxed);
    return true;
}
double last_script_tick_ms() { return g_last_tick_ms.load(std::memory_order_relaxed); }
bool plugin_attached() { return g_attached.load(); }
bool in_script_tick() {
    return g_in_tick.load(std::memory_order_acquire) && g_tick_thread.load(std::memory_order_relaxed) == GetCurrentThreadId();
}
void set_tick_hook(void (*hook)(), int slot) {
    (slot == 2 ? g_tick_hook3 : slot == 1 ? g_tick_hook2 : g_tick_hook).store(hook, std::memory_order_release);
}
void time_native(uint64_t id, int slot) {
    if (!id || slot < 0 || slot >= kTimed) return;
    AcquireSRWLockExclusive(&g_lock);
    g_timed_id[slot] = id;
    g_timed_t0[slot].QuadPart = 0;
    ReleaseSRWLockExclusive(&g_lock);
}
void native_time(int slot, uint64_t* us, uint64_t* n) {
    *us = *n = 0;
    if (slot < 0 || slot >= kTimed) return;
    AcquireSRWLockShared(&g_lock);
    *us = g_timed_us[slot];
    *n = g_timed_n[slot];
    ReleaseSRWLockShared(&g_lock);
}

void set_camera_job(const RdrvrCameraJob& job) {
    AcquireSRWLockExclusive(&g_cam_lock);
    g_job = job;
    ReleaseSRWLockExclusive(&g_cam_lock);
}

RdrvrCameraJob camera_job() {
    AcquireSRWLockShared(&g_cam_lock);
    RdrvrCameraJob j = g_job;
    ReleaseSRWLockShared(&g_cam_lock);
    return j;
}

bool actor_state(RdrvrActorState* out) {
    AcquireSRWLockShared(&g_cam_lock);
    *out = g_actor;
    ReleaseSRWLockShared(&g_cam_lock);
    return out->valid != 0;
}

int watch_native(uint32_t hash, uint64_t arg0, uint32_t argc) {
    int i = g_watch_count.load();
    for (int k = 0; k < i; ++k)
        if (g_watch_hash[k].load() == hash && g_watch_arg[k].load() == arg0 && g_watch_argc[k].load() == argc) return k;
    if (i >= kWatches) return -1;
    g_watch_hash[i] = hash;
    g_watch_arg[i] = arg0;
    g_watch_argc[i] = argc > 1 ? 1 : argc;
    g_watch_tick[i] = 0;
    g_watch_count.store(i + 1, std::memory_order_release);
    log::info("[api] watching native 0x%08X every script tick (watch %d)", hash, i);
    return i;
}

bool watched(int index, uint64_t* value, uint64_t* tick) {
    if (index < 0 || index >= g_watch_count.load(std::memory_order_acquire)) return false;
    uint64_t t = g_watch_tick[index].load(std::memory_order_acquire);
    if (!t) return false;
    *value = g_watch_value[index].load(std::memory_order_relaxed);
    if (tick) *tick = t;
    return true;
}

}  // namespace rdrvr::api

// Exported through dinput8.def.
extern "C" const RdrvrApi* RDRVR_GetApi() { return &rdrvr::api::g_api; }
