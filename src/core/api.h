#pragma once
// Core side of the gameplay-plugin API (src/common/rdrvr_api.h): the native request queue and the
// script-tick counters.

#include <cstdint>

#include "common/rdrvr_api.h"

namespace rdrvr::api {

// Queues a native call for the next script tick; returns its id.
uint64_t queue_native(uint32_t hash, const uint64_t* args, uint32_t argc, uint32_t vec_out = 0,
                      const float* vec_in = nullptr);
// (2026-10-29) Queues a raw native at the FRONT of the queue: called from a tick hook (set_tick_hook), it runs in this
// same tick, ahead of every queued request (the plugin pops 64 a tick: only the watches, at most 8, go before it), with no
// ScriptWait between the hook and it. Its id is outside result_posted's mark (wait for its own result instead).
uint64_t queue_native_front(uint32_t hash, const uint64_t* args, uint32_t argc);
// Queues one of the plugin's own operations (RdrvrNativeOp other than RAW, v5) with its arguments and input vector.
uint64_t queue_op(uint32_t op, const uint64_t* args, uint32_t argc, const float* vec_in);
// Waits up to timeout_ms for the result of `id`. False on timeout.
bool wait_native(uint64_t id, RdrvrNativeResult* out, uint32_t timeout_ms);
// Removes `id` from the queue if no script tick has taken it yet (true); false once the plugin has it (its result then
// comes as usual and stays unread). For a request given up on, so it does not run late (run 7: the gun melee's scans).
bool cancel_op(uint64_t id);

uint64_t script_ticks();           // ticks reported by the .red so far
unsigned long script_tick_thread();  // the thread the last script tick ran on (the game's), 0 before the first
// (2026-10-27) request `id` has run (the plugin posted its result or a later one's: they run in the order queued),
// whether or not its result is still kept; *tick: the latest posted result's tick, at or after the one it ran on
// (a cancelled request, cancel_op, never runs, yet reads as run once a later id posts: no spare-changing native is ever
// cancelled, so its users, reload.cpp and dual.cpp, are not misled; keep it that way)
bool result_posted(uint64_t id, uint64_t* tick);
double last_script_tick_ms();      // log::now_ms() of the last tick (0 if none)
bool plugin_attached();
// v7: the calling thread is inside the plugin's script tick (on_script_tick seen on this thread and end_script_tick not
// yet): the game thread, where the plugin runs natives and the core may call into the game (gun_melee_hit).
bool in_script_tick();
// (2026-11-09, round 115) The vectored handler's call for a fatal-class exception raised in RDR.exe's code: on the
// thread running a request the plugin popped (its result not yet posted), that request faulted inside the game. RedHook
// catches it and hands back the call's leftover buffer (its first argument), and a lock the game held stays held: the
// result is answered 0 and the request's id noted (faulted). Atomics only (any thread).
void note_native_fault();
uint64_t native_faults();    // requests that faulted so far
uint32_t last_fault_hash();  // the latest one's hash (0 for a plugin op)
bool faulted(uint64_t id);   // request `id` faulted (one of the latest 16 that did): its 0 is no answer of the game's
// A function the core runs at the start of each script tick, on the game thread, before the tick's requests (slot 0: the
// reticle's barrel probe, aim.cpp; slot 1, 2026-10-29: [Hands] OffHandItems' throw, offhand.cpp; slot 2, 2026-10-31: the
// test-only probe commands, probe.cpp, set only once a probe command is sent). It may queue natives;
// their results come in the same tick.
void set_tick_hook(void (*hook)(), int slot = 0);
// (round 130) `f` run once on the next script tick, on its thread (one at a time: a later call before the tick replaces it)
void run_on_tick(void (*f)());
// (2026-10-28) The plugin's run of request `id` timed, from its pop to its posted result, in slot 0-3 (the latest id a
// slot was given; aim.cpp's reticle probes: 0 the main's, 1 the second gun's): the runs' total (us) and count.
void time_native(uint64_t id, int slot);
void native_time(int slot, uint64_t* us, uint64_t* n);
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
