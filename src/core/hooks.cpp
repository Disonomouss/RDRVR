#include "core/hooks.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include <safetyhook.hpp>

#include "core/log.h"

namespace rdrvr::hooks {
namespace {

struct Entry {
    const char* name;
    void* target;
    safetyhook::InlineHook hook;
};

std::mutex g_mutex;
std::deque<Entry> g_entries;  // deque: stable addresses, hooks are never moved after install

void describe_address(void* p, char* out, size_t len) {
    HMODULE mod = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(p), &mod) &&
        mod) {
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        std::snprintf(out, len, "%s+%#llx", base ? base + 1 : path,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(mod)));
    } else {
        std::snprintf(out, len, "%p", p);
    }
}

}  // namespace

bool install(const char* name, void* target, void* detour, void** original) {
    char where[160];
    describe_address(target, where, sizeof(where));
    if (!target) {
        log::error("[hook] %s: no target", name);
        return false;
    }
    auto created = safetyhook::InlineHook::create(target, detour, safetyhook::InlineHook::StartDisabled);
    if (!created) {
        log::error("[hook] %s at %s: create failed (error type %d)", name, where, static_cast<int>(created.error().type));
        return false;
    }
    std::lock_guard lock(g_mutex);
    g_entries.push_back({name, target, std::move(*created)});
    Entry& e = g_entries.back();
    *original = e.hook.original<void*>();
    if (auto enabled = e.hook.enable(); !enabled) {
        log::error("[hook] %s at %s: enable failed (error type %d)", name, where, static_cast<int>(enabled.error().type));
        *original = nullptr;
        g_entries.pop_back();
        return false;
    }
    log::info("[hook] %s installed at %s", name, where);
    return true;
}

int count() {
    std::lock_guard lock(g_mutex);
    return static_cast<int>(g_entries.size());
}

void describe(char* out, size_t out_len) {
    std::lock_guard lock(g_mutex);
    size_t used = 0;
    out[0] = 0;
    for (const Entry& e : g_entries) {
        int n = std::snprintf(out + used, out_len - used, "%s%s", used ? "," : "", e.name);
        if (n < 0 || static_cast<size_t>(n) >= out_len - used) break;
        used += static_cast<size_t>(n);
    }
}

}  // namespace rdrvr::hooks
