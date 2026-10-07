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
    for (BOOL ok = Thread32First(snap, &te); ok && logged < 96; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        uintptr_t frames[10] = {};
        int n = 0;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
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

DWORD WINAPI watchdog(void*) {
    const double hang_ms = config::get_int("Debug", "HangMs", 10000);
    bool hang_reported = false, removal_reported = false;
    for (;;) {
        Sleep(500);
        double now = log::now_ms();
        uint64_t presents = state::presents.load();
        double last = state::last_present_ms.load();
        if (presents > 0 && now - last > hang_ms) {
            if (!hang_reported) {
                hang_reported = true;
                char why[96];
                std::snprintf(why, sizeof(why), "no Present for %.1f s after %llu frames", (now - last) / 1000.0,
                              static_cast<unsigned long long>(presents));
                log_threads(why);
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
