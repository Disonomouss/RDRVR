#include "core/diag.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/log.h"
#include "core/state.h"

namespace rdrvr::diag {

namespace {
struct ImageRange {
    uintptr_t lo = 0, hi = 0, rdata_lo = 0, rdata_hi = 0;
};
ImageRange image_range() {
    ImageRange r;
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    r.lo = reinterpret_cast<uintptr_t>(base);
    r.hi = r.lo + nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (std::memcmp(sec[i].Name, ".rdata", 6) == 0) {
            r.rdata_lo = r.lo + sec[i].VirtualAddress;
            r.rdata_hi = r.rdata_lo + sec[i].Misc.VirtualSize;
        }
    return r;
}
}  // namespace

// A thread stack is one reservation with a guard page between its reserved and committed parts.
bool is_stack(const void* allocation_base) {
    MEMORY_BASIC_INFORMATION m{};
    for (auto* a = static_cast<const char*>(allocation_base);
         VirtualQuery(a, &m, sizeof(m)) == sizeof(m) && m.AllocationBase == allocation_base; a += m.RegionSize)
        if (m.State == MEM_COMMIT && (m.Protect & PAGE_GUARD)) return true;
    return false;
}

void find_pattern(const void* pattern, size_t n, const char* what, int max_hits) {
    const ImageRange img = image_range();
    const auto* pat = static_cast<const uint8_t*>(pattern);
    double t0 = log::now_ms();
    struct Group {
        const void* base;
        size_t size;
        int count;
        uintptr_t first;
    };
    std::vector<Group> groups;
    int image_hits = 0, stack_groups = 0;
    uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull; a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State != MEM_COMMIT || !(mbi.Protect & rw) || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) ||
            (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_IMAGE))
            continue;
        const auto* p = static_cast<const uint8_t*>(mbi.BaseAddress);
        size_t len = mbi.RegionSize;
        if (pat >= p && pat < p + len) continue;  // the caller's own copy
        scanned += len;
        for (size_t i = 0; i + n <= len; i += 4) {
            if (std::memcmp(p + i, pat, n) != 0) continue;
            uintptr_t at = reinterpret_cast<uintptr_t>(p + i);
            if (at >= img.lo && at < img.hi) {
                if (image_hits++ < max_hits)
                    log::info("[findcam] %s at RDR.exe+%#llx", what, static_cast<unsigned long long>(at - img.lo));
                continue;
            }
            auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return x.base == mbi.AllocationBase; });
            if (g == groups.end()) {
                if (groups.size() < 512) groups.push_back({mbi.AllocationBase, 0, 1, at});
            } else {
                ++g->count;
            }
        }
    }
    int shown = 0;
    for (const Group& g : groups) {
        if (is_stack(g.base)) {
            ++stack_groups;
            continue;
        }
        if (shown++ >= max_hits) continue;
        uintptr_t obj = 0, vt = 0;
        MEMORY_BASIC_INFORMATION m{};
        VirtualQuery(reinterpret_cast<void*>(g.first), &m, sizeof(m));
        uintptr_t lo = reinterpret_cast<uintptr_t>(m.BaseAddress);
        for (uintptr_t q = g.first & ~uintptr_t{7}; q >= lo && q + 0x1000 > g.first; q -= 8) {
            uintptr_t v = *reinterpret_cast<const uintptr_t*>(q);
            if (v >= img.rdata_lo && v < img.rdata_hi) {
                obj = q, vt = v;
                break;
            }
            if (q < 8) break;
        }
        if (vt)
            log::info("[findcam] %s: %d in allocation %p, first at %#llx = object %#llx (vtable RDR.exe+%#llx) +%#llx", what, g.count,
                      g.base, static_cast<unsigned long long>(g.first), static_cast<unsigned long long>(obj),
                      static_cast<unsigned long long>(vt - img.lo), static_cast<unsigned long long>(g.first - obj));
        else
            log::info("[findcam] %s: %d in allocation %p, first at %#llx (no vtable within 0x1000 before it)", what, g.count, g.base,
                      static_cast<unsigned long long>(g.first));
    }
    log::info("[findcam] %s: %d in RDR.exe, %d heap allocation(s) shown, %d thread stack(s) skipped; %.0f MB scanned, %.0f ms",
              what, image_hits, shown, stack_groups, scanned / 1048576.0, log::now_ms() - t0);
}

