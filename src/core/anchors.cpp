#include "core/anchors.h"

#include <windows.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

namespace rdrvr::anchors {
namespace {

enum class Kind { Func, Site, Global };

struct Entry {
    const char* name;
    Kind kind;
    uint32_t rva;
    uint32_t n;
    const char* bytes;
};

constexpr Entry kEntries[] = {
#define ANCHOR(name, kind, rva, n, bytes) {#name, Kind::kind, rva, n, bytes},
#include "core/anchors.inc"
#undef ANCHOR
};

std::atomic<bool> g_verified{false};
std::atomic<bool> g_ok{false};

// ---- the build report (another build of RDR.exe): what tools/gen_buildsig.py took from the analysed one
struct Sig {
    const char* name;
    uint32_t rva;
    int n;  // 32, 64 or 128: the shortest unique in the analysed build
    const char* b;
    const char* m;
    int unique;  // unique in the analysed build (else only a neighbour places it)
};
struct GRef {
    const char* name;
    uint32_t g;
    uint32_t site;
    int doff;  // the disp32's offset in the pattern
    int end;   // the instruction's end in the pattern (the disp32 counts from there)
    const char* b;
    const char* m;
};
constexpr int kRefLen = 24;
#define SIG(name, rva, n, b, m, u) {#name, rva, n, b, m, u},
#define GREF(name, g, site, doff, end, b, m)
#define GNONE(name, g, refs)
constexpr Sig kSigs[] = {
#include "core/anchors_sig.inc"
};
#undef SIG
#undef GREF
#undef GNONE
#define SIG(name, rva, n, b, m, u)
#define GNONE(name, g, refs)
#define GREF(name, g, site, doff, end, b, m) {#name, g, site, doff, end, b, m},
constexpr GRef kRefs[] = {
#include "core/anchors_sig.inc"
};
#undef GREF
#undef GNONE
#define GREF(name, g, site, doff, end, b, m)
#define GNONE(name, g, refs) #name,
constexpr const char* kNoRef[] = {
#include "core/anchors_sig.inc"
};
#undef SIG
#undef GREF
#undef GNONE

// The masked pattern's matches in [lo, hi): how many (up to 9) and the one nearest `want`. No C++ objects (SEH).
int scan(const unsigned char* lo, const unsigned char* hi, const unsigned char* b, const unsigned char* m, int n,
         const unsigned char* want, const unsigned char** best) {
    int k = 0;
    while (k + 3 < n && !(m[k] == 0xff && m[k + 1] == 0xff && m[k + 2] == 0xff && m[k + 3] == 0xff)) ++k;
    if (k + 3 >= n) return -1;  // no fixed run to look for
    int found = 0;
    *best = nullptr;
    const unsigned char* p = lo + k;
    while (p < hi - (n - k)) {
        p = static_cast<const unsigned char*>(std::memchr(p, b[k], static_cast<size_t>(hi - (n - k) - p)));
        if (!p) break;
        const unsigned char* s = p - k;
        if (p[1] == b[k + 1] && p[2] == b[k + 2] && p[3] == b[k + 3]) {
            bool ok = true;
            for (int i = 0; i < n && ok; ++i) ok = !m[i] || s[i] == b[i];
            if (ok) {
                ++found;
                if (!*best || (s > want ? s - want : want - s) < (*best > want ? *best - want : want - *best)) *best = s;
                if (found >= 9) break;
            }
        }
        ++p;
    }
    return found;
}
int scan_guarded(const unsigned char* lo, const unsigned char* hi, const unsigned char* b, const unsigned char* m, int n,
                 const unsigned char* want, const unsigned char** best) {
    __try {
        return scan(lo, hi, b, m, n, want, best);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

}  // namespace

// The report: each code anchor and each global, in this build, against the analysed one
void build_report() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const unsigned char *lo = nullptr, *hi = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            lo = reinterpret_cast<const unsigned char*>(base() + sec[i].VirtualAddress);
            hi = lo + sec[i].Misc.VirtualSize;
        }
    wchar_t path[MAX_PATH];
    log::path_in_game_dir(L"RDRVR_build_report.txt", path, MAX_PATH);
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"w");
    auto out = [&](const char* fmt, auto... a) {
        char line[400];
        std::snprintf(line, sizeof(line), fmt, a...);
        log::info("[build] %s", line);
        if (f) std::fprintf(f, "%s\n", line);
    };
    out("RDRVR build report: RDR.exe TimeDateStamp %#lx SizeOfImage %#lx (analysed %#x / %#x); .text %p..%p",
        nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, RDRVR_EXE_TIMESTAMP, RDRVR_EXE_SIZEOFIMAGE, lo, hi);
    if (!lo) {
        out("no .text section found");
        if (f) std::fclose(f);
        return;
    }
    // the code anchors: each found once in .text gives its shift; the others (a stub, a small function found several
    // times, one that changed) are tried where their nearest found neighbour's shift puts them
    constexpr int kCount = static_cast<int>(sizeof(kSigs) / sizeof(kSigs[0]));
    constexpr long long kNone = LLONG_MIN;
    static long long shift_of[kCount];
    static int matches[kCount];
    int same = 0, moved = 0, by_neighbour = 0, lost = 0;
    for (int i = 0; i < kCount; ++i) {
        const Sig& s = kSigs[i];
        const unsigned char* at = reinterpret_cast<const unsigned char*>(base() + s.rva);
        const unsigned char* best = nullptr;
        matches[i] = scan_guarded(lo, hi, reinterpret_cast<const unsigned char*>(s.b), reinterpret_cast<const unsigned char*>(s.m), s.n, at, &best);
        shift_of[i] = matches[i] == 1 ? static_cast<long long>(best - at) : kNone;
        if (shift_of[i] == 0) {
            ++same;
        } else if (shift_of[i] != kNone) {
            ++moved;
            out("CODE %s +%#x: moved to +%#llx (%+lld)", s.name, s.rva, static_cast<unsigned long long>(s.rva + shift_of[i]), shift_of[i]);
        }
    }
    for (int i = 0; i < kCount; ++i) {
        if (shift_of[i] != kNone) continue;
        const Sig& s = kSigs[i];
        int nb = -1;
        for (int j = 0; j < kCount; ++j)
            if (shift_of[j] != kNone && matches[j] == 1 &&
                (nb < 0 || std::llabs(static_cast<long long>(kSigs[j].rva) - s.rva) < std::llabs(static_cast<long long>(kSigs[nb].rva) - s.rva)))
                nb = j;
        const unsigned char* want = nb >= 0 ? reinterpret_cast<const unsigned char*>(base() + s.rva + shift_of[nb]) : nullptr;
        const unsigned char* best = nullptr;
        const bool fits = want && want >= lo && want + s.n <= hi &&
                          scan_guarded(want, want + s.n + 1, reinterpret_cast<const unsigned char*>(s.b), reinterpret_cast<const unsigned char*>(s.m), s.n, want, &best) == 1;
        const bool fixed_run = matches[i] != -1;  // -1: the pattern has no 4 fixed bytes (a jmp [rip] stub): only the neighbour can place it
        if (fits || (!fixed_run && want)) {
            ++by_neighbour;
            out("CODE %s +%#x: %s +%#llx (%+lld, the shift of %s +%#x; %d matches in .text)", s.name, s.rva,
                fits ? "matches at" : "unverifiable, placed at", static_cast<unsigned long long>(s.rva + shift_of[nb]), shift_of[nb],
                kSigs[nb].name, kSigs[nb].rva, matches[i]);
        } else {
            ++lost;
            out("CODE %s +%#x: NOT FOUND (%d matches in .text%s)", s.name, s.rva, matches[i], want ? "; not at its neighbour's shift either" : "");
        }
    }
    out("code anchors: %d where expected, %d moved, %d placed by a neighbour, %d not found (of %d)", same, moved, by_neighbour, lost, kCount);
    // the globals: each reference found gives where this build keeps it (the disp32 from the instruction's end)
    int g_same = 0, g_moved = 0, g_conflict = 0, g_none = 0;
    const char* cur = nullptr;
    long long shift = 0;
    int agree = 0, seen = 0, total = 0;
    auto flush = [&]() {
        if (!cur) return;
        if (!seen) {
            ++g_none;
            out("DATA %s: no reference found (%d tried)", cur, total);
        } else if (agree == seen && shift == 0) {
            ++g_same;
        } else if (agree == seen) {
            ++g_moved;
            out("DATA %s: moved %+lld (%d of %d references agree)", cur, shift, agree, total);
        } else {
            ++g_conflict;
            out("DATA %s: the references disagree (%d of %d found, %d agree with %+lld)", cur, seen, total, agree, shift);
        }
    };
    for (const GRef& r : kRefs) {
        if (!cur || std::strcmp(cur, r.name) != 0) {
            flush();
            cur = r.name;
            shift = 0;
            agree = seen = total = 0;
        }
        ++total;
        const unsigned char* at = reinterpret_cast<const unsigned char*>(base() + r.site);
        const unsigned char* best = nullptr;
        const int n = scan_guarded(lo, hi, reinterpret_cast<const unsigned char*>(r.b), reinterpret_cast<const unsigned char*>(r.m), kRefLen, at, &best);
        if (n != 1 || !best) continue;
        int32_t disp = 0;
        std::memcpy(&disp, best + r.doff, 4);
        const long long target = static_cast<long long>(best - reinterpret_cast<const unsigned char*>(base())) + r.end + disp;
        const long long s = target - static_cast<long long>(r.g);
        if (!seen) shift = s;
        ++seen;
        if (s == shift) ++agree;
    }
    flush();
    constexpr int kNoRefs = static_cast<int>(sizeof(kNoRef) / sizeof(kNoRef[0]));
    for (const char* n : kNoRef) out("DATA %s: no reference pattern in the analysed build (to be found by hand)", n);
    out("data anchors: %d where expected, %d moved, %d with disagreeing references, %d unresolved, %d without a pattern", g_same, g_moved,
        g_conflict, g_none, kNoRefs);
    out("verdict: %s", lost || g_conflict || g_none ? "some anchors were not found: this build cannot be relocated from this report alone"
                       : moved || g_moved ? "every anchor with a pattern found: this build could be supported by relocation"
                                          : "every anchor with a pattern in place");
    if (f) std::fclose(f);
}

