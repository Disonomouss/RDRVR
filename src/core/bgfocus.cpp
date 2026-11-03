#include "core/bgfocus.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <sstream>

#include "core/anchors.h"
#include "core/config.h"
#include "core/log.h"

namespace rdrvr::bgfocus {
namespace {

using anchors::Id;
using IsActive_t = bool(__fastcall*)(void* window);  // movzx eax,[rcx+0xad]; ret

IsActive_t o_is_active = nullptr;
std::atomic<bool> g_installed{false}, g_on{false};
uintptr_t g_ret_pad = 0, g_ret_pause = 0;  // the two calls' return addresses (call [rdx+0xe8] is 6 bytes)
std::atomic<uint64_t> g_pad{0}, g_pause{0}, g_inactive{0};  // answered "active" to each while really inactive

// The window vtable's IsActive: reached only through that virtual call, so its callers already treat the volatile
// registers as clobbered (not a leaf detour: the leaf-hook rule does not apply)
bool __fastcall hk_is_active(void* window) {
    const bool real = o_is_active(window);
    if (real || !g_on.load(std::memory_order_relaxed)) return real;
    const uintptr_t r = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (r == g_ret_pad) {
        g_pad.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (r == g_ret_pause) {
        g_pause.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_inactive.fetch_add(1, std::memory_order_relaxed);
    return real;
}

const char* const kOptionals[] = {"WindowVtblIsActive", "WindowIsActive", "PadFocusGateCall", "FocusPauseCall"};

}  // namespace

void install() {
    if (!config::get_bool("Debug", "BackgroundFocus", false)) return;
    if (anchors::stand_down() || !anchors::present(Id::WindowVtblIsActive) || !anchors::present(Id::WindowIsActive) ||
        !anchors::present(Id::PadFocusGateCall) || !anchors::present(Id::FocusPauseCall)) {
        log::warn("[bgfocus] [Debug] BackgroundFocus: this build lacks its anchors (%s ...): off", kOptionals[0]);
        return;
    }
    void** slot = anchors::ptr<void**>(Id::WindowVtblIsActive);
    void* const want = reinterpret_cast<void*>(anchors::addr(Id::WindowIsActive));
    void* cur = nullptr;
    __try {
        cur = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cur = nullptr;
    }
    if (cur != want) {  // (the slot must hold the shared getter the anchors checked: anything else, stand down)
        log::error("[bgfocus] the window vtable's IsActive slot %p holds %p, not the getter %p: off", static_cast<void*>(slot), cur, want);
        return;
    }
    g_ret_pad = anchors::addr(Id::PadFocusGateCall) + 6;
    g_ret_pause = anchors::addr(Id::FocusPauseCall) + 6;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        log::error("[bgfocus] the IsActive slot: VirtualProtect failed (%lu): off", GetLastError());
        return;
    }
    o_is_active = reinterpret_cast<IsActive_t>(cur);
    g_on = true;
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hk_is_active));
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_installed = true;
    log::info("[bgfocus] [Debug] BackgroundFocus on (test only): the window's IsActive slot %p: %p -> the pad update (returns to %p) and the pause "
              "check (%p) told \"active\" while the window is not; every other caller the real state",
              static_cast<void*>(slot), cur, reinterpret_cast<void*>(g_ret_pad), reinterpret_cast<void*>(g_ret_pause));
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v;
    in >> c >> v;
    if ((v == "on" || v == "off") && g_installed.load()) g_on = v == "on";
    char b[240];
    std::snprintf(b, sizeof(b), "bgfocus: %s%s | answered active while inactive: pad %llu, pause %llu; other callers told inactive %llu",
                  g_installed.load() ? "installed" : "not installed ([Debug] BackgroundFocus off at start)",
                  g_installed.load() ? (g_on.load() ? ", on" : ", off") : "", static_cast<unsigned long long>(g_pad.load()),
                  static_cast<unsigned long long>(g_pause.load()), static_cast<unsigned long long>(g_inactive.load()));
    return b;
}

}  // namespace rdrvr::bgfocus