void dump_dred_device(ID3D12Device* dev, const char* why);
namespace {

std::atomic<int> g_crash_lines{0};

// The removal test's own device (a WARP device, never the game's): force_device_removal() removes it and the watcher
// must notice it like a real removal (DESIGN R0: "a test-only forceremove that proves the watcher is live").
std::atomic<ID3D12Device*> g_test_device{nullptr};
std::atomic<double> g_test_removed_at{0.0};
char g_test_status[160] = "not run";

// [Debug] HangRecorderDump (run 7, research\run7\freeze.md 5): the registers of the three frames the freezes of run 7
// stuck in, taken by log_threads' unwind while g_probe is set (the watchdog thread only): the playback thread's replay
// (Rbp the chunk, [R14] the read offset, Rsi the immediate context), its loop (Rbp the recorder, Rbx the chunk), the
// render thread's wait in the recorder's EndFrame (Rdi the recorder)
struct FrameRegs {
    bool seen = false;
    DWORD tid = 0;
    uint64_t rip = 0, rbp = 0, rbx = 0, rsi = 0, rdi = 0, r14 = 0;
};
FrameRegs g_at_replay, g_at_playback, g_at_wait;
bool g_probe = false;
DWORD g_probe_tid = 0;

void module_rva(uintptr_t addr, char* out, size_t len) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) &&
        mod) {
        char path[MAX_PATH];
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        std::snprintf(out, len, "%s+%#llx", name ? name + 1 : path,
                      static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(mod)));
    } else {
        std::snprintf(out, len, "%#llx", static_cast<unsigned long long>(addr));
    }
}

// Unwinds up to `max` frames from a context copy (no allocation).
int unwind(CONTEXT ctx, uintptr_t* out, int max) {
    int n = 0;
    for (; n < max; ++n) {
        out[n] = ctx.Rip;
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (fn && g_probe) {  // the hang's recorder dump: this frame's own registers (the callees' unwound)
            const uintptr_t begin = static_cast<uintptr_t>(image_base) + fn->BeginAddress;
            FrameRegs* f = begin == anchors::addr(anchors::Id::ReplayChunk)        ? &g_at_replay
                           : begin == anchors::addr(anchors::Id::PlaybackLoop)     ? &g_at_playback
                           : begin == anchors::addr(anchors::Id::RecorderWaitDone) ? &g_at_wait
                                                                                    : nullptr;
            if (f && !f->seen) *f = {true, g_probe_tid, ctx.Rip, ctx.Rbp, ctx.Rbx, ctx.Rsi, ctx.Rdi, ctx.R14};
        }
        if (!fn) {  // leaf: return address is at [rsp]
            if (!ctx.Rsp || IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
        }
        if (!ctx.Rip) {
            ++n;
            break;
        }
    }
    return n;
}

bool fatal_class(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
        case 0xC0000374:  // heap corruption
        case 0xC0000409:  // stack buffer overrun / fast fail
            return true;
        default:
            return false;
    }
}

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!fatal_class(code)) return EXCEPTION_CONTINUE_SEARCH;
    int line = g_crash_lines.fetch_add(1);
    if (line >= 24) return EXCEPTION_CONTINUE_SEARCH;
    char where[160];
    module_rva(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress), where, sizeof(where));
    const CONTEXT* c = ep->ContextRecord;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        log::error("[crash] exception %#lx (first chance) at %s: %s %#llx  rax %#llx rcx %#llx rdx %#llx r8 %#llx rsp %#llx", code,
                   where, ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "DEP execute" : ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                   static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]), c->Rax, c->Rcx, c->Rdx, c->R8, c->Rsp);
    } else {
        log::error("[crash] exception %#lx (first chance) at %s  rsp %#llx", code, where, c->Rsp);
    }
    uintptr_t frames[16];
    int n = unwind(*c, frames, 16);
    for (int i = 1; i < n; ++i) {
        module_rva(frames[i], where, sizeof(where));
        log::error("[crash]   #%d %s", i, where);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void log_threads(const char* why) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 te{sizeof(te)};
    int logged = 0;
    log::warn("[hang] %s: thread states follow", why);
    for (BOOL ok = Thread32First(snap, &te); ok && logged < 192; ok = Thread32Next(snap, &te)) {  // run 7: 96 cut the list
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        uintptr_t frames[10] = {};
        int n = 0;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            g_probe_tid = te.th32ThreadID;
            if (GetThreadContext(th, &ctx)) n = unwind(ctx, frames, 10);
            ResumeThread(th);
        }
        PWSTR desc = nullptr;
        char name[64] = "";
        if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc) {
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name), nullptr, nullptr);
            LocalFree(desc);
        }
        CloseHandle(th);
        char line[1024];
        int used = std::snprintf(line, sizeof(line), "[hang] tid %lu \"%s\":", te.th32ThreadID, name);
        for (int i = 0; i < n && used < static_cast<int>(sizeof(line)) - 64; ++i) {
            char w[128];
            module_rva(frames[i], w, sizeof(w));
            used += std::snprintf(line + used, sizeof(line) - used, " %s", w);
        }
        log::warn("%s", line);
        ++logged;
    }
    CloseHandle(snap);
}

