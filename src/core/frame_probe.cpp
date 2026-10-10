// The single-pass probe (frame_probe.h). Every slot is a sum of microseconds in a relaxed atomic; a reader takes two
// snapshots and divides by the render frames between them. The thread cycles are QueryThreadCycleTime deltas, in TSC
// ticks on this hardware (an invariant TSC), turned into milliseconds by the TSC's rate over the same window.

#include "core/frame_probe.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::frame_probe {
namespace {

std::atomic<bool> g_on{false}, g_installed{false};
std::atomic<uint64_t> g_us[kSlotCount];
std::atomic<uint64_t> g_cycles[kThreadCount];
std::atomic<uint64_t> g_frames{0}, g_stereo{0}, g_mono{0}, g_period_us{0}, g_periods{0}, g_draws{0};
thread_local uint64_t t_cycles = 0;   // this thread's cycle count at its last thread_frame
thread_local uint64_t t_draws = 0;    // the playback thread's draw count at its last Present
double g_last_start = 0;              // render thread: the previous SceneRender's start

// EndFrame's wait for the playback thread (FUN_140ecfd80(recorder, flag), called once a frame by EndFrame 0xecda50)
using RecorderWaitDone_t = void (*)(void* recorder, uint8_t flag);
RecorderWaitDone_t o_RecorderWaitDone = nullptr;

void hk_RecorderWaitDone(void* recorder, uint8_t flag) {
    if (!on()) return o_RecorderWaitDone(recorder, flag);
    const double t0 = log::now_ms();
    o_RecorderWaitDone(recorder, flag);
    add(kPlayWait, log::now_ms() - t0);
}

}  // namespace

bool on() { return g_on.load(std::memory_order_relaxed); }

namespace {
std::atomic<int> g_phase{kOutside};
}
void set_phase(Phase p) { g_phase.store(p, std::memory_order_relaxed); }
Phase phase() { return static_cast<Phase>(g_phase.load(std::memory_order_relaxed)); }

bool set_on(bool want) {
    if (!g_installed.load()) return false;
    g_on = want;
    log::info("[probe] %s", want ? "on" : "off");
    return true;
}

void add(Slot s, double ms) {
    if (ms > 0) g_us[s].fetch_add(static_cast<uint64_t>(ms * 1000.0), std::memory_order_relaxed);
    if (s == kPass1) g_stereo.fetch_add(1, std::memory_order_relaxed);
    if (s == kMono) g_mono.fetch_add(1, std::memory_order_relaxed);
}

void thread_frame(Thread t) {
    ULONG64 c = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &c)) return;
    if (t_cycles && c > t_cycles) g_cycles[t].fetch_add(c - t_cycles, std::memory_order_relaxed);
    t_cycles = c;
}

void render_frame_start(double now) {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    const double period = now - g_last_start;
    if (g_last_start > 0 && period < 1000.0) {  // a load or a pause is not a frame
        g_period_us.fetch_add(static_cast<uint64_t>(period * 1000.0), std::memory_order_relaxed);
        g_periods.fetch_add(1, std::memory_order_relaxed);
    }
    g_last_start = now;
    thread_frame(kRender);
}

void playback_frame(uint64_t draws) {
    if (draws > t_draws && t_draws) g_draws.fetch_add(draws - t_draws, std::memory_order_relaxed);
    t_draws = draws;
    thread_frame(kPlayback);
}

void install() {
    if (!config::get_bool("XR", "TimingProbe", true)) {
        log::info("[probe] off ([XR] TimingProbe=0)");
        return;
    }
    const bool hooked = hooks::install("RDR recorder wait for playback (probe)",
                                       reinterpret_cast<void*>(anchors::addr(anchors::Id::RecorderWaitDone)),
                                       hk_RecorderWaitDone, &o_RecorderWaitDone);
    g_installed = true;
    g_on = true;
    log::info("[probe] on: per-pass and per-wait timers, the threads' cycles%s",
              hooked ? "" : " (no playback wait: its hook failed)");
}

void snap(Snap* s) {
    for (int i = 0; i < kSlotCount; ++i) s->us[i] = g_us[i].load(std::memory_order_relaxed);
    for (int i = 0; i < kThreadCount; ++i) s->cycles[i] = g_cycles[i].load(std::memory_order_relaxed);
    s->frames = g_frames.load(std::memory_order_relaxed);
    s->stereo = g_stereo.load(std::memory_order_relaxed);
    s->mono = g_mono.load(std::memory_order_relaxed);
    s->period_us = g_period_us.load(std::memory_order_relaxed);
    s->periods = g_periods.load(std::memory_order_relaxed);
    s->draws = g_draws.load(std::memory_order_relaxed);
    s->tsc = __rdtsc();
    s->ms = log::now_ms();
}

