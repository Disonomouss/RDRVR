#pragma once
// Small D3D12 helpers shared by the mod's own draws (2026-10-29, the review of 374a871).

#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace rdrvr::d3d_util {

// The fence at `value` at least, waited for up to `ms`. The auto-reset event is reset before it is armed: a wait that
// timed out earlier leaves its signal on the event for later, and that signal would end a later wait at once. The value
// is read again after the wait, so a caller never resets an allocator (or frees a resource) while the GPU may still run
// the lists that use it.
inline bool fence_reached(ID3D12Fence* fence, uint64_t value, HANDLE ev, DWORD ms) {
    if (fence->GetCompletedValue() >= value) return true;
    ResetEvent(ev);
    if (SUCCEEDED(fence->SetEventOnCompletion(value, ev))) WaitForSingleObject(ev, ms);
    return fence->GetCompletedValue() >= value;
}

}  // namespace rdrvr::d3d_util