bool sraw(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
T rdv(uintptr_t a) {
    T v{};
    if (!sraw(a, &v, sizeof(v))) return T{};
    return v;
}

// A wait handle's state (the handle read at handle_at): a semaphore's count (NtQuerySemaphore), else an event's state
// (NtQueryEvent), else its object type (NtQueryObject) and both queries' status codes. The game opens its semaphores
// without query access (0xc0000022): a duplicate with it is queried and closed. A value that is no handle (0xc0000008)
// is read once more as a pointer to one (the renderer's frame semaphores).
bool query_handle(HANDLE h, char* out, size_t n, LONG* ss_out, LONG* se_out) {
    using Query = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    static const auto qs = reinterpret_cast<Query>(GetProcAddress(nt, "NtQuerySemaphore"));
    static const auto qe = reinterpret_cast<Query>(GetProcAddress(nt, "NtQueryEvent"));
    HANDLE dup = nullptr;  // SEMAPHORE_QUERY_STATE and EVENT_QUERY_STATE are both 0x0001
    const HANDLE q = DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0x0001 | SYNCHRONIZE, FALSE, 0) && dup ? dup : h;
    LONG info[2] = {};
    ULONG got = 0;
    const LONG ss = qs ? qs(q, 0, info, sizeof(info), &got) : -1;  // SemaphoreBasicInformation: count, maximum
    bool done = false;
    if (ss >= 0) {
        std::snprintf(out, n, "semaphore %ld of %ld", info[0], info[1]);
        done = true;
    }
    LONG se = -1;
    if (!done) {
        se = qe ? qe(q, 0, info, sizeof(info), &got) : -1;  // EventBasicInformation: type, state
        if (se >= 0) {
            std::snprintf(out, n, "%s event %s", info[0] ? "an auto" : "a manual", info[1] ? "set" : "reset");
            done = true;
        }
    }
    if (dup) CloseHandle(dup);
    *ss_out = ss;
    *se_out = se;
    return done;
}
void handle_state(uintptr_t handle_at, char* out, size_t n) {
    using Query = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static const auto qo = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    const HANDLE h = rdv<HANDLE>(handle_at);
    if (!h) {
        std::snprintf(out, n, "no handle");
        return;
    }
    LONG ss = -1, se = -1;
    if (query_handle(h, out, n, &ss, &se)) return;
    if (static_cast<ULONG>(ss) == 0xc0000008u && reinterpret_cast<uintptr_t>(h) > 0x10000) {  // not a handle: a pointer to one?
        const HANDLE h2 = rdv<HANDLE>(reinterpret_cast<uintptr_t>(h));
        LONG ss2 = -1, se2 = -1;
        char sub[96];
        if (h2 && query_handle(h2, sub, sizeof(sub), &ss2, &se2)) {
            std::snprintf(out, n, "%s (through a pointer)", sub);
            return;
        }
    }
    struct Ustr {
        USHORT len, max;
        PWSTR buf;
    };
    alignas(8) unsigned char tb[512] = {};
    ULONG got = 0;
    char type[64] = "?";
    if (qo && qo(h, 2, tb, sizeof(tb), &got) >= 0) {  // ObjectTypeInformation: its type's name first
        const Ustr* u = reinterpret_cast<const Ustr*>(tb);
        if (u->buf && u->len) WideCharToMultiByte(CP_UTF8, 0, u->buf, u->len / 2, type, sizeof(type) - 1, nullptr, nullptr);
    }
    std::snprintf(out, n, "a %s (the semaphore query %#lx, the event query %#lx)", type, static_cast<unsigned long>(ss),
                  static_cast<unsigned long>(se));
}

