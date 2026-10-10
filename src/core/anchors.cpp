#include "core/anchors.h"

#include <windows.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/config.h"
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
constexpr int kEntryCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

std::atomic<bool> g_verified{false};
std::atomic<bool> g_ok{false};
std::atomic<bool> g_relocated{false};
// This build's RVAs: the analysed ones, or the ones verify() found in another build (written before any hook is
// installed, read-only after).
uint32_t g_rva[kEntryCount];
bool g_absent[kEntryCount];  // an optional anchor this build lacks (relocated only): addr() is 0 for it
bool g_prechecked[kEntryCount];  // precheck(): matched before verify() (the bootstrap thread, before verify())
const bool g_rva_init = [] {
    for (int i = 0; i < kEntryCount; ++i) g_rva[i] = kEntries[i].rva;
    return true;
}();

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
// The masked pattern at s exactly (the caller keeps s..s+n inside .text)
bool masked_eq_guarded(const unsigned char* s, const unsigned char* b, const unsigned char* m, int n) {
    __try {
        for (int i = 0; i < n; ++i)
            if (m[i] && s[i] != b[i]) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_guarded(const unsigned char* p, void* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool has_fixed_run(const unsigned char* m, int n) {
    for (int k = 0; k + 3 < n; ++k)
        if (m[k] == 0xff && m[k + 1] == 0xff && m[k + 2] == 0xff && m[k + 3] == 0xff) return true;
    return false;
}
int entry_index(const char* name) {
    for (int i = 0; i < kEntryCount; ++i)
        if (std::strcmp(kEntries[i].name, name) == 0) return i;
    return -1;
}
const unsigned char* u8(const char* p) { return reinterpret_cast<const unsigned char*>(p); }

constexpr long long kNone = LLONG_MIN;

// The anchors a build may lack (0.8.1: a player's RDR.exe 0x6737882f had every other anchor, but not this one's
// bytes, and the whole mod stood down: the game ran flat). Without one, its feature is off on that build and the rest
// runs; addr() is 0 for it. Every other anchor stays required.
struct Optional {
    const char* name;
    const char* what;
};
constexpr Optional kOptional[] = {
    {"ReplayDispatch", "the batched hand-off to the playback thread ([Render] PlaybackBatch) is off on this build"},
};
const char* optional_what(const char* name) {
    for (const Optional& o : kOptional)
        if (std::strcmp(o.name, name) == 0) return o.what;
    return nullptr;
}
// [Debug] RelocateDrop (a test of the above): the named anchors (space or comma separated) taken as not found by the
// scan, as in a build without them. Debug only: with [Debug] RelocateTest=1 on the analysed build.
bool dropped(const std::string& list, const char* name) {
    const size_t n = std::strlen(name);
    for (size_t p = list.find(name); p != std::string::npos; p = list.find(name, p + 1)) {
        const bool l = p == 0 || list[p - 1] == ' ' || list[p - 1] == ',';
        const bool r = p + n == list.size() || list[p + n] == ' ' || list[p + n] == ',';
        if (l && r) return true;
    }
    return false;
}

// The scan of this build: where it keeps each anchor, to the log ([build]) and RDRVR_build_report.txt. reloc[i] gets
// entry i's shift (this build's RVA minus the analysed one) when it is found. True when every anchor is found and
// checked: the code anchors by their masked bytes at the new place, the jmp [rip] stubs by the slot they jump through,
// the globals by the instructions that address them, all globals by one shift. Reads only.
bool scan_build(long long* reloc) {
    const ULONGLONG t0 = GetTickCount64();
    const auto* img = reinterpret_cast<const unsigned char*>(base());
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const unsigned char *lo = nullptr, *hi = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            lo = img + sec[i].VirtualAddress;
            hi = lo + sec[i].Misc.VirtualSize;
        }
    for (int i = 0; i < kEntryCount; ++i) reloc[i] = kNone;
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
        return false;
    }
    // the shifts found so far: each pattern is tried at them first (a whole-.text search only when none fits), so the
    // scan of a build that differs by a few shifts takes milliseconds, not seconds (the game runs on meanwhile)
    long long cand[16] = {0};
    int ncand = 1;
    auto add_cand = [&](long long c) {
        for (int k = 0; k < ncand; ++k)
            if (cand[k] == c) return;
        if (ncand < 16) cand[ncand++] = c;
    };
    auto at_cand = [&](uint32_t rva, const unsigned char* b, const unsigned char* m, int n, long long* shift) {
        for (int k = 0; k < ncand; ++k) {
            const unsigned char* p = img + rva + cand[k];
            if (p >= lo && p + n <= hi && masked_eq_guarded(p, b, m, n)) {
                *shift = cand[k];
                return true;
            }
        }
        return false;
    };
    int full_scans = 0;
    const std::string drop = config::get_string("Debug", "RelocateDrop", "");
    if (!drop.empty()) out("[Debug] RelocateDrop: %s taken as not found (a test)", drop.c_str());

    // the code anchors: a unique pattern at a known shift or found once in .text; the others (a jmp [rip] stub, a
    // small function the build has several copies of) where their nearest found neighbour's shift puts them
    constexpr int kCount = static_cast<int>(sizeof(kSigs) / sizeof(kSigs[0]));
    static long long shift_of[kCount];
    int same = 0, moved = 0, by_neighbour = 0, lost = 0, lost_opt = 0;
    int stubs[kCount], nstubs = 0;
    for (int i = 0; i < kCount; ++i) {
        const Sig& s = kSigs[i];
        shift_of[i] = kNone;
        if (!s.unique || dropped(drop, s.name)) continue;
        long long sh = 0;
        if (!at_cand(s.rva, u8(s.b), u8(s.m), s.n, &sh)) {
            const unsigned char* at = img + s.rva;
            const unsigned char* best = nullptr;
            ++full_scans;
            if (scan_guarded(lo, hi, u8(s.b), u8(s.m), s.n, at, &best) != 1) continue;
            sh = static_cast<long long>(best - at);
            add_cand(sh);
        }
        shift_of[i] = sh;
        if (sh == 0) {
            ++same;
        } else {
            ++moved;
            out("CODE %s +%#x: moved to +%#llx (%+lld)", s.name, s.rva, static_cast<unsigned long long>(s.rva + sh), sh);
        }
    }
    for (int i = 0; i < kCount; ++i) {
        if (shift_of[i] != kNone) continue;
        const Sig& s = kSigs[i];
        int nb = -1;
        for (int j = 0; j < kCount; ++j)
            if (shift_of[j] != kNone && kSigs[j].unique &&
                (nb < 0 || std::llabs(static_cast<long long>(kSigs[j].rva) - s.rva) < std::llabs(static_cast<long long>(kSigs[nb].rva) - s.rva)))
                nb = j;
        const unsigned char* want = nb >= 0 ? img + s.rva + shift_of[nb] : nullptr;
        const bool inside = want && want >= lo && want + s.n <= hi;
        const bool fixed_run = has_fixed_run(u8(s.m), s.n);
        const bool fits = inside && fixed_run && masked_eq_guarded(want, u8(s.b), u8(s.m), s.n);
        const bool forced = dropped(drop, s.name);
        if (!forced && (fits || (inside && !fixed_run))) {
            ++by_neighbour;
            shift_of[i] = shift_of[nb];
            if (!fixed_run) stubs[nstubs++] = i;  // a stub: checked below by the slot it jumps through
            out("CODE %s +%#x: %s +%#llx (%+lld, the shift of %s +%#x)", s.name, s.rva, fits ? "matches at" : "a stub, placed at",
                static_cast<unsigned long long>(s.rva + shift_of[nb]), shift_of[nb], kSigs[nb].name, kSigs[nb].rva);
        } else {
            const char* opt = optional_what(s.name);
            ++(opt ? lost_opt : lost);
            out("CODE %s +%#x: NOT FOUND%s%s%s", s.name, s.rva, forced ? " (forced: [Debug] RelocateDrop)" : "",
                want && !forced ? " (not at its neighbour's shift either)" : "", opt ? " (optional)" : "");
            // for a later release: this build's bytes where the neighbour's shift puts it, and where the pattern's first
            // fixed bytes are near there
            if (inside) {
                const unsigned char* at = want - 16 >= lo ? want - 16 : want;
                unsigned char b[64] = {};
                const int nb2 = at + 64 <= hi ? 64 : static_cast<int>(hi - at);
                if (nb2 > 0 && read_guarded(at, b, static_cast<size_t>(nb2))) {
                    for (int h = 0; h < nb2; h += 32) {
                        char hex[3 * 32 + 1] = {};
                        for (int q = 0; q < 32 && h + q < nb2; ++q) std::snprintf(hex + q * 3, 4, "%02x ", b[h + q]);
                        out("  bytes at +%#llx: %s", static_cast<unsigned long long>(at + h - img), hex);
                    }
                }
                int k = 0;
                while (k + 3 < s.n && !(s.m[k] && s.m[k + 1] && s.m[k + 2] && s.m[k + 3])) ++k;
                if (k + 3 < s.n) {
                    const unsigned char* wlo = want - 0x400 >= lo ? want - 0x400 : lo;
                    const unsigned char* whi = want + 0x400 + 4 <= hi ? want + 0x400 + 4 : hi;
                    const unsigned char* hit = nullptr;
                    int hits = 0;
                    char where[200] = {};
                    for (const unsigned char* p = wlo; p + 4 <= whi && hits < 6;) {
                        const unsigned char* found = nullptr;
                        if (scan_guarded(p, whi, u8(s.b + k), u8(s.m + k), 4, p, &found) < 1 || !found) break;
                        if (found != hit) {
                            const size_t l = std::strlen(where);
                            std::snprintf(where + l, sizeof(where) - l, " +%#llx", static_cast<unsigned long long>(found - k - img));
                            ++hits;
                        }
                        hit = found;
                        p = found + 1;
                    }
                    out("  its first fixed bytes within 0x400 of there: %s", hits ? where : " none");
                }
            }
        }
    }
    for (int i = 0; i < kCount; ++i) {
        const int e = entry_index(kSigs[i].name);
        if (e >= 0 && shift_of[i] != kNone) reloc[e] = shift_of[i];
    }
    out("code anchors: %d where expected, %d moved, %d placed by a neighbour, %d not found, %d optional not found (of %d)", same, moved,
        by_neighbour, lost, lost_opt, kCount);

    // the globals: each reference found gives where this build keeps it (the disp32 from the instruction's end)
    int g_same = 0, g_moved = 0, g_conflict = 0, g_none = 0;
    bool d_uniform = true;
    long long d_shift = kNone;  // the one shift every global found has (else no relocation)
    const char* cur = nullptr;
    long long shift = 0;
    int agree = 0, seen = 0, total = 0;
    auto flush = [&]() {
        if (!cur) return;
        if (!seen) {
            ++g_none;
            out("DATA %s: no reference found (%d tried)", cur, total);
            return;
        }
        if (agree != seen) {
            ++g_conflict;
            out("DATA %s: the references disagree (%d of %d found, %d agree with %+lld)", cur, seen, total, agree, shift);
            return;
        }
        if (shift == 0) {
            ++g_same;
        } else {
            ++g_moved;
            out("DATA %s: moved %+lld (%d of %d references agree)", cur, shift, agree, total);
        }
        if (d_shift == kNone) d_shift = shift;
        if (shift != d_shift) d_uniform = false;
        const int e = entry_index(cur);
        if (e >= 0) reloc[e] = shift;
    };
    for (const GRef& r : kRefs) {
        if (!cur || std::strcmp(cur, r.name) != 0) {
            flush();
            cur = r.name;
            shift = 0;
            agree = seen = total = 0;
        }
        ++total;
        const unsigned char* found = nullptr;
        long long sh = 0;
        if (at_cand(r.site, u8(r.b), u8(r.m), kRefLen, &sh)) {
            found = img + r.site + sh;
        } else {
            const unsigned char* best = nullptr;
            ++full_scans;
            if (scan_guarded(lo, hi, u8(r.b), u8(r.m), kRefLen, img + r.site, &best) != 1 || !best) continue;
            found = best;
        }
        int32_t disp = 0;
        if (!read_guarded(found + r.doff, &disp, 4)) continue;
        const long long target = static_cast<long long>(found - img) + r.end + disp;
        const long long s = target - static_cast<long long>(r.g);
        if (!seen) shift = s;
        ++seen;
        if (s == shift) ++agree;
    }
    flush();
    constexpr int kNoRefs = static_cast<int>(sizeof(kNoRef) / sizeof(kNoRef[0]));
    for (const char* n : kNoRef) {
        const int e = entry_index(n);
        if (e >= 0 && d_uniform && d_shift != kNone) reloc[e] = d_shift;
        out("DATA %s: no reference pattern; %s", n, d_uniform && d_shift != kNone ? "given the shift every other global has" : "not placed");
    }
    out("data anchors: %d where expected, %d moved, %d with disagreeing references, %d unresolved, %d without a pattern%s", g_same, g_moved,
        g_conflict, g_none, kNoRefs, d_uniform ? "" : "; the globals do not all have one shift");

    // the stubs: each jmp [rip+disp32] must jump through the slot it jumps through in the analysed build, moved as the
    // globals moved
    int stubs_ok = 0;
    for (int k = 0; k < nstubs; ++k) {
        const Sig& s = kSigs[stubs[k]];
        const int e = entry_index(s.name);
        const Entry* en = e >= 0 ? &kEntries[e] : nullptr;
        unsigned char live[6] = {};
        bool ok = en && en->n >= 6 && static_cast<unsigned char>(en->bytes[0]) == 0xff && static_cast<unsigned char>(en->bytes[1]) == 0x25 &&
                  d_uniform && d_shift != kNone && reloc[e] != kNone && read_guarded(img + en->rva + reloc[e], live, 6) && live[0] == 0xff &&
                  live[1] == 0x25;
        if (ok) {
            int32_t d0 = 0, d1 = 0;
            std::memcpy(&d0, en->bytes + 2, 4);
            std::memcpy(&d1, live + 2, 4);
            const long long was = static_cast<long long>(en->rva) + 6 + d0;
            const long long now = static_cast<long long>(en->rva) + reloc[e] + 6 + d1;
            ok = now == was + d_shift;
        }
        if (ok) {
            ++stubs_ok;
        } else {
            if (e >= 0) reloc[e] = kNone;
            out("CODE %s: the stub does not jump through its slot", s.name);
        }
    }
    if (nstubs) out("stubs: %d of %d jump through their slots", stubs_ok, nstubs);

    int missing = 0, absent = 0;
    for (int i = 0; i < kEntryCount; ++i)
        if (reloc[i] == kNone) {
            if (const char* what = optional_what(kEntries[i].name)) {
                ++absent;
                out("ABSENT (optional): %s: %s", kEntries[i].name, what);
            } else {
                ++missing;
                out("NOT PLACED: %s", kEntries[i].name);
            }
        }
    const bool can = !lost && !g_conflict && !g_none && d_uniform && stubs_ok == nstubs && !missing;
    char without[64] = {};
    if (absent) std::snprintf(without, sizeof(without), ", without %d optional anchor%s", absent, absent > 1 ? "s" : "");
    out("verdict: %s%s (%d full searches, %.0f ms)",
        can ? (moved || by_neighbour || g_moved || absent ? "every required anchor found and checked: this build runs on the found addresses"
                                                          : "every anchor in place")
            : "some anchors were not found or checked: this build cannot be relocated",
        can ? without : "", full_scans, static_cast<double>(GetTickCount64() - t0));
    if (f) std::fclose(f);
    return can;
}

}  // namespace