void format(const Snap& a, const Snap& b, char* out, size_t len) {
    const uint64_t n = b.frames - a.frames;
    if (!on() || n == 0) {
        std::snprintf(out, len, "probe %s", on() ? "no frames" : "off");
        return;
    }
    auto m = [&](Slot s) { return static_cast<double>(b.us[s] - a.us[s]) / 1000.0 / static_cast<double>(n); };
    const uint64_t np = b.periods - a.periods;
    const double period = np ? static_cast<double>(b.period_us - a.period_us) / 1000.0 / static_cast<double>(np) : 0.0;
    const double scene = m(kPre) + m(kPass1) + m(kMid) + m(kPass2) + m(kMono);
    const double rest = period - scene - m(kUiWait) - m(kLockWait) - m(kFrameWait) - m(kPlayWait);
    const double ms_per_tick = b.tsc > a.tsc ? (b.ms - a.ms) / static_cast<double>(b.tsc - a.tsc) : 0.0;
    auto busy = [&](Thread t) {
        return static_cast<double>(b.cycles[t] - a.cycles[t]) * ms_per_tick / static_cast<double>(n);
    };
    std::snprintf(out, len,
                  "probe %llu frames (stereo %llu, mono %llu), render period %.2f | scene %.2f: pre %.2f, pass 1 %.2f "
                  "(+0x88 wait %.2f), between %.2f (+0x38 wait %.2f), pass 2 %.2f, mono %.2f | RenderFrame: +0x38 wait "
                  "%.2f, +0x40 waits %.2f, +0x80 wait %.2f, playback wait %.2f, rest %.2f | playback: idle %.2f, frame "
                  "end %.2f (xrWaitFrame %.2f), present %.2f, draws %.0f | busy: render %.2f, playback %.2f, main %.2f | "
                  "main: +0x18 wait %.2f ms",
                  static_cast<unsigned long long>(n), static_cast<unsigned long long>(b.stereo - a.stereo),
                  static_cast<unsigned long long>(b.mono - a.mono), period, scene, m(kPre), m(kPass1), m(kDataWait),
                  m(kMid), m(kUiWaitMid), m(kPass2), m(kMono), m(kUiWait), m(kLockWait), m(kFrameWait), m(kPlayWait),
                  rest, m(kPlayIdle), m(kFrameEnd), m(kXrWait), m(kPresent),
                  static_cast<double>(b.draws - a.draws) / static_cast<double>(n), busy(kRender), busy(kPlayback),
                  busy(kMain), m(kMainWait));
}

namespace {
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    return p && VirtualQuery(p, &mi, sizeof(mi)) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}
float g_limit_seen = -1.0f;  // the limiter's period as first read (s)
}  // namespace

// The swap chain object at D3dDeviceSingleton +0x48: its vt+0x18 is the Present wrapper FUN_140f8b380, which yields until
// +0x9c seconds have passed since the last frame (0: no limit) and then presents (research\sps, 2026-10-10).
void game_frame_limit(const char* arg, char* out, size_t len) {
    char* const* dev_slot = reinterpret_cast<char* const*>(anchors::addr(anchors::Id::D3dDeviceSingleton));
    char* dev = readable(dev_slot, 8) ? *dev_slot : nullptr;
    char* sc = dev && readable(dev + 0x48, 8) ? *reinterpret_cast<char**>(dev + 0x48) : nullptr;
    void* const* vt = sc && readable(sc, 0xa0) ? *reinterpret_cast<void* const* const*>(sc) : nullptr;
    if (!vt || !readable(vt, 0x20) || reinterpret_cast<uintptr_t>(vt[3]) != anchors::addr(anchors::Id::PresentWrapper)) {
        std::snprintf(out, len, "ERROR the swap chain object not found (no device, or not the Present wrapper's)");
        return;
    }
    float* limit = reinterpret_cast<float*>(sc + 0x9c);
    if (g_limit_seen < 0) {
        const float v = *limit;
        if (v != 0.0f && (v < 1.0f / 150.0f || v > 1.0f / 29.0f)) {
            std::snprintf(out, len, "ERROR an unexpected limit %.6f s: left alone", v);
            return;
        }
        g_limit_seen = v;
    }
    const char* did = "read";
    if (std::strcmp(arg, "off") == 0) {
        *limit = 0.0f;
        did = "off";
    } else if (std::strcmp(arg, "on") == 0) {
        *limit = g_limit_seen;
        did = "put back";
    }
    log::info("[probe] the game's frame limit %s: %.5f s (first seen %.5f s)", did, *limit, g_limit_seen);
    std::snprintf(out, len, "game frame limit %s: %.5f s (%.1f fps; first seen %.5f s)", did, *limit,
                  *limit > 0 ? 1.0f / *limit : 0.0f, g_limit_seen);
}

}  // namespace rdrvr::frame_probe
