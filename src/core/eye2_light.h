#pragma once
// [Stereo] PrepassEye2Light (run 21 item 5, research\run21\p3-light.md section 3; test only, off by default; needs
// [Stereo] PrepassEye2Collector=1 and everything that needs). Observe only: P3's second step, eye 2's Light Buffer made in
// pass 1 by the engine's own FUN_1406e0450 on eye 2's own cull view and viewport, from eye 2's final Shadow Collector and
// depth, kept in a mod-owned 1-slice image (LB2) and compared bit for bit with pass 2's own Light Buffer at the same call.
// No shader, descriptor or CB change; nothing reads LB2.
//
// Three points, all on the render thread in pass 1 (optional anchors; the analysed RDR.exe only):
//   (C) eye2_collector's OUT1 (playback): from_collector() copies the game's collector and depth, which hold eye 2's after
//       its directional term, into SC2L and D2 (both planes), before OUT1 puts eye 1's back. Gated on the install and the
//       stand-down, not on the run-time switch (an IN recorded while on may play back after "off"). A pass 1 whose (L) ran
//       no eye-2 call appends a clear marker after eye 1's call, so no later IN uses that frame's copy.
//   (F) FUN_140193060, the footprints (0x5cbfd0's call at 0x1405cc5df, return 0x5cc5e4), when the game draws them: marker
//       IN-F, SetCurrent(vp2), the game's call on eye 2, SetCurrent(cur), the render-state cache and the recorder's
//       vertex-declaration binding put back, marker OUT-F; then the game's own call for eye 1. IN-F: the game's collector
//       and depth saved, SC2L and D2 copied in; OUT-F: the collector into SC2L (eye 2's final) and the depth into D2, the
//       saves back.
//   (L) FUN_1406e0450, the lights (SceneRender's call at 0x1405c3e48, return 0x5c3e4d): marker IN-L; eye 2's cull view
//       made on this stack (FUN_140191920, then BuildCullView FUN_1408604a0(cv2, vp2, vp2 + 0x40)); SetCurrent(vp2);
//       o_6e0450(lighting, cv2); SetCurrent(cur); then put back what one call leaves (p3-light.md 1.3): the render-state
//       cache, the recorder's vertex-declaration binding and scissor (FUN_140ec7e90), DAT_142ad1cdc, the light-volume
//       list's count (DAT_142ad1d98 +8: appended, drained only after the colour walk) and every light's +0x60..+0x80
//       (the screen rect the cull writes); marker OUT-L; then the game's own call for eye 1. IN-L: the game's collector
//       and depth saved, and the Light Buffer (save_lb), SC2L and D2 copied in; OUT-L: the Light Buffer copied into LB2,
//       the saves back (the Light Buffer's too: eye 1's call starts from its own prior content). The Light Buffer's states
//       are the barrier watch's (its last transition in this list); not seen at IN-L: not saved, counted (lb_unsaved).
//       The Light Buffer itself is resolved from lighting +0x878 (the RT object's resource) and held with a reference.
//   pass 2 (a capture only): at (L), the game's collector and depth (both planes) to readback, the game's call, the
//       Light Buffer to readback. A capture also reads back the Light Buffer at IN-L (eye 2's prior content).
// Eye 2's call runs only when pass 1's collector step ran (eye2_collector::runs() moved in this pass) and, when the game
// drew footprints in this pass, (F) ran eye 2's. A refused IN or OUT after eye 2's call was recorded leaves eye 1 exposed:
// counted, and the module stands down for the session (as eye2_collector).
// The compare ("prepass e2lb compare N"): N captured frames compared on a worker thread: pass 2's inputs (collector,
// depth's 24 bits, stencil) against SC2L and D2; pass 2's Light Buffer against LB2. Classed by cut, footprints, interior
// (lighting +0x6c20) and the light counts (lighting +0x9c30: eye 2's call, eye 1's, pass 2's); a Light Buffer difference
// whose every texel is one eye 2's call left at its prior content (and eye 2 wrote some) is classed "prior only"
// (p3-light.md 3.6: pass 2 starts from eye 1's Light Buffer, eye 2 from last frame's).
// "prepass e2lb [on|off|compare N|reset|kv]". Off at run time: no marker, no copy, no call. Ini off: nothing installed.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>

namespace rdrvr::prepass {
extern std::atomic<bool> g_eye2_light_pending_flag;  // (defined in eye2_light.cpp) the playback side owes a step
}

