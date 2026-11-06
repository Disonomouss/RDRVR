#include "core/cut_log.h"

#include <windows.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/diag.h"
#include "core/frame_probe.h"
#include "core/log.h"
#include "core/vr_mode.h"

namespace rdrvr::cut_log {
namespace {

constexpr int kMaxCutSamples = 6;      // the cutscenes sampled a session
constexpr int kMaxPresentLines = 100;  // the Present (and frame limit) changes logged a session (then one count a minute)

// a stack sampling's request: 1 started, 0 another one running or the thread not known yet (again later), -1 not to
// be asked again (no such thread)
int sample(const char* which, int n, const char* why) {
    const std::string r = diag::sample_stacks(which, n);
    if (r.rfind("ERROR no thread named", 0) == 0) {
        log::info("[cutlog] %s: %s", why, r.c_str());
        return -1;
    }
    if (r.rfind("ERROR", 0) == 0) return 0;
    log::info("[cutlog] %s: %s", why, r.c_str());
    return 1;
}

DWORD WINAPI worker(void*) {
    bool cut = false, sampled = false, playback_due = false, playback_ok = true, play_sampled = false;
    int cut_samples = 0, present_lines = 0, limit_lines = 0;
    double cut_since = 0, stereo_since = 0, last_minute = log::now_ms();
    uint32_t last_present = 0xffffffffu;
    uint64_t changes = 0;
    float last_limit = -2.0f;
    for (unsigned tick = 0;; ++tick) {
        Sleep(250);
        const double now = log::now_ms();
        const bool c = (vr_mode::state_flags() & 16) != 0;
        uint32_t p = 0;
        uint64_t unsynced = 0, tearing = 0;
        d3d::game_present_state(&p, &unsynced, &tearing);
        if (c != cut) {
            cut = c;
            cut_since = now;
            sampled = false;
            char st[200];
            vr_mode::status_text(st, sizeof(st));
            log::info("[cutlog] a cutscene %s (%s); the game's Present: sync interval %u, flags %#x (the mod: %llu Presents with the "
                      "interval taken off, %llu given the tearing flag)", c ? "playing" : "ended", st, p >> 16, p & 0xffffu,
                      static_cast<unsigned long long>(unsynced), static_cast<unsigned long long>(tearing));
        }
        if (p != last_present && p != 0xffffffffu) {
            last_present = p;
            ++changes;
            if (present_lines < kMaxPresentLines) {
                ++present_lines;
                log::info("[cutlog] the game's Present: sync interval %u, flags %#x%s (the mod: %llu Presents with the interval taken off, "
                          "%llu given the tearing flag)%s",
                          p >> 16, p & 0xffffu, (p & DXGI_PRESENT_ALLOW_TEARING) ? " (tearing)" : "", static_cast<unsigned long long>(unsynced),
                          static_cast<unsigned long long>(tearing), cut ? " in a cutscene" : "");
            }
        }
        if (now - last_minute >= 60000.0) {
            last_minute = now;
            if (present_lines >= kMaxPresentLines)
                log::info("[cutlog] the game's Present changed %llu times so far; now sync interval %u, flags %#x", static_cast<unsigned long long>(changes),
                          last_present >> 16, last_present & 0xffffu);
        }
        if (tick % 4 == 0) {
            const float l = frame_probe::game_frame_limit_value();
            if (std::fabs(l - last_limit) > 1e-6f && limit_lines < kMaxPresentLines) {
                last_limit = l;
                ++limit_lines;
                if (l < 0)
                    log::info("[cutlog] the game's frame limit: its swap chain object not found");
                else
                    log::info("[cutlog] the game's frame limit: %.5f s (%.1f fps)%s", l, l > 0 ? 1.0f / l : 0.0f, cut ? " in a cutscene" : "");
            }
        }
        if (!cut) playback_due = false;  // (the cutscene over before the main thread's samples ended: not labelled one)
        if (cut && !sampled && cut_samples < kMaxCutSamples && now - cut_since >= 3000.0) {
            const int r = sample("main", 500, "a cutscene, the main thread");
            if (r) {
                sampled = true;
                playback_due = r > 0 && playback_ok;
                ++cut_samples;
            }
        } else if (playback_due) {  // (after the main thread's: one sampling at a time)
            const int r = sample("playback", 300, "a cutscene, the playback thread");
            if (r) playback_due = false;
            if (r < 0) playback_ok = false;
        }
        if (!vr_mode::gameplay_stereo() || cut) stereo_since = now;
        if (!play_sampled && now - stereo_since >= 60000.0 && sample("main", 500, "in play (to compare), the main thread")) play_sampled = true;
    }
}

}  // namespace

void start() {
    if (!config::get_bool("Debug", "CutsceneLog", true)) return;
    HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!t) {
        log::warn("[cutlog] no thread: the cutscene log off");
        return;
    }
    SetThreadDescription(t, L"RDRVR cutscene log");
    CloseHandle(t);
    log::info("[cutlog] on: the cutscenes' Present, frame limit and main-thread stacks logged");
}

}  // namespace rdrvr::cut_log
