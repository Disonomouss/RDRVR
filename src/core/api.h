#pragma once
// Core side of the gameplay-plugin API (src/common/rdrvr_api.h): the native request queue and the
// script-tick counters.

#include <cstdint>

#include "common/rdrvr_api.h"

namespace rdrvr::api {

// Queues a native call for the next script tick; returns its id.
uint64_t queue_native(uint32_t hash, const uint64_t* args, uint32_t argc, uint32_t vec_out = 0,
                      const float* vec_in = nullptr);
// Queues one of the plugin's own operations (RdrvrNativeOp other than RAW, v5) with its arguments and input vector.
uint64_t queue_op(uint32_t op, const uint64_t* args, uint32_t argc, const float* vec_in);
// Waits up to timeout_ms for the result of `id`. False on timeout.
bool wait_native(uint64_t id, RdrvrNativeResult* out, uint32_t timeout_ms);
// Removes `id` from the queue if no script tick has taken it yet (true); false once the plugin has it (its result then
// comes as usual and stays unread). For a request given up on, so it does not run late (run 7: the gun melee's scans).
bool cancel_op(uint64_t id);

uint64_t script_ticks();           // ticks reported by the .red so far
double last_script_tick_ms();      // log::now_ms() of the last tick (0 if none)
bool plugin_attached();
// v7: the calling thread is inside the plugin's script tick (on_script_tick seen on this thread and end_script_tick not
// yet): the game thread, where the plugin runs natives and the core may call into the game (gun_melee_hit).
bool in_script_tick();
// A native with no arguments called on every script tick (state queries such as IS_GAME_PAUSED), its latest result
// kept: up to 8. Returns the watch index, or -1 when full.
int watch_native(uint32_t hash, uint64_t arg0 = 0, uint32_t argc = 0);  // argc 0 or 1 (arg0: e.g. a static string's address)
// The latest result of watch `index` and the script tick it ran on; false before the first result.
bool watched(int index, uint64_t* value, uint64_t* tick);
// G-B (v3): the camera job the plugin reads every tick, and the actor state it posts back.
void set_camera_job(const RdrvrCameraJob& job);
RdrvrCameraJob camera_job();
bool actor_state(RdrvrActorState* out);

}  // namespace rdrvr::api