namespace rdrvr::eye2_light {
void install();                            // bootstrap, after eye2_collector::install()
void poll(ID3D12GraphicsCommandList* cl);  // prepass::cut_poll, the list call's thread, when the flag is set
// eye2_collector's OUT1, the playback thread: the game's collector and depth (their followed states) hold eye 2's after its
// directional term; copied into SC2L and D2 and left in those states
void from_collector(ID3D12GraphicsCommandList* cl, ID3D12Resource* sc, ID3D12Resource* d, unsigned planes, D3D12_RESOURCE_STATES ssc,
                    const D3D12_RESOURCE_STATES* sd);
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "e2lb"
}  // namespace rdrvr::eye2_light

// [Stereo] PrepassEye2Images (eye2_images.cpp, test only): usable() installed, on and not stood down; LB2 and its generation,
// read on the playback thread
namespace rdrvr::eye2_light {
bool usable();
ID3D12Resource* lb2_image(uint64_t* gen, unsigned long* tid);  // tid: the thread its steps play on (ensure's)
// (GOAL-single-pass item 6b) SC2L, eye 2's final collector (OUT1's copy, then OUT-F's when footprints are drawn): what uint 57
// views. Made and retired with LB2 (the same generation and thread)
ID3D12Resource* sc2l_image(uint64_t* gen, unsigned long* tid);
// (GOAL-single-pass item 8f, test only: main_instanced's [Stereo] MainRegionE1Compare) D2L (PrepassEye2LightDepth: eye 2's depth
// after its lights, both planes; at rest COPY_DEST; null when not made) on the images' thread, as lb2_image. fresh: an OUT-L of this
// pass 1 wrote it (have2), not stood down, no step queued. game: the depth it was made for. d2_image: D2 (eye 2's depth before its
// lights; at rest COPY_DEST), the "seed" control's
ID3D12Resource* d2l_image(uint64_t* gen, unsigned long* tid, bool* fresh, ID3D12Resource** game = nullptr);
ID3D12Resource* d2_image(uint64_t* gen, unsigned long* tid, ID3D12Resource** game = nullptr);
// (item 8e 14, test only: main_instanced's region trace) the box of the images a draw of the traced pair just sampled, copied to
// rb at off[0] (the collector) and off[1] (the Light Buffer), each at most cap bytes, its footprint returned in fp[k] (the box's
// size in the image's own format): pass 1, SC2L and LB2 while lent on this list (PIXEL_SHADER_RESOURCE -> COPY_SOURCE -> back); pass 2, the
// game's collector (its followed state) and Light Buffer (the barrier watch's last state on this list). Nothing touched (false,
// why set) when a state is not known, the list is predicated (unless pred_nulled: the caller nulled the list's held predicate for
// this point, as rg_after does before its duplicates), or a box does not fit
bool trace_images(ID3D12GraphicsCommandList* cl, int pass, ID3D12Resource* rb, const uint64_t off[2], uint64_t cap, D3D12_SUBRESOURCE_FOOTPRINT fp[2],
                  const D3D12_BOX& box, bool pred_nulled, const char** why);
// (item 6b) the freshness hooks, null (the default: nothing called) unless PrepassEye2Images installed them.
// lights_done: the render thread, pass 1, at the end of (L) (eye 1's call returned), before the MRT bind and the main walk;
//   eye2_ran: eye 2's lights were recorded this pass 1 between IN-L and OUT-L, both markers appended, not stood down (so e2sc
//   ran this pass 1 and (F) ran eye 2's if the game drew footprints).
// check_marker: the render thread: appends the check marker. On the playback thread its step runs after every step before it
//   (this pass 1's OUT-L included: at once when none is queued, else queued behind them) and calls the sink with fresh = the
//   last IN-L / OUT-L pair played and OUT-L copied the Light Buffer into LB2 (SC2L final), not stood down; gen = the images'
//   generation then. A check never stands the module down (one not queued reports not fresh). tag (0..kCheckTags-1, false
//   otherwise): the caller's, carried by the marker's own callback and its queued op and handed back to the sink (the
//   caller's frame slot: the playback thread learns which frame the check is for)
inline constexpr int kCheckTags = 4;
using LightsDone_t = void (*)(bool eye2_ran);
using FreshSink_t = void (*)(bool fresh, uint64_t gen, int tag);
void set_lights_done(LightsDone_t fn);
void set_fresh_sink(FreshSink_t fn);
bool check_marker(int tag);
}  // namespace rdrvr::eye2_light

