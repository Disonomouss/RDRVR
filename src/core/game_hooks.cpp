#include "core/game_hooks.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/state.h"

namespace rdrvr::game_hooks {
namespace {

// ------------------------------------------------------------------------------------ SetCurrent census
using SetCurrent_t = void* (*)(void* vp, char push, void* p3, char p4);
using PushGlobals_t = void (*)(void* vp, char push_view_inverse);
SetCurrent_t o_SetCurrent = nullptr;
PushGlobals_t o_PushGlobals = nullptr;

constexpr int kSites = 256;
struct Site {
    std::atomic<uintptr_t> caller{0};
    std::atomic<uint32_t> count{0};      // this frame
    std::atomic<uint32_t> last{0};       // last completed frame
    std::atomic<uint64_t> total{0};
};
Site g_sites[kSites];
std::atomic<uint32_t> g_frame_calls{0}, g_frame_pushes{0};
std::atomic<bool> g_overflow{false};

void count_site(uintptr_t caller) {
    uint32_t h = static_cast<uint32_t>((caller >> 2) * 2654435761u) % kSites;
    for (int probe = 0; probe < kSites; ++probe) {
        Site& s = g_sites[(h + probe) % kSites];
        uintptr_t c = s.caller.load(std::memory_order_relaxed);
        if (c == caller || (c == 0 && s.caller.compare_exchange_strong(c, caller)) || c == caller) {
            s.count.fetch_add(1, std::memory_order_relaxed);
            s.total.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_overflow = true;
}

void* hk_SetCurrent(void* vp, char push, void* p3, char p4) {
    g_frame_calls.fetch_add(1, std::memory_order_relaxed);
    count_site(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return o_SetCurrent(vp, push, p3, p4);
}

void hk_PushGlobals(void* vp, char push_view_inverse) {
    g_frame_pushes.fetch_add(1, std::memory_order_relaxed);
    o_PushGlobals(vp, push_view_inverse);
}

void on_frame_end(uint64_t frame) {
    state::frame_set_current = g_frame_calls.exchange(0, std::memory_order_relaxed);
    state::frame_push_globals = g_frame_pushes.exchange(0, std::memory_order_relaxed);
    for (Site& s : g_sites) {
        if (s.caller.load(std::memory_order_relaxed)) s.last = s.count.exchange(0, std::memory_order_relaxed);
    }
    if (frame == 600 || frame % 18000 == 0) census_log_now();
}

double entropy(const unsigned char* p, size_t n) {
    size_t hist[256] = {};
    for (size_t i = 0; i < n; ++i) ++hist[p[i]];
    double e = 0;
    for (size_t c : hist) {
        if (!c) continue;
        double q = static_cast<double>(c) / static_cast<double>(n);
        e -= q * std::log2(q);
    }
    return e;
}

}  // namespace

void census_log_now() {
    std::vector<std::pair<uint32_t, uintptr_t>> rows;
    for (Site& s : g_sites) {
        uintptr_t c = s.caller.load();
        if (c) rows.push_back({s.last.load(), c});
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
    log::info("[census] SetCurrent: %u calls last frame from %zu call sites%s; per-draw globals push %u (probe %s)",
              state::frame_set_current.load(), rows.size(), g_overflow ? " (table overflow)" : "",
              state::frame_push_globals.load(), o_PushGlobals ? "on" : "off");
    uintptr_t base = anchors::base();
    for (size_t i = 0; i < rows.size() && i < 64; ++i) {
        uintptr_t c = rows[i].second;
        if (c >= base && c < base + 0x5a5ec600) log::info("[census]   RDR.exe+%#llx  %u/frame", static_cast<unsigned long long>(c - base), rows[i].first);
        else log::info("[census]   %p  %u/frame", reinterpret_cast<void*>(c), rows[i].first);
    }
}

void census_text(char* out, size_t len) {
    std::vector<std::pair<uint32_t, uintptr_t>> rows;
    for (Site& s : g_sites) {
        uintptr_t c = s.caller.load();
        if (c) rows.push_back({s.last.load(), c});
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
    size_t used = 0;
    out[0] = 0;
    for (size_t i = 0; i < rows.size() && i < 16; ++i) {
        int n = std::snprintf(out + used, len - used, "%s%llx:%u", used ? " " : "",
                              static_cast<unsigned long long>(rows[i].second - anchors::base()), rows[i].first);
        if (n < 0 || static_cast<size_t>(n) >= len - used) break;
        used += static_cast<size_t>(n);
    }
}

bool dump_text_region(const char* reason) {
    const uintptr_t base = anchors::base();
    const size_t begin = 0x1000, size = 0x100000;
    const auto* mem = reinterpret_cast<const unsigned char*>(base + begin);
    wchar_t path[MAX_PATH];
    log::path_in_game_dir(L"RDRVR_text_dump.bin", path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        log::error("[textdump] cannot write %ls (error %lu)", path, GetLastError());
        return false;
    }
    DWORD w = 0;
    BOOL ok = WriteFile(f, mem, static_cast<DWORD>(size), &w, nullptr);
    CloseHandle(f);
    char ent[256];
    size_t used = 0;
    for (size_t off = 0; off < size; off += 0x10000) {
        used += std::snprintf(ent + used, sizeof(ent) - used, " %.2f", entropy(mem + off, 0x10000));
    }
    log::info("[textdump] %s: wrote RVA %#zx..%#zx (%lu bytes, ok %d) to %ls; entropy per 64 KB:%s", reason, begin, begin + size,
              w, ok, path, ent);
    return ok != FALSE;
}

void install() {
    d3d::add_frame_end_listener(on_frame_end);
    if (config::get_bool("Debug", "Census", true)) {
        hooks::install("RDR grcViewport::SetCurrent", reinterpret_cast<void*>(anchors::addr(anchors::Id::ViewportSetCurrent)),
                       hk_SetCurrent, &o_SetCurrent);
    }
    if (config::get_bool("Debug", "PushGlobalsProbe", false)) {
        hooks::install("RDR viewport push globals (probe)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ViewportPushGlobals)),
                       hk_PushGlobals, &o_PushGlobals);
    }
    if (config::get_bool("Debug", "DumpText", true)) dump_text_region("startup");
}

}  // namespace rdrvr::game_hooks
