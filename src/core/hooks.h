#pragma once
// Inline hooks (safetyhook), created disabled and then enabled, each registered by name so the log and the status
// file can list what is installed. Install once; never remove while the game runs (RDR2VR: mid-game installs of
// hot D3D12 hooks raced with the render threads).

#include <cstdint>

namespace rdrvr::hooks {

// Hooks `target` with `detour`. On success *original receives the trampoline and true is returned; on failure
// the reason is logged and false returned. `name` must be a string literal.
bool install(const char* name, void* target, void* detour, void** original);

template <typename Fn>
bool install(const char* name, void* target, Fn detour, Fn* original) {
    return install(name, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

int count();                                   // installed hooks
void describe(char* out, size_t out_len);      // "name@module+rva, ..." for the status file

// Reads entry `slot` of a COM object's vtable.
inline void* vtable_entry(void* com_object, int slot) { return (*reinterpret_cast<void***>(com_object))[slot]; }

}  // namespace rdrvr::hooks