void hex_dump(const char* what, uintptr_t a, size_t n) {
    for (size_t o = 0; o < n; o += 32) {
        uint8_t b[32];
        const size_t k = n - o < 32 ? n - o : 32;
        char line[160];
        int used = std::snprintf(line, sizeof(line), "[hang] rec %s +%#05zx:", what, o);
        if (!sraw(a + o, b, k)) {
            log::warn("%s unreadable", line);
            return;
        }
        for (size_t i = 0; i < k; ++i) used += std::snprintf(line + used, sizeof(line) - used, " %02x", b[i]);
        log::warn("%s", line);
    }
}

void log_chunk(const char* what, uintptr_t c) {
    if (!c) {
        log::warn("[hang] rec chunk %s: none", what);
        return;
    }
    log::warn("[hang] rec chunk %s %#llx: refs %u count %u +0x14 %u data %#llx size %#x cap %#x +0x28 %#x +0x2c %#x", what,
              static_cast<unsigned long long>(c), rdv<uint32_t>(c + 0xc), rdv<uint32_t>(c + 0x10), rdv<uint32_t>(c + 0x14),
              static_cast<unsigned long long>(rdv<uintptr_t>(c + 0x18)), rdv<uint32_t>(c + 0x20), rdv<uint32_t>(c + 0x24),
              rdv<uint32_t>(c + 0x28), rdv<uint32_t>(c + 0x2c));
}

// The game's D3D command recorder at a hang (research\run7\freeze.md 5 A, B, D; reads only): its chunks and queues,
// the renderer's frame semaphores, the stuck frames' registers, and the packets about the read point and the write end.
// One dump says which desync it was: packets written but not counted, the writer on another chunk, the reader past the
// writer, or a stale chunk queued.
}  // namespace

