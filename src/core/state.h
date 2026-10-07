#pragma once
// Process-wide state shared between the core's modules. Plain atomics: written by the hook that owns them, read by
// the status writer and the watchdogs.

#include <atomic>
#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain;

namespace rdrvr::state {

// D3D12 objects the game created (not owned; the game holds the references).
inline std::atomic<ID3D12Device*> device{nullptr};
inline std::atomic<ID3D12CommandQueue*> present_queue{nullptr};   // the queue the swapchain was created on
inline std::atomic<IDXGISwapChain*> swapchain{nullptr};
inline std::atomic<uint64_t> swapchain_frame{0};       // presents count when the newest swapchain was created
inline std::atomic<void*> game_hwnd{nullptr};

// Frame counters (Present is the frame boundary).
inline std::atomic<uint64_t> presents{0};
inline std::atomic<double> last_present_ms{0.0};
inline std::atomic<uint32_t> swap_width{0}, swap_height{0}, swap_format{0}, swap_buffers{0};

// Per-frame counts, published at each Present for the frame that just ended.
inline std::atomic<uint32_t> frame_ecl_calls{0};       // ExecuteCommandLists calls, all queues
inline std::atomic<uint32_t> frame_ecl_lists{0};       // command lists submitted, all queues
inline std::atomic<uint32_t> frame_ecl_direct{0};      // ... on DIRECT queues
inline std::atomic<uint32_t> frame_ecl_compute{0};     // ... on COMPUTE queues
inline std::atomic<uint32_t> frame_ecl_copy{0};        // ... on COPY queues
inline std::atomic<uint32_t> frame_ecl_present{0};     // ExecuteCommandLists calls on the present queue
inline std::atomic<uint32_t> frame_set_current{0};     // grcViewport::SetCurrent calls
inline std::atomic<uint32_t> frame_push_globals{0};    // 0x130e60 calls (dev probe)
inline std::atomic<uint32_t> queues_direct{0}, queues_compute{0}, queues_copy{0};

inline std::atomic<double> gpu_frame_ms{0.0};          // EMA of GPU busy time per frame on the present queue
                                                       // (sum over the game's ExecuteCommandLists of end - begin)
inline std::atomic<double> gpu_span_ms{0.0};           // EMA of first submission begin -> last submission end
inline std::atomic<uint32_t> gpu_brackets{0};          // game submissions timed last frame
inline std::atomic<double> cpu_frame_ms{0.0};          // EMA of Present-to-Present time

inline std::atomic<bool> device_removed{false};
inline std::atomic<bool> dred_enabled{false};

}  // namespace rdrvr::state