void build_report() {
    static long long reloc[kEntryCount];
    scan_build(reloc);
}

uintptr_t base() {
    static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}

uint32_t rva(Id id) { return g_rva[static_cast<int>(id)]; }
const char* name(Id id) { return kEntries[static_cast<int>(id)].name; }
uintptr_t addr(Id id) { return g_absent[static_cast<int>(id)] ? 0 : base() + rva(id); }
bool present(Id id) { return !g_absent[static_cast<int>(id)]; }

bool precheck(Id id) {
    const int i = static_cast<int>(id);
    const Entry& e = kEntries[i];
    if (g_verified.load() || !exe_matches() || e.kind == Kind::Global) return false;
    if (std::memcmp(reinterpret_cast<const unsigned char*>(base() + e.rva), e.bytes, e.n) != 0) {
        log::error("[anchors] precheck: %s at +%#x differs from research\\RDR.exe", e.name, e.rva);
        return false;
    }
    g_prechecked[i] = true;
    return true;
}

bool exe_matches() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base() + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp == RDRVR_EXE_TIMESTAMP && nt->OptionalHeader.SizeOfImage == RDRVR_EXE_SIZEOFIMAGE;
}

bool relocated() { return g_relocated.load(); }

bool verify() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base() + dos->e_lfanew);
    log::info("[anchors] RDR.exe base %p, TimeDateStamp %#lx, SizeOfImage %#lx (expected %#x / %#x)",
              reinterpret_cast<void*>(base()), nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
              RDRVR_EXE_TIMESTAMP, RDRVR_EXE_SIZEOFIMAGE);
    bool ok = exe_matches();
    if (!ok) log::error("[anchors] this RDR.exe is not the analysed build (v42_PC-49788435): looking for its anchors");

    int bad = 0, checked = 0;
    for (const Entry& e : kEntries) {
        if (e.kind == Kind::Global) continue;
        ++checked;
        if (g_prechecked[&e - kEntries]) continue;  // checked before it was hooked (precheck)
        const auto* p = reinterpret_cast<const unsigned char*>(base() + e.rva);
        if (std::memcmp(p, e.bytes, e.n) != 0) {
            char got[64] = {};
            for (uint32_t i = 0; i < e.n && i < 16; ++i) std::snprintf(got + i * 3, 4, "%02x ", p[i]);
            log::error("[anchors] MISMATCH %s at +%#x: got %s", e.name, e.rva, got);
            ++bad;
        }
    }
    if (bad) {
        log::error("[anchors] %d of %d code anchors differ from research\\RDR.exe", bad, checked);
        ok = false;
    } else {
        log::info("[anchors] all %d code anchors match", checked);
    }
    // another build (or [Debug] RelocateTest on this one): where it keeps the anchors; when every one is found and
    // checked, the mod runs on those addresses ([Debug] Relocate), else it stands down
    if (!ok || config::get_bool("Debug", "RelocateTest", false)) {
        static long long reloc[kEntryCount];
        const bool can = scan_build(reloc);
        if (can && config::get_bool("Debug", "Relocate", true)) {
            int moved = 0, absent = 0;
            for (int i = 0; i < kEntryCount; ++i) {
                g_absent[i] = reloc[i] == kNone;  // scan_build accepts that only for an optional anchor
                if (g_absent[i]) {
                    ++absent;
                    continue;
                }
                g_rva[i] = static_cast<uint32_t>(static_cast<long long>(kEntries[i].rva) + reloc[i]);
                moved += reloc[i] != 0;
            }
            g_relocated.store(true);
            ok = true;
            log::info("[anchors] RELOCATED: running on this build's addresses (%d of %d anchors moved, %d optional absent; "
                      "RDRVR_build_report.txt)",
                      moved, kEntryCount, absent);
            for (int i = 0; i < kEntryCount; ++i)
                if (g_absent[i]) log::warn("[anchors] %s is not in this build: %s", kEntries[i].name, optional_what(kEntries[i].name));
        } else {
            ok = false;
            log::error("[anchors] STAND DOWN: %s", can ? "this build could run relocated, but [Debug] Relocate=0"
                                                       : "this build's anchors were not all found (RDRVR_build_report.txt)");
        }
    }
    g_ok.store(ok);
    g_verified.store(true);
    return ok;
}

bool stand_down() { return !g_verified.load() || !g_ok.load(); }

}  // namespace rdrvr::anchors