std::string sample_threads(int n) {
    n = n < 1 ? 1 : n > 100 ? 100 : n;
    struct Th {
        DWORD tid;
        HANDLE h;
        uint64_t cpu0;
        char name[64];
        std::vector<std::pair<uint64_t, uint64_t>> rips;  // (rip, the qword at rsp)
    };
    std::vector<Th> ths;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return "ERROR no thread snapshot";
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok && ths.size() < 256; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        Th t{te.th32ThreadID, h, 0, "", {}};
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u))
            t.cpu0 = (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, t.name, sizeof(t.name), nullptr, nullptr);
            LocalFree(desc);
        }
        ths.push_back(std::move(t));
    }
    CloseHandle(snap);
    const double t0 = log::now_ms();
    for (int s = 0; s < n; ++s) {
        for (Th& t : ths) {
            if (SuspendThread(t.h) == static_cast<DWORD>(-1)) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            uint64_t top = 0;
            if (GetThreadContext(t.h, &ctx)) {
                sraw(static_cast<uintptr_t>(ctx.Rsp), &top, sizeof(top));
                t.rips.emplace_back(ctx.Rip, top);
            }
            ResumeThread(t.h);
        }
        Sleep(50);
    }
    const double secs = (log::now_ms() - t0) / 1000.0;
    int busy = 0;
    for (Th& t : ths) {
        FILETIME c, e, k, u;
        uint64_t cpu1 = t.cpu0;
        if (GetThreadTimes(t.h, &c, &e, &k, &u))
            cpu1 = (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
        CloseHandle(t.h);
        const double share = secs > 0 ? (cpu1 - t.cpu0) / 1e7 / secs : 0.0;  // 100 ns units
        if (share < 0.10) continue;
        ++busy;
        std::vector<std::pair<uint64_t, int>> hist;  // rip -> count
        for (const auto& r : t.rips) {
            auto it = std::find_if(hist.begin(), hist.end(), [&](const auto& x) { return x.first == r.first; });
            if (it == hist.end()) hist.emplace_back(r.first, 1);
            else ++it->second;
        }
        std::sort(hist.begin(), hist.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        char line[1400];
        int used = std::snprintf(line, sizeof(line), "[sample] tid %lu \"%s\" %.0f%% of a core, %zu samples:", t.tid, t.name, share * 100.0, t.rips.size());
        for (size_t i = 0; i < hist.size() && i < 6 && used < static_cast<int>(sizeof(line)) - 200; ++i) {
            char where[128], top[128];
            module_rva(static_cast<uintptr_t>(hist[i].first), where, sizeof(where));
            uint64_t tv = 0;
            for (const auto& r : t.rips)
                if (r.first == hist[i].first) {
                    tv = r.second;
                    break;
                }
            module_rva(static_cast<uintptr_t>(tv), top, sizeof(top));
            used += std::snprintf(line + used, sizeof(line) - used, " %s x%d (top %s);", where, hist[i].second, top);
        }
        log::warn("%s", line);
    }
    char out[160];
    std::snprintf(out, sizeof(out), "sampled %zu threads %d times over %.1f s: %d busy (over 10%% of a core) logged as [sample] lines", ths.size(), n, secs, busy);
    return out;
}

void dump_recorder() {
    using anchors::Id;
    if (anchors::stand_down()) {
        log::warn("[hang] rec: the anchors stood down, no recorder dump");
        return;
    }
    const uintptr_t dev = rdv<uintptr_t>(anchors::addr(Id::D3dDeviceSingleton));
    const uintptr_t hrec = rdv<uintptr_t>(dev + 0x58), rec = rdv<uintptr_t>(hrec + 0x128);
    const uintptr_t himm = rdv<uintptr_t>(dev + 0x50), imm = rdv<uintptr_t>(himm + 0x128);
    log::warn("[hang] rec: device %#llx; recorder holder %#llx recorder %#llx (vtable %s), frame open %u +0x2ad %u; immediate holder %#llx "
              "context %#llx (vtable %s)",
              static_cast<unsigned long long>(dev), static_cast<unsigned long long>(hrec), static_cast<unsigned long long>(rec),
              rdv<uintptr_t>(rec) == anchors::addr(Id::RecorderVtbl) ? "ok" : "NOT the recorder's", rdv<uint8_t>(hrec + 0x2ac),
              rdv<uint8_t>(hrec + 0x2ad), static_cast<unsigned long long>(himm), static_cast<unsigned long long>(imm),
              rdv<uintptr_t>(imm) == anchors::addr(Id::ImmCtxVtbl) ? "ok" : "NOT the immediate context's");
    const uintptr_t W = rdv<uintptr_t>(rec + 0x80), H = rdv<uintptr_t>(rec + 0xc8);
    log::warn("[hang] rec: writer %#llx desc %#llx +0x98 %d ring %#llx index %d frame %d head %#llx", static_cast<unsigned long long>(W),
              static_cast<unsigned long long>(rdv<uintptr_t>(rec + 0x88)), rdv<int>(rec + 0x98),
              static_cast<unsigned long long>(rdv<uintptr_t>(rec + 0xa0)), rdv<int>(rec + 0xb8), rdv<int>(rec + 0xbc),
              static_cast<unsigned long long>(H));
    log_chunk("head", H);
    if (W != H) log_chunk("writer", W);
    const struct {
        const char* name;
        uintptr_t q;
    } queues[2] = {{"finished", rec + 0xd0}, {"work", rec + 0x108}};
    for (const auto& qq : queues) {
        char hs[128];
        handle_state(qq.q, hs, sizeof(hs));
        const uintptr_t map = rdv<uintptr_t>(qq.q + 0x18);
        const uint64_t msz = rdv<uint64_t>(qq.q + 0x20), head = rdv<uint64_t>(qq.q + 0x28), count = rdv<uint64_t>(qq.q + 0x30);
        log::warn("[hang] rec queue %s: handle %#llx %s, lock %d, map %#llx size %llu head %llu count %llu", qq.name,
                  static_cast<unsigned long long>(rdv<uintptr_t>(qq.q)), hs, rdv<int>(qq.q + 8),
                  static_cast<unsigned long long>(map), static_cast<unsigned long long>(msz), static_cast<unsigned long long>(head),
                  static_cast<unsigned long long>(count));
        if (!map || !msz || (msz & (msz - 1)) || count > 64) continue;
        for (uint64_t i = 0; i < count && i < 8; ++i) {
            const uintptr_t block = rdv<uintptr_t>(map + (((head + i) >> 1) & (msz - 1)) * 8);
            char what[32];
            std::snprintf(what, sizeof(what), "%s[%llu]", qq.name, static_cast<unsigned long long>(i));
            log_chunk(what, rdv<uintptr_t>(block + ((head + i) & 1) * 8));
        }
    }
    const uintptr_t ren = rdv<uintptr_t>(anchors::addr(Id::RendererSingleton));
    char sems[768];
    int used = std::snprintf(sems, sizeof(sems), "[hang] rec renderer %#llx:", static_cast<unsigned long long>(ren));
    for (const int off : {0x10, 0x18, 0x38, 0x68, 0x88}) {
        char hs[128];
        handle_state(ren ? ren + off : 0, hs, sizeof(hs));
        used += std::snprintf(sems + used, sizeof(sems) - used, " +%#x %s;", off, hs);
    }
    log::warn("%s; bytes +0x34 %u +0x53d %u +0x6db %u; the render thread id %u", sems, rdv<uint8_t>(ren + 0x34), rdv<uint8_t>(ren + 0x53d),
              rdv<uint8_t>(ren + 0x6db), rdv<uint32_t>(anchors::addr(Id::RenderThreadId)));
    const FrameRegs fr[3] = {g_at_replay, g_at_playback, g_at_wait};
    const char* const frn[3] = {"the replay (Rbp chunk, [R14] offset, Rsi context)", "the playback loop (Rbp recorder, Rbx chunk)",
                                "the EndFrame wait (Rdi recorder)"};
    for (int i = 0; i < 3; ++i)
        if (fr[i].seen)
            log::warn("[hang] rec frame %s: tid %lu rip %#llx rbp %#llx rbx %#llx rsi %#llx rdi %#llx r14 %#llx", frn[i], fr[i].tid,
                      static_cast<unsigned long long>(fr[i].rip), static_cast<unsigned long long>(fr[i].rbp),
                      static_cast<unsigned long long>(fr[i].rbx), static_cast<unsigned long long>(fr[i].rsi),
                      static_cast<unsigned long long>(fr[i].rdi), static_cast<unsigned long long>(fr[i].r14));
        else
            log::warn("[hang] rec frame %s: not on any stack", frn[i]);
    if (g_at_replay.seen) {
        const uintptr_t X = static_cast<uintptr_t>(g_at_replay.rbp);
        const int ofs = rdv<int>(static_cast<uintptr_t>(g_at_replay.r14));
        log::warn("[hang] rec: the replayed chunk %#llx is %s, read offset %#x", static_cast<unsigned long long>(X),
                  X == H ? "the head" : X == W ? "the writer" : "neither the head nor the writer", ofs);
        log_chunk("replayed", X);
        const uintptr_t xd = rdv<uintptr_t>(X + 0x18);
        if (xd && ofs >= 0) hex_dump("replayed about the read offset", xd + (ofs > 0x80 ? ofs - 0x80 : 0), 0x100);
    }
    const uintptr_t c = W ? W : H;
    const uintptr_t cd = rdv<uintptr_t>(c + 0x18);
    const uint32_t sz = rdv<uint32_t>(c + 0x20);
    if (cd && sz) hex_dump("the write end", cd + (sz > 0x80 ? sz - 0x80 : 0), sz > 0x80 ? 0x80 : sz);
}

namespace {

DWORD WINAPI watchdog(void*) {
    const double hang_ms = config::get_int("Debug", "HangMs", 10000);
    const bool rec_dump = config::get_bool("Debug", "HangRecorderDump", false);
    bool hang_reported = false, removal_reported = false;
    // run 7 (G1b): a slowdown is no hang: under 20 Presents in two 5 s windows in a row, logged once a session (with
    // HangRecorderDump: the busy threads sampled and the recorder read)
    double win_start = log::now_ms();
    uint64_t win_presents = state::presents.load();
    int slow_windows = 0;
    bool slow_reported = false, late_reported = false;
    const double started = log::now_ms();
    for (;;) {
        Sleep(500);
        double now = log::now_ms();
        uint64_t presents = state::presents.load();
        if (!late_reported && presents == 0 && now - started > 30000.0) {  // run 8 item 3: the hooks came after the game's swapchain
            late_reported = true;
            log::error("[d3d] LATE: no Present through the hook 30 s after the startup hooks: the game made its device and swapchain "
                       "before them (a game executable or wrapper that loads the mod late?): no VR this run");
        }
        double last = state::last_present_ms.load();
        if (now - win_start >= 5000.0) {
            const uint64_t n = presents - win_presents;
            slow_windows = presents > 600 && n > 0 && n < 20 ? slow_windows + 1 : 0;  // in play (600 frames in), not hung
            win_start = now;
            win_presents = presents;
            if (slow_windows >= 2 && !slow_reported) {
                slow_reported = true;
                log::warn("[hang] slow frames: %llu Presents in the last 5 s (after %llu frames)%s", static_cast<unsigned long long>(n),
                          static_cast<unsigned long long>(presents), rec_dump ? ": the busy threads and the recorder follow" : "");
                if (rec_dump) {
                    log::warn("[hang] %s", sample_threads(20).c_str());
                    dump_recorder();
                }
            }
        }
        if (presents > 0 && now - last > hang_ms) {
            if (!hang_reported) {
                hang_reported = true;
                char why[96];
                std::snprintf(why, sizeof(why), "no Present for %.1f s after %llu frames", (now - last) / 1000.0,
                              static_cast<unsigned long long>(presents));
                g_at_replay = g_at_playback = g_at_wait = FrameRegs{};  // this hang's frames only
                g_probe = rec_dump;
                log_threads(why);
                g_probe = false;
                if (rec_dump) dump_recorder();
            }
        } else if (hang_reported && now - last < 1000) {
            hang_reported = false;
            log::info("[hang] Presents resumed (frame %llu)", static_cast<unsigned long long>(presents));
        }
        if (ID3D12Device* t = g_test_device.load()) {
            HRESULT r = t->GetDeviceRemovedReason();
            if (r != S_OK && g_test_device.exchange(nullptr) == t) {
                double ms = log::now_ms() - g_test_removed_at.load();
                std::snprintf(g_test_status, sizeof(g_test_status), "passed: test device removal seen after %.0f ms, reason %#lx",
                              ms, static_cast<unsigned long>(r));
                log::warn("[dred] TEST DEVICE REMOVED (reason %#lx) and seen by the watcher after %.0f ms; the game's device is"
                          " untouched", static_cast<unsigned long>(r), ms);
                dump_dred_device(t, "test device removed");
                t->Release();
            }
        }
        if (!removal_reported) {
            if (ID3D12Device* dev = state::device.load()) {
                HRESULT r = dev->GetDeviceRemovedReason();
                if (r != S_OK) {
                    removal_reported = true;
                    state::device_removed = true;
                    log::error("[dred] DEVICE REMOVED: reason %#lx (frame %llu)", static_cast<unsigned long>(r),
                               static_cast<unsigned long long>(presents));
                    dump_dred("device removed");
                }
            }
        }
    }
}

const char* op_name(D3D12_AUTO_BREADCRUMB_OP op) {
    static const char* const kNames[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
                                         "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion",
                                         "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
                                         "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
                                         "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission",
                                         "EndSubmission"};
    return op < sizeof(kNames) / sizeof(kNames[0]) ? kNames[op] : "op";
}

}  // namespace

void install_crash_handler() {
    if (AddVectoredExceptionHandler(1, veh)) log::info("[crash] vectored handler installed (fatal classes, first 24 logged)");
}

void start_watchdog() {
    if (HANDLE t = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr)) {
        SetThreadDescription(t, L"RDRVR watchdog");
        CloseHandle(t);
    }
}

