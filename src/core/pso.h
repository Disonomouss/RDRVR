#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Pipeline census and per-shader draw skip (round 3b: find the shader that draws the distant trees that turn with the
// head). Graphics pipelines are recorded with a hash of their vertex shader's bytecode (FNV-1a 64 over the container,
// the same bytes as the .dxbc blobs under research\shaders); draws are counted per vertex shader each frame, and the
// draws of one vertex shader can be skipped. A test aid; it only reads and (when asked) skips draws.
namespace rdrvr::pso {

void install_device_hooks(ID3D12Device* dev);
void install_list_hooks(ID3D12GraphicsCommandList* cl);
// From the draw hooks: true = skip this draw. Counts the draw against the list's current pipeline.
bool on_draw(ID3D12GraphicsCommandList* cl);
// After a draw that was not skipped: its first vertex and count (for the vertex capture).
void on_draw_args(ID3D12GraphicsCommandList* cl, int64_t first_vertex, uint32_t count);
// "pso census [n]" / "pso skip <hash>|off" / "pso dump <dir>" (the vertex shaders drawn last frame, as .dxbc).
std::string command(const std::string& line);
// The census on from now (the frame graph arms it); the list's current pipeline as the census knows it: its vertex and
// pixel shaders' hashes and what it writes (bits 0-7 the render targets, bit 8 depth). False: not known (another list).
void arm_census();
bool current(const ID3D12GraphicsCommandList* cl, uint64_t* vs, uint64_t* ps, uint32_t* writes);
// (r117 DG4's depthlog, test only) the pipeline the mod last set on cl with set_raw, if no game SetPipelineState came after it; else null
const void* raw_current(const ID3D12GraphicsCommandList* cl);
// [Stereo] InstancedShaders=<folder> (single-pass stereo, the pre-pass's milestone 4a step 1; test only, off; 2026-10-14):
// for each game pipeline whose vertex shader has a rewritten vs_<16 hex fnv>.dxil there and whose "vs ps" pair is a line
// of the folder's twins.txt (research\sps\tools\vi\prepass_twins.py's ready list), a view-instanced twin is made next
// to it (2 views on viewport 0, render target slices 0 and 1, masking on: psotest's array0 desc, 441 of 441 made offline
// on the developer's driver). Nothing draws with them yet. nullptr: no twin.
ID3D12PipelineState* twin_of(ID3D12PipelineState* game);
// [Stereo] PrepassWvpMirror (run 17): twins.txt's "vs ps wvp" pairs (their VS reads eye 2's gWorldViewProj, rows 21-24)
// are listed only with the switch on at start, and twin_of hands their twins out only while this is on (prepass's mirror).
void set_wvp_live(bool on);
// [Stereo] PrepassInstPrevMirror (run 19 item 2): twins.txt's "instprev<K>" VS (eye 2's previous instance matrices at rows
// K + i, written only by prepass's mirror) are listed only with PrepassInstPrevMirror=1 and the same PrepassInstPrevK at
// start; twin_of hands their twins out only while this is on, and instprev_twin (the mutex: once per pipeline set, never
// per draw) tells prepass's playback which twins also need the draw's "mirrored" marker.
void set_instprev_live(bool on);
bool instprev_twin(ID3D12PipelineState* twin);
std::string twins_status();
uint64_t twins_made();
// [Stereo] MainInstanced (run 25 item 3, main_instanced.h; test only): the billboard family's game pipelines (VS
// ad643733f27ba4bd, PS 31e194eccc611691), listed at their creation when the switch is 1 at start. main_listed: lock-free,
// with the pipeline's flags (main_inst_flags). main_twin_of: a listed pipeline's twin (the ordinary twin rules made it),
// never a wvp or instprev one, and an eye2img or prevworld one only under item 6c's latch (below; off by default);
// nullptr otherwise (takes the twins' mutex: per listed draw only). With the switch on, twin_of never hands out a listed
// pipeline's twin (the pre-pass region never draws the main step's twins). main_status: the set's counts.
namespace main_inst_flags {
constexpr uint32_t kDepthOn = 1, kDepthWrite = 2, kStencilWrite = 4;  // bits 8-15 the DepthFunc, 16-19 NumRenderTargets
}
bool main_family_on();
// (GOAL-single-pass items 6d/6e, test only) [Stereo] MainFamily=bb|cw|bs|fb (read with MainInstanced at the first device's
// hooks; default bb): the one family whose pipelines are listed (research\run25\families.md 1.1). Its key ("bb" ...)
const char* main_family_key();
// (GOAL-single-pass item 8e, research\run25\item8e.md 2.1; test only) [Stereo] MainFamily=region: accepted only with [Stereo]
// MainRegion=1, MainUnionCull=1, MainPredication=1 and InstancedShadersAuto=1 (otherwise a warning and the billboards). The main set
// stays empty (main_listed never lists, main_twin_of never hands out, the withhold latch stays false: twin_of, twin_given_now and the
// walk cut's hold as in observe); main_inst lists from main_region's classifier and takes its twins from region_twin_of. Key "region"
bool main_family_region();
// (item 6e, test only) set_vb_note: IASetVertexBuffers notes slot 0 per thread as the census does (MainUnionCull's draw keys);
// last_vb: this thread's last one on cl (false when not noting or another list's)
void set_vb_note(bool on);
bool last_vb(const ID3D12GraphicsCommandList* cl, uint64_t* loc, uint32_t* size);
// (item 8d, test only) the same note's stride (main_region's dynamic-ring keys; 6e's keys do not read it)
bool last_vb_stride(const ID3D12GraphicsCommandList* cl, uint32_t* stride);
// (the review's should-fix 1) twin_of withholds the family's twins from the pre-pass only while main_inst::withholding();
// latched by prepass at each pass 1 pre-pass region's open, so a frame's two passes look twins up the same way
void latch_main_withhold();
bool main_withheld();  // the latched value
// serial (optional): the listing's serial (moves at each listing, so a pipeline re-made at an old address is another one)
bool main_listed(const void* game, uint32_t* flags, uint32_t* serial = nullptr);
// (GOAL-single-pass item 6c) cl: the list the twin will draw on (an eye2img twin needs SC2L and LB2 lent on it)
// idx: the listed draw's index in pass 1's region (item 6c: a prevworld hand-out is recorded by it for pass 2's would-drop)
ID3D12PipelineState* main_twin_of(ID3D12PipelineState* game, ID3D12GraphicsCommandList* cl = nullptr, int idx = -1);
std::string main_status();
// ---- (GOAL-single-pass item 6c; test only) the frame latch of the eye2img and prevworld twins. All on the playback thread
// (the thread that plays the stream and hands the twins out), in the frame's order:
//   prepass_open(slice): each pre-pass region's open (prepass_arrays clear_dsv): slice 0, pass 1's first, is the latch's
//     first stage (pass 1's start): the switch "prepass mi allowtwins" and the mirror's gate (prevworld::gate_live) latched,
//     and twin_of's eye2img answer for the pre-pass regions of BOTH passes: guard_valid() and fresh_this_frame() and the safe
//     point's live, read here. (Since 6b the pre-pass precedes this frame's check, so on the playback thread it is false: no
//     pre-pass region draws an eye2img twin; a pass 2 region never re-reads it, so it never drops a draw pass 1 did not twin.)
//   walk_open(pass): main_inst's region open (DVL call 1). Pass 1 without a pre-pass latch this frame: the first stage here.
//     The prevworld verdict reset (prevworld::pb_walk_reset).
//   walk_draw(cl, pass): every draw in the region, after the draw's list-call poll. Pass 1's first: the second stage, inside
//     pass 1's walk where freshness is known (6b): the eye2img decision = the first stage's switch and live, guard_valid() and
//     fresh_this_frame() now, and SC2L and LB2 lent on cl (eye2_light::lend); recorded for the frame. Every later draw in pass
//     1 and every draw in pass 2 uses the record. Lent again on a later list of the walk (counted).
//   walk_close(cl, pass): pass 1: SC2L and LB2 given back (on cl, else owed to the lent list); pass 2: the two passes' answers
//     compared (disagree, expected 0) with what the unlatched rule would have answered (live_disagree).
// main_twin_of hands an eye2img or prevworld twin to a listed pipeline only with the switch latched on: eye2img when the record
// says yes and the images are lent on cl; prevworld when the gate was live at the first stage and prevworld::draw_mirrored()
// for this draw. Off ("allowtwins off", the default): neither, as before (and walk_draw returns at once).
void prepass_open(int slice);
void walk_open(int pass);
void walk_draw(ID3D12GraphicsCommandList* cl, int pass);
void walk_close(ID3D12GraphicsCommandList* cl, int pass);
void set_allow_twins(bool on);  // "prepass mi allowtwins on|off" (the command thread; latched at the next pass 1's start)
bool allow_twins();
std::string latch_kv();
// (item 6c, the review's SF5; with the switch latched on only) the behavioural checks of the latch: prepass_draw, each draw of a
// pre-pass region (twin: drawn in pass 1, the reason of pass 2's drop; nullptr single); main_would_drop, pass 2's listed draw
// at its index by the rule a drop will use (counted); main_drop_compare, pass 1's twins against pass 2's would-drops (a good
// frame); allow_latched, the switch as this frame latched it
void prepass_draw(int slice, const void* game, ID3D12PipelineState* twin);
bool main_would_drop(ID3D12PipelineState* game, int idx);
void main_drop_compare(int twins1, int would2);
bool allow_latched();
// [Stereo] MainSlice (run 25 item 5, research\run25\slice.md 2.3; test only): with MainSlice=1 and MainInstanced=1 read at the
// first device's hooks, main_created also keeps each listed pipeline's desc, its root signature AddRef'd and never released
// (the mod's merge pipeline is made with that very root signature). main_desc: the kept desc of the listed pipeline at `game`
// with that serial; false when none is kept or another pipeline is listed there now (takes the mutex: once per pipeline).
struct MainDesc {
    ID3D12RootSignature* root = nullptr;
    D3D12_BLEND_DESC blend{};
    D3D12_RASTERIZER_DESC rast{};
    D3D12_DEPTH_STENCIL_DESC ds{};
    UINT sample_mask = 0;
    DXGI_SAMPLE_DESC sample{};
    UINT num_rt = 0;
    DXGI_FORMAT rtv[8]{};
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    UINT node_mask = 0;
    D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    bool stream_out = false;  // a stream-output desc (never the family's; refused)
    uint32_t serial = 0;
};
bool main_slice_kept();
// ([Stereo] MainRegionHandover, item 8g; test only) with it and MainInstanced read at the first device's hooks, each graphics pipeline's
// root signature (AddRef'd, never released: test only) kept at its creation (CreateGraphicsPipelineState); root_of: the one of the pipeline
// at `game`, or null (not kept, or made through a pipeline stream)
ID3D12RootSignature* root_of(const void* game);
// (the review's SF1) the root parameter of `root` (from its serialized desc at creation) whose descriptor table, visible to the pixel
// shader, holds an SRV range in register space 2 covering register `reg`; -1 none or not known
int bindless_param(const void* root, uint32_t reg);
bool main_desc(const void* game, uint32_t serial, MainDesc* out);
// A game pipeline's vertex and pixel shader hashes as recorded at its creation (0 when unknown); false: not recorded.
// For status lines only: it takes the census's mutex (never call it per draw).
bool describe(const void* game, uint64_t* vs, uint64_t* ps);
// The list's pipeline set without the census or the pre-pass's notes (the instanced pre-pass swaps twins in and out).
void set_raw(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps);
// (run 19, [Stereo] InstancedShadersAuto) around the mod's own pipeline creations (the menu, the rounds, the UI, the XR
// blit): while one is alive on a thread, the creation hook neither queues those shaders nor drains kept twins there
// (never on the XR or present path).
struct OwnCreation {
    OwnCreation();
    ~OwnCreation();
    OwnCreation(const OwnCreation&) = delete;
    OwnCreation& operator=(const OwnCreation&) = delete;
};
// f() under an OwnCreation (a call that may make pipelines inside it: the OpenXR runtime's session, swapchains, frames)
template <class F>
auto as_own(F&& f) {
    const OwnCreation own;
    return f();
}

// ---- [Stereo] MainRegion (GOAL-single-pass item 8d, research\run25\item8d.md 1.1; test only, off by default; read with
// MainInstanced=1 at the first device's hooks): the region table. Every graphics pipeline the game makes gets a slot (a static
// open-addressed table of 16,384 by pipeline address, 64 probes; full: not listed, counted, read as "unknown"), with its shaders'
// hashes, main_inst_flags' layout and its twin state, written only under the twins' mutex at the twin builder's own decision points
// (the creation, settle's refused and held branch, the held kinds at creation, the safe point's install with the twin and its kinds,
// a failed build, the folder's skips), a new creation number when an address is made again. Read lock-free (a seqlock). It is not
// the family set: twin_of's withhold and main_twin_of still consult only the one family (MainFamily), so nothing draws differently.
namespace region {
// the state (st, bits 0-7); bits 8-12 the kinds: for kRsMade the twin's, for kRsHeld the kind held
enum State : uint32_t {
    kRsUnknown = 0,     // a stream creation (no desc read)
    kRsQueued,          // a shader still being converted (the desc kept)
    kRsReadyNotBuilt,   // both shaders ready: to the builder, not installed yet
    kRsMade,            // the twin installed (twin set)
    kRsVsRefused,       // the converter refused the VS
    kRsPsRefused,       // the converter refused the PS
    kRsHeld,            // a ready shader whose kind's mirror was off at start (the kind in the high bits)
    kRsBuildFailed,     // the twin's creation failed (or the desc was dropped: the queues' caps, the builder not started)
    kRsShape,           // stream output, DS, HS or GS: never twinned
    kRsNoRule,          // no folder line and not by shader (or the mod's own pipeline, or no twins this session)
    kRsStates
};
constexpr uint32_t kKPlain = 1u << 8, kKPw = 1u << 9, kKE2i = 1u << 10, kKIp = 1u << 11, kKWvp = 1u << 12, kKindMask = 0x1f00;
// region_verdict's word: the verdict (bits 0-7) | the slot's st << 8 (so verdict >> 8 is the state and its kinds). (Item 8e)
// region_twin_of's own refusals after kVTwin: kVLatch the frame latch's allow off or not pass 1's walk; kVFormat the slot's formats,
// its sample count or NumRenderTargets not the side arrays' views'; kVIpOff an instprev twin while main_inst keeps them refused
// ("prepass mi region ip off")
enum Verdict : uint32_t { kVUnknownPso = 1, kVNoTwin, kVKindWvp, kVGateE2i, kVGatePw, kVGateIp, kVTwin, kVLatch, kVFormat, kVIpOff, kVerdicts };
// (item 8e) the packed formats of a pipeline's RTV 0-2 (8 bits each) and DSV (bits 24-30), and bit 31 kFmtSingle: SampleDesc Count 1
// and Quality 0 (the side arrays' own). 0: a colour format past 254 or a depth format past 126 (every DSV format is below): never equal
constexpr uint32_t kFmtSingle = 1u << 31;
constexpr uint32_t pack_formats(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t ds, bool single) {
    return r0 > 0xfe || r1 > 0xfe || r2 > 0xfe || ds > 0x7e ? 0u : (r0 | r1 << 8 | r2 << 16 | ds << 24 | (single ? kFmtSingle : 0u));
}
}  // namespace region
struct RegionInfo {
    uint64_t vs = 0, ps = 0;
    uint32_t flags = 0;   // main_inst_flags' layout (0 for a stream creation)
    uint32_t st = 0;      // region::State | kinds
    uint32_t gen = 0;     // the slot's creation number (moves when the address is made again)
    ID3D12PipelineState* twin = nullptr;
    uint64_t made_at = 0;  // kRsMade: state::presents at the safe point's install (0 for any other state)
    uint32_t fmt = 0;      // (item 8e) region::pack_formats of the desc's RTVFormats[0..2], DSVFormat and SampleDesc (0 for a stream creation)
};
bool region_on();  // the table is built this session
// lock-free: false when the pipeline has no slot (or its slot was being written through every retry)
bool region_lookup(const void* game, RegionInfo* out);
// ([Stereo] PrepassCutHoldPending, research\run25\item8d.md 11; test only, off by default; read at the first device's hooks, with
// InstancedShadersAuto=1) the same table written without MainRegion (region_on() and region_lookup stay MainRegion's only), for
// the walk cut's hold: twin_states_on, the table is written this session; twin_state, a pipeline's slot as region_lookup reads it
// (lock-free: the render thread's decision, once a frame for the pipelines whose singles moved; the status)
bool twin_states_on();
bool twin_state(const void* game, RegionInfo* out);
// (the PrepassCutHoldPending review's must-fix) would twin_of hand `game`'s installed twin (st: its slot's kRsMade | kinds, from
// twin_state) to the pre-pass now: false for a prevworld twin (twin_of never does) and an eye2img one (only under the frame latch's
// pre-pass answer, false in practice: taken as never), a wvp or instprev twin while its mirror is off (set_wvp_live,
// set_instprev_live), and a main-set pipeline while the main withhold is latched; false for any state but kRsMade. twin_of's
// rules read without its lock: no g_mutex, no counters, no thread-local (the render thread's decision, once per looked-up pipeline)
bool twin_given_now(const void* game, uint32_t st);
// The rule 8f's hand-out will use, a pure function (no counters, no g_pw_bits; the playback thread: the frame latch is its own): no
// slot -> kVUnknownPso; not kRsMade -> kVNoTwin; a wvp twin -> kVKindWvp (the main walk has no WVP mirror); an eye2img twin
// without the latch's yes and the images lent on cl -> kVGateE2i; a prevworld twin without the latch's pw and draw_mirrored() ->
// kVGatePw; an instprev twin without inst_prev::main_live() and draw_ok() -> kVGateIp; else kVTwin. out: the slot (may be null)
uint32_t region_verdict(const void* game, ID3D12GraphicsCommandList* cl, RegionInfo* out);
// (GOAL-single-pass item 8e, research\run25\item8e.md 2.2; test only, [Stereo] MainFamily=region) main_inst's hand-out, lock-free (the
// playback thread: g_lat is its own): region_verdict must be kVTwin; then the frame latch's allow and phase 2 (pass 1's walk) for every
// kind; then the slot's packed formats equal fmt (the side arrays' view formats, region::pack_formats; the single-sample bit set in both)
// and its NumRenderTargets 3; then an instprev twin only with ip_ok (main_inst's "prepass mi region ip on", the default); then the slot's
// twin. A twin read from a slot is never released (forget_twin leaks a handed-out twin; only never-installed stale twins are released,
// and they never reach a slot: install_built's creation-number check). nullptr: refused, *verdict its word (as region_verdict's)
ID3D12PipelineState* region_twin_of(const void* game, ID3D12GraphicsCommandList* cl, uint32_t fmt, bool ip_ok, uint32_t* verdict);
std::string region_status();
// (reporting only, never a verdict) research\run25\item8a.md 1's 40 pairs of segment e: the VS and PS hashes' top 32 bits, the
// kind the full flag set gives (kKPlain, kKPw, kKE2i, kKIp; kRefused the grass composite's PS) and where 8a puts it
struct E1Pair {
    uint32_t vs, ps;
    uint32_t kind;
    int prefix;  // 0 e01 (the entities, bucket 0's post), 1 e1 (bucket 1's post), 2 e only (ForestDraw mode 0), 3 before DVL call 1
    const char* name;
};
constexpr uint32_t kE1Refused = 1u << 16;
int e1_pairs(const E1Pair** out);                       // the table and its length
int e1_pair(uint64_t vs, uint64_t ps);                  // the row of this pair (by the hashes' top 32 bits), -1 not in 8a's census
// (8d's observe) the frame latch as the playback thread holds it, and the converter's kinds on at start
struct LatchView {
    int phase = 0, e2i = -1;
    int e2i_why = 0;  // the eye2img decision's reason: 0 yes, 1 no_kind, 2 no_live, 3 no_guard, 4 no_fresh, 5 no_lend, 6 undecided
    bool allow = false, pw = false;
};
void latch_view(LatchView* out);
void auto_kinds(bool* e2i, bool* pw);

// ---- [Holsters] InteractionHighlights=parts (run 29 item 2, research\run29\glow.md approach a; off by default): the gun's
// parts glowing on their own surface. body.cpp records, after the game's own draw of the held gun's geometry, a "glow on"
// marker (op 0x4a, its payload below), the same geometry drawn again with only the part's triangles (one 0x2b packet per
// index run, as [Body] HiddenGeometry=filter records them) and a "glow off" marker. At playback, while "glow on" is
// pending, each of those draws is drawn with the list's current pipeline's glow twin (the game's desc with our pixel
// shader, RT0 added to by the diffuse times the blend factor, every other target and the depth left alone) and the
// blend factor set to the payload's colour, then the game's pipeline and the blend factor (1, 1, 1, 1) are set back. A
// draw while pending that is not one of the payload's runs, or whose pipeline has no glow twin (a shadow's, the depth
// pre-pass's, a pipeline made before the switch), is dropped: nothing extra is ever drawn with the game's own pipeline.
// The twins are made for the weapon family's pipelines (PSWeapon, PSWeaponGold, by their pixel shaders' bytecode hashes)
// and for single pass's view-instanced twins of them ([Stereo] InstancedShaders' folder): at their creation while the
// mode is parts, else from a copy of the desc kept at creation, on a worker thread when the mode first becomes parts.
namespace glow {
struct Run {
    uint16_t start, count;  // in indices, within the game's own range
};
constexpr int kMaxRuns = 64;
constexpr uint32_t kMagic = 0x574f4c47;  // "GLOW"
struct Payload {  // the "glow on" marker's, copied into the stream (at most prepass::kMarkerPayloadMax bytes)
    uint32_t magic = kMagic;
    float rgb[3] = {};        // the blend factor: the colour (linear) times the strength now
    uint16_t nruns = 0;
    int8_t place = -1;        // holster::HighlightId (the status's)
    uint8_t eye = 0;          // recorded in an eye's pass (DrawVisEntity pass 1 or 3): only those count for live()
    Run runs[kMaxRuns] = {};
};
static_assert(sizeof(Payload) <= 512, "prepass::kMarkerPayloadMax");
inline uint32_t payload_size(uint16_t nruns) { return static_cast<uint32_t>(offsetof(Payload, runs) + sizeof(Run) * nruns); }
void on_cb(void* data);   // the "glow on" marker (the playback thread): data the payload's copy
void off_cb(void* data);  // the "glow off" marker
extern thread_local bool t_on;  // a "glow on" pending on this thread (the playback thread's; d3d_hooks' draws read it)
// d3d_hooks' indexed draw while t_on: one of the runs drawn with the glow twin (through `draw`, the original) or dropped,
// never with the game's pipeline (true: handled, the hook returns). False only when the pending glow expired at it (more
// draws that are none of its runs than the extra draw holds: its "off" taken as lost): the hook draws it as the game's
using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
bool draw_indexed(ID3D12GraphicsCommandList* cl, UINT count, UINT inst, UINT start, INT base, UINT first_inst, DrawIndexed_t draw);
enum Drop : int { kDropRegion, kDropNoRun, kDropNoPipeline, kDropNoTwin, kDropNonIndexed, kDropIndirect, kDrops };
void drop(int why);
// a draw while t_on that is none of the runs (DrawInstanced, ExecuteIndirect, another indexed draw): dropped (true), or the
// pending glow expired (false: the hook goes on with the game's draw). count: an indexed draw's index count (one equal to a
// pending run's is always dropped: it can only be the extra draw's)
bool foreign(int why, UINT count = 0);
// d3d_hooks' Reset(cl, ps): the list's current pipeline is ps from here (the glow's restore)
void list_reset(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps);
// the mode is parts (holster.cpp, at start and at each change): from now each weapon pipeline's twin is made at its creation,
// and the descs kept before are made on a one-shot worker thread (never on the game's or the presenting thread)
void want(bool on);
// a glow draw played within 250 ms: for that place (holster::HighlightId), or any (-1). zone_rings: a gun place's sprite
// is left out only while its glow is played
bool live(int place = -1);
int twins();  // glow twins in the table now
// "holster glow bias <n>" (test only, the session): the twins' depth bias (units of the depth buffer's resolution) toward
// the eye, for a pipeline that tests LESS or GREATER; every twin made again with it on the worker (the old ones leaked: the
// GPU may still use them). EQUAL (the gun's colour pass after the pre-pass) takes no bias
void set_bias(int units);
std::string status();
}  // namespace glow

}  // namespace rdrvr::pso
