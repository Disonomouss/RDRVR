#include "core/image_mem.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/anchors.h"
#include "core/log.h"

namespace rdrvr::image_mem {
namespace {

struct Range {
    uintptr_t lo = 0, hi = 0;
};
uintptr_t g_img_lo = 0, g_img_hi = 0;
Range g_sec_data, g_sec_rdata;  // from the headers
std::once_flag g_parse_once;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_fast_on{true};

std::atomic<uint64_t> g_calls[kSrcCount], g_ticks[kSrcCount], g_hits{0};
std::atomic<uint64_t> g_ws_calls{0}, g_ws_ticks{0}, g_ws_absent{0}, g_ws_other{0};
constexpr int kSlots = 3;  // 0 the [xr] timing window, 1 the perf window, 2 "skel vq"
// The slowest per slot: its ticks read racily (the hot path's test), the pair written and taken under the slot's lock
// (taken only by a new slowest, or a take)
std::atomic<uint64_t> g_max_ticks[kSlots];
uintptr_t g_max_addr[kSlots] = {};
SRWLOCK g_max_lock[kSlots] = {SRWLOCK_INIT, SRWLOCK_INIT, SRWLOCK_INIT};
double g_us_per_tick = 0.0;
constexpr int kMaxPages = 8;  // the fast path's longest range (a skeleton's matrices: 0x40 a bone, a few pages)

// The loaded image's own section table (GetModuleHandle(nullptr): the headers as mapped)
void parse() {
    std::call_once(g_parse_once, [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        g_us_per_tick = f.QuadPart ? 1e6 / static_cast<double>(f.QuadPart) : 0.0;
        auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_img_lo = reinterpret_cast<uintptr_t>(base);
        g_img_hi = g_img_lo + nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            Range* r = std::memcmp(sec[i].Name, ".data\0\0", 8) == 0    ? &g_sec_data
                       : std::memcmp(sec[i].Name, ".rdata\0", 8) == 0 ? &g_sec_rdata
                                                                         : nullptr;
            if (!r || r->lo) continue;
            r->lo = g_img_lo + sec[i].VirtualAddress;
            r->hi = r->lo + sec[i].Misc.VirtualSize;
        }
    });
}

// Readable at all (any read protection, committed, not a guard or no-access page): the vq report's class
bool readable_region(const MEMORY_BASIC_INFORMATION& m) {
    return m.State == MEM_COMMIT && (m.Protect & kReadAny) && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

uint64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

const char* rva_or_abs(uintptr_t a, char* out, size_t len) {
    if (a >= g_img_lo && a < g_img_hi) std::snprintf(out, len, "RDR.exe+%#llx", static_cast<unsigned long long>(a - g_img_lo));
    else std::snprintf(out, len, "%#llx", static_cast<unsigned long long>(a));
    return out;
}

const char* state_name(DWORD s) { return s == MEM_COMMIT ? "commit" : s == MEM_RESERVE ? "reserve" : s == MEM_FREE ? "free" : "?"; }
const char* type_name(DWORD t) { return t == MEM_IMAGE ? "image" : t == MEM_MAPPED ? "mapped" : t == MEM_PRIVATE ? "private" : "-"; }

// One region, as the report lists it: "+rva size state protect/allocation protect type"
void region_text(const MEMORY_BASIC_INFORMATION& m, char* out, size_t len) {
    std::snprintf(out, len, "+%#llx %.0fK %s %#lx/%#lx %s%s", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(m.BaseAddress) - g_img_lo),
                  m.RegionSize / 1024.0, state_name(m.State), static_cast<unsigned long>(m.Protect),
                  static_cast<unsigned long>(m.AllocationProtect), type_name(m.Type), readable_region(m) ? "" : " UNREADABLE");
}

}  // namespace

void init() {
    parse();
    // .data's and .rdata's first regions as the bootstrap sees them (r236: init's walk counted 1 region of .data and 2
    // of .rdata before it refused them; the cycle's later walk found 904 in .data): the log's record of the start
    for (const Range* r : {&g_sec_data, &g_sec_rdata}) {
        std::string line;
        MEMORY_BASIC_INFORMATION m{};
        int k = 0;
        for (uintptr_t a = r->lo; a && a < r->hi && k < 4; a = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize, ++k) {
            if (VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m)) != sizeof(m) || !m.RegionSize) break;
            char t[120];
            region_text(m, t, sizeof(t));
            line += (k ? "; " : "") + std::string(t);
        }
        log::info("[vq] at init, %s's first regions: %s", r == &g_sec_data ? ".data" : ".rdata", line.c_str());
    }
    g_ready = true;
    log::info("[vq] the checked reads' fast path: the working set's page check inside RDR.exe's image (on)");
}