// (GOAL-single-pass item 6c, test only: pso's frame latch with "prepass mi allowtwins on") SC2L and LB2 lent to pass 1's main
// walk, so an eye2img twin may sample them (uints 57 / 58). They rest in COPY_DEST; lend(cl) records COPY_DEST ->
// PIXEL_SHADER_RESOURCE for both on cl, give_back(cl) the reverse on the same list. All on the images' thread (the one their
// steps play on), on a DIRECT list. One lend at a time, on one list:
//   lend: true when lent on cl (now, or already); false (nothing recorded) when not usable, stood down, the images not made,
//     another thread, not DIRECT, or lent on another list still open (lend_cross: that list's Close gives them back first);
//   give_back: true when not lent or given back on cl; false when lent on another list (nothing recorded);
//   owe_give_back: (pass 1's walk closed by its marker, no list) given back at the next list call on the lent list (poll), or
//     at that list's Close;
//   list_closing(cl): the lent list's Close (d3d_hooks hk_Close, before the original): given back on it;
//   list_reset(cl): the lent list's Reset (a list reset unclosed records nothing more: the lend is discarded with it).
// Every step of this module that copies into or out of SC2L or LB2 (poll's steps, from_collector) first gives them back on its
// own list when lent there (counted: by_step, expected 0); lent on another list the step is not run and the module stands down
// (lend_cross_step, expected 0: two lists open at once on the images' thread). Images made again while lent: the old ones are
// given back on the lent list as recorded (they stay alive 8 frames), the new ones are at rest. Nothing of this runs unless
// lend() was called; lent_flag is one relaxed load for the list hooks.
namespace rdrvr::eye2_light {
extern std::atomic<bool> g_lent_flag;
inline bool lent_any() { return g_lent_flag.load(std::memory_order_relaxed); }
bool lend(ID3D12GraphicsCommandList* cl);
bool give_back(ID3D12GraphicsCommandList* cl);
bool lent_on(const ID3D12GraphicsCommandList* cl);  // lent, on cl (the images' thread)
void owe_give_back();
void list_closing(ID3D12GraphicsCommandList* cl);
void list_reset(ID3D12GraphicsCommandList* cl);
std::string lend_kv();  // lends= given_back= at_close= owed= at_poll= at_list_close= by_step= cross= cross_step= reset_dropped= lend_refused= lent=
}  // namespace rdrvr::eye2_light

// [Stereo] PrepassEye2Velocity (eye2_velocity.h, test only): a function called in (L) on the render thread, in pass 1, right
// after eye 2's lights returned without a fault (vp2 still current), before SetCurrent(cur), snap_put and OUT-L's marker.
// Null (the default): nothing is called.
namespace rdrvr::eye2_light {
using LightTail_t = void (*)();
void set_light_tail(LightTail_t fn);
}  // namespace rdrvr::eye2_light

// [Stereo] Pass2FrontSkip (pass2_skip.cpp, research\run25\item7.md; item 7's S2, test only). Pass 2's lights (hk_light) and
// footprints (hk_foot) run through p2skip::around (a record-skipped stretch when pass2_skip decided the pass 2 skips); pass 1's
// (L) hands p2skip::pass1_lights what eye 2's front half did. P2IN (the op kP2In, its marker p2in_marker: the render thread,
// after pass 2's lights' stretch, before the save marker) copies SC2L, D2L and LB2 into the game's collector, depth (both
// planes) and Light Buffer on the playback thread, gated on have2 (an OUT-L of this frame wrote D2L and LB2, SC2L final); a
// refusal is p2skip::exposed2. d2l_installed: [Stereo] PrepassEye2LightDepth installed (D2L made). capturing: a compare's frame
// in flight. set_p2in_control: the copies P2IN leaves out (p2skip::kNoCopy* bits) and "stale" (OUT-L leaves LB2 an older
// frame's); p2in_kv: p2in_ok= p2in_refused= have2_set= p2in_ctl= stale_outl= p2in_marker_failed= nocopy= stale=
namespace rdrvr::eye2_light {
bool d2l_installed();
bool capturing();
bool p2in_marker();
void set_p2in_control(unsigned nocopy, bool stale);
void p2in_reset();
std::string p2in_kv();
}  // namespace rdrvr::eye2_light
