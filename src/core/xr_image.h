#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <string>

// (2026-11-09, the code review) one swapchain's image from its acquire to its release, across frames. A wait's
// XR_TIMEOUT_EXPIRED is a success code (XR_SUCCEEDED took it for a waited image: the image drawn while the compositor may
// still read it, its release refused as unwaited, the swapchain one image behind for the session). The spec: after a
// timeout the next wait waits on the same image. So an image is acquired once, waited until XR_SUCCESS (a timeout keeps
// it, waited again on the next call), drawn only once waited, released only once waited. Under xr.cpp's frame mutex (the
// presenting thread's frame calls, the session thread's frame end at a stop); the scope's pictures and the rings' atlas
// keep their own, already right.
namespace rdrvr::xr_image {

struct Held {
    XrSwapchain sc = XR_NULL_HANDLE;  // the swapchain the image is held from (another one: the old state dropped)
    uint32_t index = 0;
    bool acquired = false;
    bool waited = false;
    bool timed_out = false;  // its last wait timed out (the next one waits on the same image: counted as retried)
};

// True: image h.index is waited (draw into it, then release()). False: none this frame (the acquire failed, or the wait
// timed out or failed: the image kept, waited again on the next call). `what` names the swapchain in the log ("eyes",
// "ui", "wrist", "wheel", "deadeye", "cinema", "menu").
bool acquire(XrSwapchain sc, Held& h, const char* what, XrDuration timeout = 100000000);
// The waited image released (drawn or not); an image not waited is kept. False when nothing was released.
bool release(Held& h, const char* what);

uint64_t timeouts();          // waits that timed out (each kept and waited again)
uint64_t wait_errors();       // waits that failed
uint64_t release_failures();  // releases the runtime refused (none expected)
// "image waits timed out N (by swapchain), retried N, wait errors N, release failures N, acquires N, releases N, faked N
// (N to come)"
std::string status();
// "hud waittimeout <n> [what|all]" (the test's): the next n waits on that swapchain (or any) taken as timed out without
// calling the runtime's wait, so the image stays acquired and unwaited as after a real timeout. False: no such swapchain
bool fake_timeouts(int n, const std::string& what);

}  // namespace rdrvr::xr_image