void image(uintptr_t* lo, uintptr_t* hi) {
    parse();
    *lo = g_img_lo, *hi = g_img_hi;
}

void section(const char* name, uintptr_t* lo, uintptr_t* hi) {
    parse();
    const Range* r = std::strcmp(name, ".data") == 0 ? &g_sec_data : std::strcmp(name, ".rdata") == 0 ? &g_sec_rdata : nullptr;
    *lo = r ? r->lo : 0, *hi = r ? r->hi : 0;
}

bool fast_readable(uintptr_t a, size_t n, unsigned mask) {
    if (!a || !n || !g_fast_on.load(std::memory_order_relaxed) || !g_ready.load(std::memory_order_acquire)) return false;
    if (a < g_img_lo || a >= g_img_hi || n > g_img_hi - a) return false;  // (no a + n overflow)
    const uintptr_t p0 = a & ~uintptr_t{0xfff}, p1 = (a + n - 1) & ~uintptr_t{0xfff};
    const size_t pages = static_cast<size_t>((p1 - p0) >> 12) + 1;
    if (pages > kMaxPages) return false;
    PSAPI_WORKING_SET_EX_INFORMATION ws[kMaxPages];
    for (size_t i = 0; i < pages; ++i) {
        ws[i].VirtualAddress = reinterpret_cast<void*>(p0 + (i << 12));
        ws[i].VirtualAttributes.Flags = 0;
    }
    const uint64_t t0 = qpc();
    const BOOL ok = K32QueryWorkingSetEx(GetCurrentProcess(), ws, static_cast<DWORD>(pages * sizeof(ws[0])));
    g_ws_ticks.fetch_add(qpc() - t0, std::memory_order_relaxed);
    g_ws_calls.fetch_add(1, std::memory_order_relaxed);
    if (!ok) return false;
    for (size_t i = 0; i < pages; ++i) {
        const auto& v = ws[i].VirtualAttributes;
        if (!v.Valid) {  // paged out or never touched: VirtualQuery says
            g_ws_absent.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const unsigned prot = static_cast<unsigned>(v.Win32Protection);
        if (v.Bad || (prot & PAGE_GUARD) || !(prot & mask & 0xffu)) {  // present but not readable for this check
            g_ws_other.fetch_add(1, std::memory_order_relaxed);
            char w[64];
            log::limited("vq.ws.other", 16, "[vq] regions changed: a checked page at %s present with protection %#x (bad %u), not "
                         "readable for this check (mask %#x): VirtualQuery decides",
                         rva_or_abs(p0 + (i << 12), w, sizeof(w)), prot, static_cast<unsigned>(v.Bad), mask);
            return false;
        }
    }
    g_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool fast_on() { return g_fast_on.load(); }
void set_fast(bool on) {
    g_fast_on = on;
    log::info("[vq] the fast path %s (the session)", on ? "on" : "off");
}

bool query(uintptr_t a, void* mbi, Src src) {
    const uint64_t t0 = qpc();
    const bool ok = VirtualQuery(reinterpret_cast<void*>(a), static_cast<MEMORY_BASIC_INFORMATION*>(mbi), sizeof(MEMORY_BASIC_INFORMATION)) ==
                    sizeof(MEMORY_BASIC_INFORMATION);
    const uint64_t d = qpc() - t0;
    g_calls[src].fetch_add(1, std::memory_order_relaxed);
    g_ticks[src].fetch_add(d, std::memory_order_relaxed);
    for (int s = 0; s < kSlots; ++s)
        if (d > g_max_ticks[s].load(std::memory_order_relaxed)) {
            AcquireSRWLockExclusive(&g_max_lock[s]);
            if (d > g_max_ticks[s].load(std::memory_order_relaxed)) {  // (again under the lock: the pair stays one call's)
                g_max_ticks[s].store(d, std::memory_order_relaxed);
                g_max_addr[s] = a;
            }
            ReleaseSRWLockExclusive(&g_max_lock[s]);
        }
    return ok;
}

void snap(Snap* s) {
    for (int i = 0; i < kSrcCount; ++i) s->calls[i] = g_calls[i].load(), s->ticks[i] = g_ticks[i].load();
    s->fast = g_hits.load();
    s->ws_calls = g_ws_calls.load(), s->ws_ticks = g_ws_ticks.load(), s->ws_absent = g_ws_absent.load(), s->ws_other = g_ws_other.load();
}

void take_slowest(int slot, double* us, uintptr_t* addr, bool* in_image) {
    parse();
    slot = slot < 0 ? 0 : slot >= kSlots ? kSlots - 1 : slot;
    AcquireSRWLockExclusive(&g_max_lock[slot]);
    *us = static_cast<double>(g_max_ticks[slot].exchange(0)) * g_us_per_tick;
    *addr = g_max_addr[slot];
    g_max_addr[slot] = 0;
    ReleaseSRWLockExclusive(&g_max_lock[slot]);
    *in_image = *addr >= g_img_lo && *addr < g_img_hi;
}

double ticks_us(uint64_t ticks) {
    parse();
    return static_cast<double>(ticks) * g_us_per_tick;
}

std::string format(const Snap& a, const Snap& b, uint64_t frames, int slot, bool* quiet) {
    const double n = frames ? static_cast<double>(frames) : 1.0;
    double us = 0;
    uintptr_t at = 0;
    bool img = false;
    take_slowest(slot, &us, &at, &img);
    if (quiet) *quiet = b.calls[kBody] == a.calls[kBody] && b.calls[kD3d] == a.calls[kD3d] && us < 1000.0;
    char w[64] = "-";
    if (at) rva_or_abs(at, w, sizeof(w));
    const uint64_t wc = b.ws_calls - a.ws_calls;
    char out[480];
    std::snprintf(out, sizeof(out),
                  "vq %llu frames, per frame: body %.1f calls %.1f us, d3d %.1f calls %.1f us, fast hits %.1f (working set %.1f calls %.1f "
                  "us, %.2f us a call; pages absent %.2f, of another protection %.2f) | slowest %.0f us at %s | fast path %s",
                  static_cast<unsigned long long>(frames), (b.calls[kBody] - a.calls[kBody]) / n, ticks_us(b.ticks[kBody] - a.ticks[kBody]) / n,
                  (b.calls[kD3d] - a.calls[kD3d]) / n, ticks_us(b.ticks[kD3d] - a.ticks[kD3d]) / n, (b.fast - a.fast) / n, wc / n,
                  ticks_us(b.ws_ticks - a.ws_ticks) / n, wc ? ticks_us(b.ws_ticks - a.ws_ticks) / static_cast<double>(wc) : 0.0,
                  (b.ws_absent - a.ws_absent) / n, (b.ws_other - a.ws_other) / n, us, w, fast_on() ? "on" : "off");
    return out;
}

std::string report() {
    parse();
    std::string out;
    char b[600];
    // (a) one walk of .data: each region a run of one protection; every region of another protection than RW or
    // WRITECOPY listed (r236: 29 of them, the old walk's "not committed readable" counted executable ones too)
    {
        const double t0 = log::now_ms();
        int regions = 0, rw = 0, wc = 0, other = 0, unreadable = 0;
        size_t longest = 0, rw_bytes = 0, other_bytes = 0;
        uintptr_t longest_at = 0;
        DWORD longest_prot = 0;
        double slowest = 0;
        uint32_t sig = 2166136261u;  // FNV-1a over the other regions' (rva, size, state, protect): two walks compared
        auto mix = [&](uint64_t v) {
            for (int i = 0; i < 8; ++i) sig = (sig ^ static_cast<uint8_t>(v >> (i * 8))) * 16777619u;
        };
        std::string listed, logged;
        int nlogged = 0;
        MEMORY_BASIC_INFORMATION m{};
        for (uintptr_t a = g_sec_data.lo; a && a < g_sec_data.hi && regions < 1000000;
             a = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize) {
            const uint64_t q0 = qpc();
            if (VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m)) != sizeof(m) || !m.RegionSize) break;
            slowest = (std::max)(slowest, ticks_us(qpc() - q0));
            ++regions;
            const DWORD p = m.Protect & 0xff;
            const bool plain = m.State == MEM_COMMIT && !(m.Protect & ~0xffu) && (p == PAGE_READWRITE || p == PAGE_WRITECOPY);
            if (plain && p == PAGE_READWRITE) ++rw, rw_bytes += m.RegionSize;
            else if (plain) ++wc;
            if (!readable_region(m)) ++unreadable;
            if (!plain) {
                ++other;
                other_bytes += m.RegionSize;
                mix(reinterpret_cast<uintptr_t>(m.BaseAddress) - g_img_lo), mix(m.RegionSize), mix(m.State), mix(m.Protect);
                char t[120];
                region_text(m, t, sizeof(t));
                if (other <= 24) listed += std::string(other > 1 ? "; " : "") + t;
                logged += std::string(nlogged ? "; " : "") + t;
                if (++nlogged == 8) {
                    log::info("[vq] .data's other regions: %s", logged.c_str());
                    logged.clear(), nlogged = 0;
                }
            }
            if (m.RegionSize > longest) {
                longest = m.RegionSize;
                longest_at = reinterpret_cast<uintptr_t>(m.BaseAddress);
                longest_prot = m.Protect;
            }
        }
        if (nlogged) log::info("[vq] .data's other regions: %s", logged.c_str());
        std::snprintf(b, sizeof(b),
                      "vq .data (RDR.exe+%#llx, %.0f MB): %d regions, %d PAGE_READWRITE runs (%.1f MB), %d PAGE_WRITECOPY runs, %d other "
                      "(%.1f MB, signature %08x), %d unreadable; the longest %.1f MB at RDR.exe+%#llx (protect %#lx); the walk %.1f ms, its "
                      "slowest call %.0f us",
                      static_cast<unsigned long long>(g_sec_data.lo - g_img_lo), (g_sec_data.hi - g_sec_data.lo) / 1048576.0, regions, rw,
                      rw_bytes / 1048576.0, wc, other, other_bytes / 1048576.0, sig, unreadable, longest / 1048576.0,
                      static_cast<unsigned long long>(longest_at ? longest_at - g_img_lo : 0), static_cast<unsigned long>(longest_prot),
                      log::now_ms() - t0, slowest);
        out += b;
        out += " | other regions (the first 24): " + (listed.empty() ? std::string("none") : listed);
    }
    // (b) the per-frame anchors' globals: a timed VirtualQuery (the median of 5) and its RegionSize, a timed
    // QueryWorkingSetEx (the median of 5) and what it says of the page
    {
        struct A {
            const char* name;
            anchors::Id id;
        } const as[] = {{"ObjectsPool", anchors::Id::ObjectsPool}, {"ActorPool", anchors::Id::ActorPool},
                        {"ViewportCurrent", anchors::Id::ViewportCurrent}};
        for (const A& x : as) {
            const uintptr_t a = anchors::addr(x.id);
            double t[5] = {}, tw[5] = {};
            MEMORY_BASIC_INFORMATION m{};
            PSAPI_WORKING_SET_EX_INFORMATION ws{};
            bool ok = a != 0, wok = a != 0;
            for (int i = 0; i < 5 && ok; ++i) {
                const uint64_t q0 = qpc();
                ok = VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m)) == sizeof(m);
                t[i] = ticks_us(qpc() - q0);
            }
            for (int i = 0; i < 5 && wok; ++i) {
                ws.VirtualAddress = reinterpret_cast<void*>(a & ~uintptr_t{0xfff});
                ws.VirtualAttributes.Flags = 0;
                const uint64_t q0 = qpc();
                wok = K32QueryWorkingSetEx(GetCurrentProcess(), &ws, sizeof(ws)) != 0;
                tw[i] = ticks_us(qpc() - q0);
            }
            std::sort(t, t + 5);
            std::sort(tw, tw + 5);
            char w[64];
            if (ok)
                std::snprintf(b, sizeof(b),
                              " | %s at %s: VirtualQuery median %.1f us (max %.1f), RegionSize %.2f MB, protect %#lx; the working set %s "
                              "median %.1f us: %s, protection %#x",
                              x.name, rva_or_abs(a, w, sizeof(w)), t[2], t[4], m.RegionSize / 1048576.0, static_cast<unsigned long>(m.Protect),
                              wok ? "asked" : "FAILED", tw[2], ws.VirtualAttributes.Valid ? "present" : "absent",
                              static_cast<unsigned>(ws.VirtualAttributes.Win32Protection));
            else
                std::snprintf(b, sizeof(b), " | %s: not queried", x.name);
            out += b;
        }
    }
    // (c) the process's regions
    {
        const double t0 = log::now_ms();
        int regions = 0, committed = 0;
        MEMORY_BASIC_INFORMATION m{};
        for (uintptr_t a = 0; a < 0x7FFFFFFF0000ull && regions < 4000000; a = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize) {
            if (VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m)) != sizeof(m) || !m.RegionSize) break;
            ++regions;
            if (m.State == MEM_COMMIT) ++committed;
        }
        std::snprintf(b, sizeof(b), " | the process: %d regions (%d committed), the walk %.1f ms", regions, committed, log::now_ms() - t0);
        out += b;
    }
    Snap s{};
    snap(&s);
    std::snprintf(b, sizeof(b),
                  " | fast path %s; since start: body %llu calls %.0f ms, d3d %llu calls %.0f ms, fast hits %llu, working set %llu calls "
                  "%.0f ms (pages absent %llu, of another protection %llu)",
                  fast_on() ? "on" : "off", static_cast<unsigned long long>(s.calls[kBody]), ticks_us(s.ticks[kBody]) / 1000.0,
                  static_cast<unsigned long long>(s.calls[kD3d]), ticks_us(s.ticks[kD3d]) / 1000.0, static_cast<unsigned long long>(s.fast),
                  static_cast<unsigned long long>(s.ws_calls), ticks_us(s.ws_ticks) / 1000.0, static_cast<unsigned long long>(s.ws_absent),
                  static_cast<unsigned long long>(s.ws_other));
    out += b;
    log::info("[vq] %s", out.c_str());
    return out;
}

}  // namespace rdrvr::image_mem
