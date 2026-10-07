#include "core/anchors.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
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

}  // namespace

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
    g_ok.store(ok);
    g_verified.store(true);
    return ok;
}

bool stand_down() { return !g_verified.load() || !g_ok.load(); }

}  // namespace rdrvr::anchors