void dump_dred(const char* why) { dump_dred_device(state::device.load(), why); }

void dump_dred_device(ID3D12Device* dev, const char* why) {
    if (!dev) return;
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        log::error("[dred] %s: DRED interface unavailable (armed: %d)", why, state::dred_enabled.load());
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&bc))) {
        int lists = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc.pHeadAutoBreadcrumbNode; n && lists < 24; n = n->pNext, ++lists) {
            UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount) continue;  // completed lists are not interesting
            log::error("[dred] list \"%ls\" on queue \"%ls\": %u of %u ops completed", n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"?",
                       n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"?", done, n->BreadcrumbCount);
            UINT from = done > 6 ? done - 6 : 0, to = done + 6 < n->BreadcrumbCount ? done + 6 : n->BreadcrumbCount;
            for (UINT i = from; i < to; ++i) {
                log::error("[dred]   %s op %u %s", i == done ? "->" : "  ", i, op_name(n->pCommandHistory[i]));
            }
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pf)) && pf.PageFaultVA) {
        log::error("[dred] page fault at GPU VA %#llx", static_cast<unsigned long long>(pf.PageFaultVA));
        int k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* a = pf.pHeadExistingAllocationNode; a && k < 12; a = a->pNext, ++k)
            log::error("[dred]   existing allocation \"%ls\" type %d", a->ObjectNameW ? a->ObjectNameW : L"?", static_cast<int>(a->AllocationType));
        k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* a = pf.pHeadRecentFreedAllocationNode; a && k < 12; a = a->pNext, ++k)
            log::error("[dred]   recently freed \"%ls\" type %d", a->ObjectNameW ? a->ObjectNameW : L"?", static_cast<int>(a->AllocationType));
    }
    dred->Release();
}

