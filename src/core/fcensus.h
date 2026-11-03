#pragma once
// [Stereo] RouteFCensus (GOAL-single-pass item 8h', step 1: research\run25\item8f_census.md; test only, off by default, never
// in a shipped or user ini): route F's state census. Read only: it snapshots CPU-side caches of the game inside DVL call 1's
// e1 (its open to the first ForestDraw mode 0) of both passes of a double frame and compares pass 1 with pass 2. It never
// skips, writes or calls a game function; every read of the game's memory is under SEH, bounded, and skips null pointers.
//   The points (the design's section 1): E0 the first DrawVisEntity of bucket 0 from DVL's entity loop (body.cpp's hook, before
// its own work), P0 bucket 0's post body 0x6ddbe0 (its entry; a new detour), E1 the first DrawVisEntity of bucket 1, P1
// bucket 1's post 0x5c1970 (its entry; a new detour), F ForestDraw mode 0's entry (dual_pass's hook). The two detours count only
// when entered from DVL's post-callback calls (their return addresses, checked at install), directly inside DVL call 1.
//   The fields (section 2): 1 the render-state shadow (state 0xc excluded), 2 the derived device state and the device cache,
// 3 the fade, 4 the AO modifier and its flag, 6 the rim object's caches and statics, 7 the tree-wind caches, 8 the light
// cursor, 9 the world matrix and its copy, 10 the global shader-variable CB shadows (each variable its own element; the CBs of
// the per-eye camera variables excluded as derived, judged through 9), 11 0x142272484. 5 and 11's 0x142272488/0x142ac1d47 are
// asserts; 12's storage is unknown (its device slot is logged once).
//   The predicate (section 3), per element of each field at P0 and P1 (bucket b): D1 = pass 1's entities changed it (E_b ->
// P_b), D2 the same in pass 2; route F leaves L = D1 ? pass 1's P_b value : pass 2's E_b value, ok = (L == pass 2's P_b value).
// The spec's rule (D1 == D2 and (not D1 or X1 == X2)) is reported alongside as "strict" (it also fails D1, not D2, X1 == X2,
// which is exact). At F the elements with D1 at bucket 1 must be equal between the passes (L = X1F: both rules). A bucket a
// pass drew no entity in has D false there. The device cache's keep-alive vectors (0x142b073f0, 0x142b07470: per frame slot
// begin/end/cap) are a cursor class, not judged: pass 2's pushes per point are reported (what route F leaves behind).
// "prepass fcensus [on|off|reset|dump|kv]". Ini off: nothing installed; each call site is two relaxed loads.
//   Route F's probe (item 8h' step 2; test only, the simulator only: eye 2 is knowingly wrong while it skips), "prepass
// fcensus probe off|skip|skipwb|reset|kv", independent of the census's on/off (the census, when on as well, judges what the
// probe leaves). Latched per double frame at pass 1's DVL call 1 open (refused while MainInstanced is armed, the p2skip probe
// is on, renderer +0x1fa6 is set), re-checked at pass 2's (refused when pass 1 is missing or of another frame):
//   skip    in pass 2 only, inside DVL call 1's e1 (its open to ForestDraw mode 0), DrawVisEntity calls from DVL's entity loop
//           (return 0x6421ae) of buckets 0 and 1 are not made: entity() returns true and body.cpp's hook returns before all of
//           its own work (setting t_draw_pass only, the value the call would have left). What DrawVisEntity does that must not
//           be lost is replicated: with arg 7 == 2 the query slot *(u16*)(rec + 12 + 16*(b >> 1) + 2*(b & 1)) is zeroed when
//           non-zero (the tail 0x1406413bf-0x1406413d4; the begin op at 0x6401d7 and the end op at 0x6413b9 are both skipped,
//           so the stream stays balanced), under SEH
//   skipwb  skip, and at P0 and P1 (before the callbacks run) pass 1's exit values of item8f_census.md section 7's set written
//           back from this frame's pass-1 snapshots (only the elements pass 1's bucket changed, D1, found at pass 1's P_b): the
//           CB shadow elements (FUN_140ec8bd0's map, then the store: SetShaderGlobal's pattern), the AO modifier's cache, flag
//           and CB copy (DrawVisEntity 0x140640a65-0x140640af6's pattern), the rim statics and their CB copies (FUN_14072b210's),
//           rimobj +0x2c (a plain store, as 0x140640476/0x14064051e; reset to pass 1's value), the world matrix through
//           ViewportSetWorldMtx 0x1329e0 (its Regenerate re-derives pass 2's WV/WVP), last. Never at F: P1's write-back covers
//           what bucket 1's post reads, and its prefix is the same code in both passes (the census's F rule).
// Any SEH fault in the probe's own reads or writes stands it down for the session (logged as an error).
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace rdrvr::fcensus {

extern std::atomic<bool> g_on_flag;     // the census
extern std::atomic<int> g_probe_flag;   // route F's probe: 0 off, 1 skip, 2 skipwb
extern std::atomic<bool> g_seg_flag;    // (item 8d) [Stereo] MainRegion's segment tags: the points run with the census and probe off
inline bool on() {
    return g_on_flag.load(std::memory_order_relaxed) || g_probe_flag.load(std::memory_order_relaxed) != 0 || g_seg_flag.load(std::memory_order_relaxed);
}
// (GOAL-single-pass item 8d, main_region.h; test only) installed: the points exist; set_segments: main_region's observe (E0, P0, E1
// and P1, the first time each is reached in order inside e1, call main_region::segment there, both passes)
bool installed();
void set_segments(bool on);

void install();  // bootstrap, after p2skip::install(): with [Stereo] RouteFCensus=1, the two detours and the census on

// dual_pass's DrawVisList hook, around every call while on() (call1: the call returns to MainDvlCall + 5; list: its rcx)
void dvl_enter(int pass, bool call1, void* list);
void dvl_leave(int pass, bool call1);
uintptr_t dvl_ret();  // DVL call 1's return address (MainDvlCall + 5), 0 when not installed
// body.cpp's DrawVisEntity hook, first (ret: its return address; only the calls from DVL's entity loop count). True: route
// F's probe skips this call (the hook returns at once, the original not called)
bool entity(void* const* rec, uint64_t bucket, uint64_t a7, uintptr_t ret);
// dual_pass's ForestDraw hook, mode 0, first (F: the first one directly inside a census'd DVL call 1)
void forest_e1();

std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "fcensus"

}  // namespace rdrvr::fcensus
