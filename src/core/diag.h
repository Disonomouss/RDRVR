#pragma once
// Crash, hang and device-removal diagnostics (DESIGN R0):
//  - CrashLog: a first vectored exception handler that logs fatal-class exceptions (module+RVA, registers, a short
//    unwound stack), rate-limited, then lets the exception continue to the game's own handlers.
//  - HangLog: a watchdog that notices when Presents stop for HangMs and logs where every thread is.
//  - Device removal: polls GetDeviceRemovedReason every 0.5 s and dumps the DRED breadcrumbs and page-fault
//    allocations when the device goes away. RDR2VR's "freezes" were device removals behind an invisible box.

#include <string>

namespace rdrvr::diag {

// Test diagnostic: searches the process's committed read-write memory (image and private; not write-combined or
// uncached) for an exact byte pattern and logs up to max_hits places: RDR.exe+rva, the calling thread's stack, or a heap
// address with the nearest preceding qword that points into RDR.exe's .rdata (an object's vtable, probably).
void find_pattern(const void* pattern, size_t n, const char* what, int max_hits);

void install_crash_handler();   // as early as possible
void start_watchdog();          // after the D3D12 hooks are installed
// Test-only (DESIGN R0 exit criterion: the watcher is proven live): creates a separate WARP device, removes it and
// lets the watcher detect that removal. The game's device is never touched.
bool force_device_removal();
const char* removal_test_status();   // "not run", "running", "passed: ...", "failed: ..."
void dump_dred(const char* why);     // the game's device
// [Debug] HangRecorderDump's reads (the game's D3D command recorder, its queues and chunks, the renderer's frame
// semaphores; reads only, SEH-guarded), logged as "[hang] rec ...": at a hang with the stuck frames' registers, or on
// demand ("recdump", the test channel) to check the reads against a running game.
void dump_recorder();
// "sample [n]" (the test channel): n samples, 50 ms apart, of every thread's RIP and stack top (each thread suspended
// only for its context: no unwind); the threads that used over 10% of a core meanwhile logged with their commonest RIPs
// ("[sample] ..."). Returns a one-line summary.
std::string sample_threads(int n);
// "stacks <render|playback> [n]" (the single-pass study): n samples, 1-3 ms apart, of the render (recording) or the
// playback thread's whole stack, unwound after the thread runs on, counted per function (inclusive and self) and per
// scene phase (frame_probe::phase). Runs on its own thread; "[stack]" lines in the log, the last "... done".
std::string sample_stacks(const char* which, int n);
// "affinity same|split|off" (the single-pass study): the render and playback threads pinned to one P-core's two
// logical processors (2, 3), to two P-cores (2, 4), or back to their masks. Test only.
std::string set_affinity(const char* mode);

}  // namespace rdrvr::diag