// Removes a separate WARP device, never the game's: removing the game's device makes the game crash, and its Crashpad
// handler uploads every crash to Rockstar's crash service (2026-10-03).
bool force_device_removal() {
    if (g_test_device.load()) return false;  // one test at a time
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter* warp = nullptr;
    ID3D12Device* dev = nullptr;
    bool ok = SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))) && f && SUCCEEDED(f->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
              warp && SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) && dev;
    if (warp) warp->Release();
    if (f) f->Release();
    ID3D12Device5* dev5 = nullptr;
    if (!ok || FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev5))) || !dev5) {
        if (dev) dev->Release();
        std::snprintf(g_test_status, sizeof(g_test_status), "failed: no WARP test device");
        log::error("[dred] removal test: could not create a WARP test device");
        return false;
    }
    if (dev == state::device.load()) {  // never the game's device
        dev5->Release();
        dev->Release();
        return false;
    }
    std::snprintf(g_test_status, sizeof(g_test_status), "running");
    g_test_removed_at = log::now_ms();
    g_test_device = dev;  // the watcher owns this reference now
    log::warn("[dred] removal test: removing WARP test device %p (game device %p untouched)", static_cast<void*>(dev),
              static_cast<void*>(state::device.load()));
    dev5->RemoveDevice();
    dev5->Release();
    return true;
}

const char* removal_test_status() { return g_test_status; }

}  // namespace rdrvr::diag
