#pragma once
// [Stereo] MainInstanced (run 25, GOAL-single-pass item 3; research\run24\billboard.md sections 3 and 8; test only, off by
// default): the billboard compare. Compare only: no game target is redirected, no game draw is dropped or changed.
//
// The family: the tree billboards (VS ad643733f27ba4bd / PS 31e194eccc611691, pso.h main_listed), 23 draws a pass in bucket
// 1's post 0x5c1970 inside DVL call 1 (RenderVisPhase's DrawVisList call, mask 0xb, return 0x5b3e8a).
//   Render thread (dual_pass hk_DrawVisList at that call, each pass of a double frame): an op-0x4a open marker before the
//     call (pass 1 or 2, and whether the frame's gate holds: the sun cascades fitted to the union in pass 1 and, in pass 2,
//     skipped (Split_shadows) or the union's again (Split_shadowunion); in pass 1 also eye 2's rows written this frame),
//     the fallback close marker after it. The positive control ("control nudge|eye1") changes eye 2's rows between the two
//     in pass 1 only, and puts them back at the close.
//   Playback thread, in the region (open marker .. the game's CopyResource into "Depth Resolve", or the fallback marker):
//     a draw is listed when the list's current game pipeline is in the family's set; it must have the four targets bound
//     (FullScreenCopy, Velocity RT, Reactivity RT | DS Shadow Collector Depth Buffer) and no predication (an assert: such
//     a frame is excluded, the predicate never touched).
//     Pass 1, the first listed draw, before the game's: the mod's side arrays S and reference arrays R (2 slices each, the
//       four kinds, the game resources' descs) cleared (colour) and seeded (depth and stencil): slice 0 of both from the
//       game's depth, slice 1 of both from one eye-2 depth (the pre-pass depth array's slice 1 when PrepassInstanced has
//       it, else the game's again). The game's depth from DEPTH_WRITE (the state a writable DSV bind with a depth-writing
//       pipeline requires; checked against the walk cut's follow when it has the state) and back.
//     Pass 1, each listed draw, after the game's: the twin (view mask 3) into S's both-slice views, then the game's own
//       pipeline into R slice 0, the same arguments; the game's pipeline, its last bind and view mask 0 put back.
//     Pass 2, each listed draw, after the game's: the game's own pipeline into R slice 1; the bind put back.
//     Root signature, root arguments, vertex buffer, viewport, scissor and stencil reference stay the game's.
//   At pass 2's close of a good frame with a capture armed ("prepass mi compare N"): S and R (both slices, 5 planes) to
//     readback, a fence, a worker: eye 2 (S1 against R1), eye 1 (S0 against R0), the negative control (S1 against R0's
//     FullScreenCopy), Reactivity all zero, coverage (R1's texels the billboards wrote).
// A frame is good when both passes' gates hold, no listed draw was predicated or refused, the listed counts are equal and
// non-zero, each pass's listed draws are one contiguous run, and pass 1 seeded.
// "prepass mi [on|off|compare N|reset|kv|control off|nudge|eye1]". Off at run time: no marker, no draw, no copy. With the
// (GOAL-single-pass item 6c, test only) "prepass mi allowtwins on|off" (off by default): pso's frame latch may hand the listed
// families eye2img and prevworld twins (pso.h walk_open .. walk_close; the billboards have neither, so nothing draws one yet);
// "prepass mi latch": its counters, eye2_light's lend and the prevworld gate's (kv). The region's open, each draw and its close
// call pso's latch; DVL call 1's open marker is followed by the prevworld verdict (armed only). With the
// ini switch off nothing is installed or made (each hook site: one relaxed load). Another RDR.exe build: off.
//
// [Stereo] MainSlice (run 25, GOAL-single-pass item 5; research\run25\slice.md; test only, off by default; needs MainInstanced=1):
// the billboard slice for real. Pass 2 drops the family's listed draws, and eye 2's billboards come from pass 1's twin through
// a depth-tested merge at the drop point (pass 2's first listed draw, bucket 1's post):
//   Pass 1 of a slice frame (latched by the open marker, as the control): S slice 1 is seeded far (depth 1.0, stencil 0), not
//     from eye 2's depth, so it holds per texel the fragment pass 2 would keep (slice.md 1.2); S's depth array is typeless
//     (R24G8, DSV 45) so it can be read. The four SRVs of S slice 1 (colour, velocity, depth plane 0, stencil plane 1) are
//     partition-1 records from the game's DescAlloc (eye2_images' route), kept for the session, written again 8 presents after
//     an array re-make (the drop held until then).
//   The merge pipeline, one per listed pipeline (a worker; pso::OwnCreation): the mod's VS (one covering triangle) and PS (the
//     four SRVs read through the game's bindless t-space 2 at the records' indices; discard where S1's stencil is 0; colour,
//     velocity and SV_Depth from S1), compiled with D3DCompile (vs_5_1 / ps_5_1), made with the family pipeline's own root
//     signature, blend and depth-stencil state (verbatim), so it changes only the pipeline. The family's desc is checked first
//     (LESS_EQUAL or LESS with writes; stencil ALWAYS / REPLACE / depth-fail KEEP or REPLACE, both faces equal; blend off; 3
//     RTs; DSV 45; triangles; one sample): another value, that pipeline is not sliced.
//   Pass 2, the first listed draw: decided before anything is dropped (the frame latched, pass 1's region whole: good, seeded,
//     a twin for each listed draw, one run; the arrays unchanged; the SRVs written for them; the merge ready for this pipeline;
//     the four targets bound; no predication). Then [compare frames] the game's targets copied into R slice 1; S slice 1 to
//     shader-resource states; the merge drawn (3 vertices) and the game's pipeline put back; S slice 1 back; [compare] the
//     targets copied into C, the game's pipeline drawn into R slice 1; the draw dropped. Listed draws 2..listed1: [compare] the
//     reference into R slice 1, then dropped; past listed1 drawn by the game (counted). A barrier on the four targets inside the
//     run excludes the frame. Refused: no drop this frame (counted by reason), the game draws its own.
//   The compare (G1): C (the merged targets) against R slice 1 (the game's own 24 draws at the drop point), bit for bit on five
//     planes; eye 1 S0 = R0 as before; the negative control C against R0; coverage = S1's marked texels, split into passing and
//     failing (R1's depth equal to S1's or not). Controls: "slice nomerge" (dropped, not merged) and "slice always" (the merge
//     with DepthFunc ALWAYS) must fail G1 where their case occurs; "control nudge" as item 3.
// The handover is also refused when pass 2's first listed pipeline is not pass 1's, or when a query that counts draws
// (anything but a timestamp) is open on the list at the drop point (BeginQuery is hooked only with MainSlice=1 at the list
// hooks' install); a query begun or ended inside the dropped run excludes the frame after the fact (counted: the test's gate).
// The control "slice identity": no merge, no drop; on compare frames T into R slice 1, the game draws its own listed draws
// (each also into R slice 1), and C is taken after the last one: C must equal R1 (R1 is the game's real result).
// "prepass mi slice on|off|nomerge|always|identity" (the next pass 1 latches it). With the ini switch absent nothing of this is made:
// no DescAlloc, no D3DCompile, no merge pipeline, no C, no typeless S; MainInstanced alone is item 3.
//
// (GOAL-single-pass item 6, research\run25\families.md; test only, each off by default, each needs MainInstanced=1)
// [Stereo] MainFamily=bb|cw|bs|fb (pso.h): the one family listed this session (bb, the billboards, the default). A family that
//   writes no depth and no stencil (the three: 007, EQUAL) is seeded only when the walk cut's follow has both depth planes in
//   DEPTH_WRITE; else its frames are refused as before.
// [Stereo] MainPredication=1 (item 6d): DrawVisEntity's mode 2 (0x640120) brackets each entity with SetPredication(its pre-pass
//   query) / (NULL), so every listed draw of the three families is predicated in pass 1. d3d_hooks keeps per list the predicate
//   its SetPredication hook passed on (d3d::thread_predicate; a Reset makes it "none", a Close forgets it). A predicated listed
//   draw then keeps its own predicate; its duplicates (twin, reference) are drawn under SetPredication(null) and the held predicate
//   is put back right after the last one (counted: held, nulled, put back; balanced). The record unknown (another list's): the
//   frame is excluded as before (never guessed); the record and thread_predicated() disagreeing, or the record moving across the
//   duplicates: MainPredication stands down (excluded as before from then on). Never on a MainSlice frame. "prepass mi pred
//   on|off|refs" (off: item 3's exclusion; refs: pass 1 without a twin still draws R0, its frame excluded: the null and the
//   restore exercised on a family not twinned yet).
// [Stereo] MainUnionCull=1 (item 6e): pass 1's DVL call 1 culls against both eyes' union (render thread, around the call, as
//   PrepassUnionCull's walk): the current viewport's side planes +0x390 (IsSphereVisible) and side-plane matrix +0x3f0 (SetWorldMtx
//   0x1348e0 makes +0x2c0 = World x it, grmModel's culler 0x142b00 reads +0x2c0) set to camera_lever's union viewport's, +0x2c0
//   made again with them; at the call's return the eye's back (+0x2c0 made again) unless the game rewrote a block inside the walk
//   (counted, its value kept). A marker after the open's tells the playback (no marker: the swap undone at once). Pass 2 is never
//   touched. The playback keys each listed draw (pipeline, arguments, vertex buffer slot 0; also without the buffer) and checks
//   pass 2's against pass 1's as multisets: per frame "within" (no miss) by category (union-culled or the control), the listed
//   and region draw counts per pass (the cost), and pass 2's order-free digest. "prepass mi ucull on|off" (from the next pass 1).
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/d3d_hooks.h"

