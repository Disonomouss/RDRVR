#pragma once
// The single-pass probe (research\sps\current.md 3.6-3.7): where each thread's frame goes. Timers once per pass, wait
// or frame, never per draw (QPC only, plus one QueryThreadCycleTime a frame on each of the three threads), summed in
// atomics and reported as per-frame means between two snapshots: in the test channel's perf status and the log's
// "[xr] timing" line. [XR] TimingProbe (on): off, nothing is timed and RecorderWaitDone is not hooked.

#include <cstddef>
#include <cstdint>

namespace rdrvr::frame_probe {

enum Slot {
    // the render thread, inside SceneRender (camera_lever.cpp)
    kPre,         // before the first pass: the hand latch, body::before_scene, the late latch, begin_frame
    kPass1,       // the first eye's pass (with its wait on +0x88)
    kMid,         // between the passes: the first eye's post (with the wait on +0x38), the gust and sunvis put back
    kPass2,       // the second eye's pass
    kMono,        // a mono frame's one pass (with its wait on +0x88)
    kDataWait,    // SceneRender's wait on +0x88 for the main thread's frame data (pass 1, or mono)
    kUiWaitMid,   // the wait on +0x38 for the main thread's UI data, taken between the passes (dual_pass.cpp)
    // the render thread, RenderFrame outside SceneRender
    kUiWait,      // RenderFrame's own wait on +0x38 (mono, or stereo when it was not taken between the passes)
    kLockWait,    // its two waits on +0x40
    kFrameWait,   // its wait on +0x80
    kPlayWait,    // EndFrame's wait for the playback thread to replay the chunk (RecorderWaitDone)
    // the playback thread ("RenderThread")
    kPlayIdle,    // the playback loop's wait for a recorded chunk
    kFrameEnd,    // the mod's frame end before Present (xrWaitFrame, the eye copies, xrEndFrame, the listeners)
    kXrWait,      // the xrWaitFrame block inside it
    kPresent,     // the game's Present
    // the main thread
    kMainWait,    // PreRender's wait on +0x18 for the render thread's frame
    kSlotCount
};
enum Thread { kMain, kRender, kPlayback, kThreadCount };

bool on();
// The render thread's scene phase, for the stack sampler (diag::sample_stacks): 0 outside SceneRender, 1 before the
// first pass, 2 pass 1, 3 between the passes (the first eye's post), 4 pass 2, 5 a mono frame's pass.
enum Phase { kOutside, kBefore, kPhasePass1, kBetween, kPhasePass2, kPhaseMono, kPhaseCount };
void set_phase(Phase p);
Phase phase();
bool set_on(bool on);                  // the test's switch (only once installed): its own cost, on against off
void add(Slot s, double ms);
void thread_frame(Thread t);           // once a frame on that thread: its CPU cycles since its last call
void render_frame_start(double now);   // SceneRender's start (render thread): the frame period, the render cycles
void playback_frame(uint64_t draws);   // at Present (playback thread): the thread's D3D12 draw count, its cycles
void install();                        // after dual_pass::install (the wait sites): [XR] TimingProbe, the hook

struct Snap {
    uint64_t us[kSlotCount];
    uint64_t cycles[kThreadCount];
    uint64_t frames, stereo, mono, period_us, periods, draws, tsc;
    double ms;
};
void snap(Snap* s);
// The per-frame means from a to b, one line ("probe ..."); "no frames" when none was rendered between them.
void format(const Snap& a, const Snap& b, char* out, size_t len);

// The game's own frame limiter (graphicsOptions' FrameRateLimit, 90 fps here), for an uncapped measurement: "off" sets
// its period to 0 in memory, "on" puts back the value first seen; anything else only reads it. Never on its own.
void game_frame_limit(const char* arg, char* out, size_t len);

}  // namespace rdrvr::frame_probe
