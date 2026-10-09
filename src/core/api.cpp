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
std::atomic<uint64_t> g_ticks{0};
std::atomic<double> g_last_tick_ms{0.0};
std::atomic<bool> g_attached{false};

// Watched natives: request ids with the top bit set, the low bits the watch index.
constexpr uint64_t kWatchBit = 1ull << 63;
constexpr int kWatches = 8;
std::atomic<uint32_t> g_watch_hash[kWatches];
std::atomic<uint64_t> g_watch_arg[kWatches];
std::atomic<uint32_t> g_watch_argc[kWatches];
std::atomic<int> g_watch_count{0};
std::atomic<uint64_t> g_watch_value[kWatches], g_watch_tick[kWatches];

void api_log(int level, const char* text) {
    log::write(level >= 2 ? log::Level::Error : level == 1 ? log::Level::Warn : log::Level::Info, "[red] %s", text);
}

void api_on_script_tick(uint64_t tick, double /*script_ms*/) {
    if (!g_attached.exchange(true)) log::info("[api] gameplay plugin attached; first script tick %llu", tick);
    g_tick_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_in_tick.store(true, std::memory_order_release);
    g_ticks.store(tick, std::memory_order_relaxed);
    g_last_tick_ms.store(log::now_ms(), std::memory_order_relaxed);
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
    }
    ReleaseSRWLockExclusive(&g_lock);
    return have ? 1 : 0;
}

void api_post_native_result(const RdrvrNativeResult* r) {
    if (r->id & kWatchBit) {
        int i = static_cast<int>(r->id & 0xff);
        if (i < kWatches) {
            g_watch_value[i].store(r->value, std::memory_order_relaxed);
            g_watch_tick[i].store(r->tick, std::memory_order_release);
        }
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
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
    r.id = g_next_id.fetch_add(1);
    r.op = RDRVR_NATIVE_RAW;
    r.hash = hash;
    r.argc = argc > 12 ? 12 : argc;
    r.vec_out = vec_out;
    if (args && r.argc) std::memcpy(r.args, args, r.argc * sizeof(uint64_t));
    if (vec_in) std::memcpy(r.vec_in, vec_in, 3 * sizeof(float));
    AcquireSRWLockExclusive(&g_lock);
    g_requests.push_back(r);
    ReleaseSRWLockExclusive(&g_lock);
    return r.id;
}

uint64_t queue_op(uint32_t op, const uint64_t* args, uint32_t argc, const float* vec_in) {
    RdrvrNativeRequest r{};
    r.id = g_next_id.fetch_add(1);
    r.op = op;
    r.argc = argc > 12 ? 12 : argc;
    if (args && r.argc) std::memcpy(r.args, args, r.argc * sizeof(uint64_t));
    if (vec_in) std::memcpy(r.vec_in, vec_in, 3 * sizeof(float));
    AcquireSRWLockExclusive(&g_lock);
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
double last_script_tick_ms() { return g_last_tick_ms.load(std::memory_order_relaxed); }
bool plugin_attached() { return g_attached.load(); }
bool in_script_tick() {
    return g_in_tick.load(std::memory_order_acquire) && g_tick_thread.load(std::memory_order_relaxed) == GetCurrentThreadId();
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