struct ID3D12GraphicsCommandList;
struct ID3D12PipelineState;
struct ID3D12Resource;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct D3D12_RESOURCE_BARRIER;

namespace rdrvr::main_inst {
extern std::atomic<bool> g_armed;   // installed and on: the render thread records markers
extern std::atomic<bool> g_listen;  // a main region is open on a playback thread: the list hooks call in
extern std::atomic<bool> g_noting;  // installed (set once at install): the list hooks note pipelines and binds
inline bool armed() { return g_armed.load(std::memory_order_relaxed); }
inline bool listening() { return g_listen.load(std::memory_order_relaxed); }
// the list hooks note the list's pipeline and bind (per thread) once installed, whatever the run-time switch: the family's
// bind is made before the region opens (segment e's MRT bind, 0x5c3e8d); the notes are dropped at the list's Reset and Close
inline bool noting() { return g_noting.load(std::memory_order_relaxed); }
extern std::atomic<bool> g_slice;  // ([Stereo] MainSlice or MainRegionE1Compare installed, set once at install): the list-hook calls (barrier, query)
inline bool slice_installed() { return g_slice.load(std::memory_order_relaxed); }
void install();          // bootstrap, after prepass::install()
uintptr_t dvl_ret();     // DVL call 1's return address (MainDvlCall + 5), 0 when not installed
// the render thread, dual_pass hk_DrawVisList at DVL call 1 (pass 1 or 2), before and after the call
void dvl_open(int pass);
void dvl_close(int pass);
// the list hooks while listening() (each returns at once on another thread or outside the region)
void note_pipeline(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps);
void note_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
               const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
// kDuplicate: a listed draw to duplicate after the game's (seeded if pass 1's first); kDrop ([Stereo] MainSlice): not drawn (the
// hook returns before the original; merged at pass 2's first listed draw of a handed-over frame)
enum Draw : int { kNone = 0, kDuplicate = 1, kDrop = 2 };
int before_draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args);
void after_draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args);
// ([Stereo] MainSlice) a ResourceBarrier while listening: one on the family's targets inside a handed-over run excludes the frame
void barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);
// ([Stereo] MainSlice) a BeginQuery / EndQuery (not a timestamp) while listening: inside a handed-over run it excludes the frame
void query(ID3D12GraphicsCommandList* cl, bool begin);
// ([Stereo] MainRegionE1Compare, item 8f) a ClearRenderTargetView / ClearDepthStencilView (the game's view) while listening: one of the four
// targets cleared between T and C2 refuses C2
void clear(ID3D12GraphicsCommandList* cl, uintptr_t view);
// (item 8f) a CopyResource / CopyTextureRegion (its destination) while listening: one of the four written between T and C2 refuses C2
void copy_write(ID3D12GraphicsCommandList* cl, const ID3D12Resource* dst, const ID3D12Resource* src);
// (item 8f's remap, "prepass mi region e1f remap on|off") a ResourceBarrier while listening, before the walk cut follows it: a batch that
// would end the remap closes it first (S0 -> the four)
void pre_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);
// (item 8f's remap) a SetPredication (any list, what it now has): the remap list's predicate kept for its close
void note_predication(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, uint64_t off, int op);
void before_copy(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst);  // a CopyResource: "Depth Resolve" closes the region
void poll(ID3D12GraphicsCommandList* cl);  // prepass::cut_poll, when prepass::g_main_pending_flag (a readback owed)
void list_ends(ID3D12GraphicsCommandList* cl);  // a list's Reset or Close (while noting): this thread's notes of it dropped
// (GOAL-single-pass item 6c) a list's Close (while noting), before the original: SC2L and LB2 lent on it given back on it
void list_closing(ID3D12GraphicsCommandList* cl);
bool withholding();  // installed, on and not stood down: pso withholds the family's twins from the pre-pass (latched per frame)
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "mi"
// ---- (GOAL-single-pass item 8d, main_region.h; test only) what [Stereo] MainRegion's observe reads of the main region
void indirect(ID3D12GraphicsCommandList* cl);  // an ExecuteIndirect while listening (main_region's "indirect" single)
bool ucull_installed();                        // [Stereo] MainUnionCull installed (the draw keys' vertex buffers noted)
// the playback thread, the open region: pass 1's union marker played (uc); this frame's exclusion by order (kXOrder); the list's
// last bind is the family's four targets (check_bind's test, cached per note_bind: its four lookups once a bind, not once a draw)
bool pb_union_culled();
bool pb_frame_order();
bool region_bind_ok(ID3D12GraphicsCommandList* cl);
// (item 8d tier 2; the render thread) pass 1's DVL call 1 while union-culled on viewport vp: the eye's own side planes (+0x390, the
// 4 planes the swap replaced, as saved there) into out; false when vp's planes are not swapped now
bool uc_eye_planes(uintptr_t vp, float out[16]);
// ---- (GOAL-single-pass item 8e, research\run25\item8e.md; test only, off by default) [Stereo] MainFamily=region (pso.h; needs
// MainRegion=1, MainUnionCull=1, MainPredication=1, InstancedShadersAuto=1, read from the ini at this install, and pso's own switch at
// the device, checked lazily; MainSlice refused): with "prepass mr compare" every
// non-empty draw of pass 1's and pass 2's e1 (main_region's classifier: in e1, non-empty, its pair's ordinal in the subset latched at
// pass 1's open) is listed on dup frames only (a capture armed, 1 in N: "prepass mi region every N", N >= 8 except solo). Pass 1: the
// twin from pso::region_twin_of (lock-free; the side arrays' formats and sample count checked; an instprev twin's "ok" marker spent
// once drawn; "prepass mi region ip off" keeps them refused) into S's both slices, the game's pipeline into R0; pass 2: the game's
// pipeline into R1; both passes refuse a bind whose view formats, or a game pipeline whose formats, are not the arrays'; every restore
// as before (pipeline, bind, view-instance mask, predicate); the list's
// ID3D12GraphicsCommandList1 cached per list (released at each region close and at the list's Reset or Close). The seed: both depth
// planes' state from the walk cut's follow. The frame rule is 8d's verdict (mr_verdict, pushed by main_region at pass 2's e1, its frame
// number the one main_region pushed at this frame's pass-1 open, mr_open): judged,
// no failed gate, no single (anywhere in e1 for joint; in a listed pair for solo / subset), within (misses 0); plus seeded, the arrays'
// generation, nothing predicated or refused, a dup frame. Joint ("prepass mi region all") / solo F ("solo <vs8>:<ps8>") / subset ("subset
// <hex>", the bisect); "swap off|auto" (joint, information: the next pair drawn in the last judged frame drawn by the game's pipeline
// into S0 and S1 instead of its twin); per-pair results by ordinal ("prepass mi region kv [first]"). The ip control: "prepass instprev
// main control nudge" (control kind 5). Off (MainFamily another): none of this runs; each site one relaxed load or one bool
bool region_family();  // installed with MainFamily=region (its prerequisites installed) and pso's own switch on
// (item 8e, research\run25\item8e.md 14) body.cpp's entity hook, the render thread: while own_armed() (MainFamily=region installed and "prepass
// mi region own on", the default), the entities it edits per pass are bracketed by own_mark(true) / own_mark(false), stream markers the
// playback plays in order; the region's listing leaves the draws between them to each pass
bool own_armed();
void own_mark(bool on);
struct MrVerdict {     // (main_region's finalize, the playback thread) the frame's verdict as 8d judges it
    uint64_t frame = 0;        // main_region's frame number (state::presents at its pass-1 open on the render thread)
    bool judged = false, within = false, rep = false;
    bool ctl = false;          // mi's control gate held (a control frame: not a failed gate there)
    uint32_t gates = 0;        // the failed gates other than the control (main_region's bits)
    uint32_t unj = 0;          // why not judged (main_region's bits)
    uint32_t cats = 0;         // the single classes seen (main_region's bits)
    uint32_t singles = 0;      // pass 1's e1 singles, all
    uint32_t noord = 0;        // pass 1's non-empty e1 draws of pairs with no ordinal (never listed)
    uint64_t single_ords = 0;  // the ordinals with a single in pass 1's e1
    const uint16_t* draws = nullptr;  // pass 1's non-empty e1 draws by ordinal [64] (the call's only)
};
// main_region's pass-1 open (the playback thread, right after main_inst's own open marker): the frame number its verdict will carry,
// latched only inside main_inst's pass-1 region of a region frame (the latch cleared at each pass 1's open; else counted stale)
void mr_open(uint64_t frame);
// inside main_inst's pass-2 region of the same frame only (else counted stale), its frame the one mr_open latched (else counted)
void mr_verdict(const MrVerdict& v);
// ---- (GOAL-single-pass item 8f, research\run25\item8f.md "8f: the plan"; test only, off by default) [Stereo] MainRegionE1Compare=1
// (needs MainFamily=region installed and PrepassEye2LightDepth=1; read at this install) with "prepass mi region e1f on" (off by default):
// on joint dup frames slice 1 of S and R is seeded from eye 2's own images (depth D2L, stencil D2; V2 under DLSS, a clear under FXAA; reactivity 0; FSC the
// 8f0b sentinel), checked fresh, on the thread and of the bound targets' descs first (else the frame excluded, kXE1fSeed by reason); R
// gets no duplicates (R0 <- T, the game's four at pass 2's e1's first non-empty draw; R1 keeps the seed); pass 2's own draws (body.cpp's
// brackets) are drawn again into S1; C <- C2, the game's four at pass 2's e1 end (mr_e1_end: owed, taken at the next list call on the
// thread of pass 2's last e1 draw, the bind and the four's barriers unchanged since). The worker's G0 = R1 against T, G1 = S1 against
// C2 (FSC: covered texels against C2, the sentinel's against T), the negative S1 against S0, coverage. "prepass mi region e1f
// [on|off|pred live|null]" (pass 1's twins under the list's live predicate by default); controls: the render thread's (nudge, eye1,
// e2img, pw) and the playback's "prepass mi control seed|single <vs8>:<ps8>" (S1's stencil from D2L; the pair into S0 only, no twin)
void mr_e1_end(int pass);
int pb_pass();  // (r117 DG4's depthlog, test only) the region's pass (1 or 2) on its thread while open; 0 otherwise  // main_region's pb_e1 (the playback thread, e1's end marker of each pass)
// (item 6e's draw keys, made general for 8d's misses census: a table set per user, 6e's own unchanged) a frame's keys: pass 1's
// counted in, pass 2's counted out (a miss: not found); [0] with the vertex buffer slot 0, [1] without; generation-stamped, no clearing
namespace keys {
struct Slot {
    uint32_t gen, n;
    uint64_t key;
};
struct Tables {
    Slot* tab[2];
    unsigned slots;  // (a power of two)
    uint32_t gen;
};
struct Frame {
    bool over = false;  // a table full: the frame not judged
    uint32_t miss[2] = {}, n1 = 0, n2 = 0;
    uint64_t sum1 = 0, sum2 = 0;  // order-free digests of each pass's keys (with the vertex buffer)
    bool rep = false;             // (the family review's should-fix 3) a pass-1 key drawn twice: "within" cannot tell its instances apart
};
void begin(Tables& t);  // a new frame's tables (the generation; cleared once at its wrap)
// the draw's two keys: the pipeline, the arguments, this thread's last vertex buffer slot 0 on cl (with) or none (without)
void make(ID3D12GraphicsCommandList* cl, const void* pso, const d3d::DrawArgs& a, uint64_t* with_vb, uint64_t* without_vb);
// pass 1: counted in; any other pass: counted out. Returns bit 0 a miss with the vertex buffer, bit 1 without; (pass 1, item 8d)
// bit 2 this key (with the vertex buffer) already counted in this frame (Frame::rep set as well; 6e ignores the return)
unsigned count(Tables& t, Frame& f, int pass, uint64_t with_vb, uint64_t without_vb);
uint64_t digest(uint64_t key);  // (a digest's term)
}  // namespace keys
}  // namespace rdrvr::main_inst
