#pragma once
// GPU time per frame on the present queue: each game ExecuteCommandLists there is bracketed by timestamps, and the
// frame's busy time (sum of the brackets) and span (first to last) go to state::gpu_frame_ms / gpu_span_ms as EMAs,
// with a percentile line in the log every 600 frames.

namespace rdrvr::gpu_timer {

void init();   // registers with the D3D12 hooks (ECL bracket and frame end)

}  // namespace rdrvr::gpu_timer