uintptr_t base() {
    static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}

uint32_t rva(Id id) { return kEntries[static_cast<int>(id)].rva; }
const char* name(Id id) { return kEntries[static_cast<int>(id)].name; }
uintptr_t addr(Id id) { return base() + rva(id); }

bool exe_matches() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base() + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp == RDRVR_EXE_TIMESTAMP && nt->OptionalHeader.SizeOfImage == RDRVR_EXE_SIZEOFIMAGE;
}

bool verify() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base() + dos->e_lfanew);
    log::info("[anchors] RDR.exe base %p, TimeDateStamp %#lx, SizeOfImage %#lx (expected %#x / %#x)",
              reinterpret_cast<void*>(base()), nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
              RDRVR_EXE_TIMESTAMP, RDRVR_EXE_SIZEOFIMAGE);
    bool ok = exe_matches();
    if (!ok) log::error("[anchors] STAND DOWN: this RDR.exe is not the analysed build (v42_PC-49788435)");

    int bad = 0, checked = 0;
    for (const Entry& e : kEntries) {
        if (e.kind == Kind::Global) continue;
        ++checked;
        const auto* p = reinterpret_cast<const unsigned char*>(base() + e.rva);
        if (std::memcmp(p, e.bytes, e.n) != 0) {
            char got[64] = {};
            for (uint32_t i = 0; i < e.n && i < 16; ++i) std::snprintf(got + i * 3, 4, "%02x ", p[i]);
            log::error("[anchors] MISMATCH %s at +%#x: got %s", e.name, e.rva, got);
            ++bad;
        }
    }
    if (bad) {
        log::error("[anchors] STAND DOWN: %d of %d code anchors differ from research\\RDR.exe", bad, checked);
        ok = false;
    } else {
        log::info("[anchors] all %d code anchors match", checked);
    }
    if (!ok) build_report();  // another build: where its anchors are (reads only; the mod stays stood down)
    g_ok.store(ok);
    g_verified.store(true);
    return ok;
}

bool stand_down() { return !g_verified.load() || !g_ok.load(); }

}  // namespace rdrvr::anchors
