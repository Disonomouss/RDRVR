#pragma once
// The checked reads' VirtualQuery (2026-10-29, round 42: a simulator cycle fell from 80 Hz to 4 Hz and stayed there,
// the render thread 98% in NtQueryVirtualMemory). RDR.exe's .data section is 1.4 GB (RVA 0x1f5b000, almost all of it
// .bss) and the game keeps its objects there: VirtualQuery's RegionSize walks the pages from the one asked about until
// the protection changes (written PAGE_READWRITE against never-written PAGE_WRITECOPY), so once the game's writes join
// the runs each call walks hundreds of thousands of page table entries (about 10 ms; normally 20-45 us,
// research\run3\sliver.md). One-way.
//
// The fast path (r236, the second design: the first trusted all of .data and .rdata, but .data held 29 regions of
// another protection by the time the game had loaded, and init's walk refused both sections): each check asks the
// working set about the very pages it reads (QueryWorkingSetEx: a fixed cost per page, no walk). A page present in the
// working set (Valid) is committed, and its Win32Protection is the page's own: readable (by the caller's mask, not a
// guard page) is as good an answer as VirtualQuery's, at the same moment. A page not present (paged out, never touched)
// or of another protection, more than 8 pages, or anything outside RDR.exe's image keeps VirtualQuery. No cache and no
// trust kept between checks: nothing to go stale, no window wider than VirtualQuery's own check-then-read.

#include <cstddef>
#include <cstdint>
#include <string>

namespace rdrvr::image_mem {

void init();  // once (body::install, the bootstrap thread): the image's headers, .data's and .rdata's first regions logged
// RDR.exe's image bounds and its .rdata (from the in-memory headers; diag's vtable search)
void image(uintptr_t* lo, uintptr_t* hi);
void section(const char* name, uintptr_t* lo, uintptr_t* hi);  // 0, 0 when the image has no such section

// The protections a caller takes as readable (the low byte of a page protection): body's readable and d3d's take code
// too, data_readable data only
constexpr unsigned kReadData = 0x02 | 0x04 | 0x08;    // PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY
constexpr unsigned kReadAny = kReadData | 0x20 | 0x40 | 0x80;  // and PAGE_EXECUTE_READ, _READWRITE, _WRITECOPY
// [a, a + n) inside RDR.exe's image, at most 8 pages, every page in the working set with a protection in `mask` and not
// a guard page, with the fast path on (counted as a hit). False for a = 0, n = 0 or anything else: VirtualQuery then.
bool fast_readable(uintptr_t a, size_t n, unsigned mask);
bool fast_on();
void set_fast(bool on);  // "skel vqfast on|off" (the session only; on at start)

// The checked reads' VirtualQuery, timed and counted by caller: 0 body.cpp's readable / data_readable, 1 d3d_hooks'
enum Src { kBody, kD3d, kSrcCount };
bool query(uintptr_t a, void* mbi, Src src);  // mbi: a MEMORY_BASIC_INFORMATION; true when VirtualQuery filled it

struct Snap {
    uint64_t calls[kSrcCount], ticks[kSrcCount], fast;
    uint64_t ws_calls, ws_ticks, ws_absent, ws_other;  // the fast path's QueryWorkingSetEx: calls, time, a page not
                                                       // present, a present page of another protection
};
void snap(Snap* s);
// The slowest call since the last take on that slot (0 the [xr] timing window, 1 the "perf" window, 2 "skel vq"): its
// us and address (RDR.exe's RVA when inside it), read and cleared together
void take_slowest(int slot, double* us, uintptr_t* addr, bool* in_image);
double ticks_us(uint64_t ticks);
// One line: per frame over `frames` between a and b, and the slot's slowest. quiet (optional): no VirtualQuery call
// between a and b and the slowest under 1 ms (the [xr] timing line's own vq line is then left out)
std::string format(const Snap& a, const Snap& b, uint64_t frames, int slot, bool* quiet = nullptr);

// "vq" (the test thread, read only): one walk of RDR.exe's .data (regions, RW and WRITECOPY runs, the longest, and each
// region of another protection: its RVA, size, State, Protect, AllocationProtect and Type, all in the log, the first 24
// in the reply, with a signature of them all to compare two walks), at the per-frame anchors' globals a timed
// VirtualQuery (the median of 5) with its RegionSize and a timed QueryWorkingSetEx, the process's region count
std::string report();

}  // namespace rdrvr::image_mem
