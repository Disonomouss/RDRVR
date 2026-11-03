#include "core/pso.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/eye2_images.h"
#include "core/eye2_light.h"
#include "core/hooks.h"
#include "core/inst_prev_mirror.h"
#include "core/log.h"
#include "core/main_instanced.h"
#include "core/prepass_arrays.h"
#include "core/prevworld_mirror.h"
#include "core/ring_probe.h"
#include "core/state.h"
#include "core/vi_auto.h"

namespace rdrvr::pso {
namespace {

using CreateGraphics_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStream_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using SetPipelineState_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using SetVB_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
SetVB_t o_SetVB = nullptr;
thread_local ID3D12GraphicsCommandList* t_vb_cl = nullptr;
thread_local D3D12_VERTEX_BUFFER_VIEW t_vb{};
// Vertex capture ("pso vbcap <vs> <file>"): the next frame's first 16 draws of that vertex shader, 1024 vertices each.
std::atomic<uint64_t> g_cap_vs{0};
std::atomic<int> g_cap_state{0};  // 0 idle, 1 armed, 2 capturing, 3 done
std::string g_cap_file, g_cap_text;
int g_cap_draws = 0;
CreateGraphics_t o_CreateGraphics = nullptr;
CreateStream_t o_CreateStream = nullptr;
SetPipelineState_t o_SetPipelineState = nullptr;

std::mutex g_mutex;
std::unordered_map<void*, uint64_t> g_vs_of;                 // pipeline -> vertex shader hash
std::unordered_map<uint64_t, std::vector<uint8_t>> g_bytecode;  // vertex shader hash -> bytecode
std::unordered_map<uint64_t, uint64_t> g_draws, g_last;     // vertex shader hash -> draws this frame / last frame
std::unordered_set<uint64_t> g_skip_set;  // under g_mutex
std::atomic<bool> g_skip_any{false};
// The per-draw census (draws per vertex shader, the skips, the vertex capture) runs only after the first "pso" test
// command (2026-10-11, performance for weaker PCs: a mutex and a map update on every draw and pipeline set on the
// playback thread, about 12,000 a frame). [Debug] PsoCensus=1 runs it from the start.
std::atomic<bool> g_census{false};
std::atomic<uint64_t> g_skip{0}, g_skipped{0}, g_graphics{0}, g_stream{0}, g_unhashed{0};
thread_local ID3D12GraphicsCommandList* t_cl = nullptr;
thread_local const ID3D12GraphicsCommandList* t_raw_cl = nullptr;  // (r117 DG4's depthlog: the mod's set_raw pipeline, until the game's next)
thread_local const void* t_raw_ps = nullptr;
thread_local uint64_t t_vs = 0;
// The frame graph's shader census (single-pass stereo, the pre-pass census): per graphics pipeline its pixel shader's
// hash and what it writes (bits 0-7: render target i written, bit 8: depth written); the list's current one.
struct PsoInfo {
    uint64_t ps = 0;
    uint32_t writes = 0;
};
std::unordered_map<void*, PsoInfo> g_info;  // under g_mutex
thread_local PsoInfo t_info;

// ---- [Stereo] InstancedShaders (2026-10-14, test only): the view-instanced twins (pso.h twin_of)
std::unordered_map<uint64_t, std::vector<uint8_t>> g_vi_vs;  // vertex shader hash -> its rewrite (read-only after load)
// run 14 (P0): a pixel shader that reads a per-eye row (PSShadowBlendRDR2_Fadeless: gViewInverse's camera position),
// rewritten as the vertex shaders are: ps_<fnv>.dxil in the same folder, used in a listed pair's twin when present
std::unordered_map<uint64_t, std::vector<uint8_t>> g_vi_ps;
std::unordered_set<std::string> g_vi_pairs;                  // "vs ps" (16 hex each) allowed a twin (read-only after load)
std::unordered_map<void*, ID3D12PipelineState*> g_twin;      // the game's pipeline -> its twin, under g_mutex
std::atomic<uint64_t> g_twins_made{0}, g_twins_failed{0}, g_twins_skipped{0}, g_twins_by_shader{0};
// run 17 ([Stereo] PrepassWvpMirror, research\run17\sps.md (A)): a twins.txt line "vs ps wvp" is a pair whose VS reads
// gWorldViewProj (eye 2's rows 21-24, written only by prepass's mirror): listed only with the switch on at start, and
// its twins handed out by twin_of only while the mirror runs (set_wvp_live), so with it off the pair stays single.
std::unordered_set<std::string> g_vi_wvp_pairs;              // (read-only after load)
std::unordered_set<ID3D12PipelineState*> g_wvp_twins;        // those pairs' twins, under g_mutex
size_t g_vi_wvp_held = 0;                                    // "wvp" lines not listed (the switch off at start)
// run 18: per-shader lines, the converter's rule on a player's PC rather than a list of pairs seen in a few views:
// "vs <hash> [wvp]" a vertex shader classed ready (it reads only the per-view rows eye 2 has; wvp: gWorldViewProj, the
// mirror's), "ps <hash>" a pixel shader classed ready (no per-eye reads); a pipeline whose VS and PS (or no PS) are both
// ready gets a twin whatever its pair. Read-only after load.
std::unordered_set<uint64_t> g_vi_vs_ok, g_vi_vs_ok_wvp, g_vi_ps_ok;
size_t g_vi_vs_ok_held = 0;                                  // "vs <hash> wvp" lines held (the mirror off at start)
// the held ones by hash (a VS) and by key (a "vs ps wvp" pair), so no other line (a plain "vs" line for the same hash, a
// pair without "wvp") gives them a twin drawn while nobody writes eye 2's gWorldViewProj (the review). Read-only after load.
std::unordered_set<uint64_t> g_vi_vs_held;
std::unordered_set<std::string> g_vi_pairs_held;
std::atomic<bool> g_wvp_live{false};
// run 19 item 2 ([Stereo] PrepassInstPrevMirror, research\run19\instprev.md section 4): a twins.txt line "vs <hash> ...
// instprev<K>" (or a pair "vs ps ... instprev<K>") is a VS made with the converter's --instprev K: its twin reads eye 2's
// previous instance matrices at rows K + i, which only the mirror writes. Listed only with PrepassInstPrevMirror=1 at
// start and its K = PrepassInstPrevK, else held like a wvp line; their twins are handed out by twin_of only while the
// mirror runs (set_instprev_live), and per draw only when prepass's playback says the draw was mirrored (instprev_twin).
std::unordered_set<uint64_t> g_vi_vs_ok_ip;                  // (read-only after load)
std::unordered_set<std::string> g_vi_ip_pairs;               // (read-only after load)
size_t g_vi_ip_held = 0;                                     // instprev lines held (the mirror off or another K at start)
std::unordered_set<ID3D12PipelineState*> g_instprev_twins;   // under g_mutex
std::atomic<size_t> g_instprev_n{0};                         // its size (read without the mutex)
std::atomic<bool> g_instprev_live{false};
std::atomic<int64_t> g_twin_ms_max{0}, g_twin_ms_total{0};
// ---- run 19 item 4, [Stereo] InstancedShadersAuto (test only; vi_auto.h): the twins from the converter's cache on this
// PC, by shader (the per-shader rule's: a ready VS with a ready PS or none), instead of the InstancedShaders folder.
// A pipeline whose shaders are still being converted is kept (a copy of its desc: the input layout and its names, the
// pixel shader's bytecode, a reference on the root signature) and is looked at again when results land (drain: the game's
// next pipeline creation call, and (run 20) each frame's end).
// Run 20 item 3: no twin is built on a game thread. A pipeline whose shaders are ready (at its creation, or later from
// a kept desc) is copied the same way (plus a reference on its device) and handed to the builder thread, which makes the
// twin (a D3D12 device is free-threaded) and leaves it in g_built; the safe point installs it (install_built: each
// frame's end, on the present thread, under g_pend_mutex and g_mutex with the creation-number check). Set at the device's
// creation, read-only after.
std::atomic<bool> g_auto{false}, g_auto_wvp{false};
std::atomic<bool> g_auto_ip{false};  // (run 20) the converter given --instprev K (vi_auto::instprev_k): instprev twins allowed
// (run 25, [Stereo] PrepassEye2Shaders; GOAL-single-pass item 2) the converter given --eye2-images (vi_auto::eye2_shaders):
// eye2img twins allowed. Those twins (g_e2i_twins, under g_mutex) are their own kind, never wvp: twin_of hands one out only
// while eye2_images::guard_valid() AND at the last frame's end (g_e2i_live, set at the safe point). (Item 6c) Both, and
// fresh_this_frame(), latched at pass 1's first pre-pass region for both passes (g_lat_pp); main_twin_of by the frame latch.
std::atomic<bool> g_auto_e2i{false};
std::unordered_set<ID3D12PipelineState*> g_e2i_twins;  // under g_mutex
std::atomic<size_t> g_e2i_n{0};                         // its size (read without the mutex)
std::atomic<bool> g_e2i_live{false};
std::atomic<uint64_t> g_auto_held_e2i{0}, g_e2i_given{0}, g_e2i_held_draws{0}, g_e2i_frames_live{0}, g_e2i_frames_held{0}, g_e2i_releases{0},
    g_e2i_restores{0};
// (run 26, [Stereo] PrepassPrevWorldTwins; GOAL-single-pass item 6a) the converter given --prevworld (vi_auto::prevworld_shaders):
// prevworld twins allowed (VS twins reading eye 2's gPrevWorldViewProjSingle at rage_prev_world rows 5-8, the PrevWorld
// mirror's). Their own kind, never wvp (g_pw_twins, under g_mutex): twin_of hands one out only while the mirror wrote eye 2's
// rows with no fallback in the last frame's pass 1 (g_pw_live, set at the safe point from prevworld::rows_written; one answer
// for both passes of a frame; 6a: never true). twin_of never hands one out (refused, counted); since item 6c main_twin_of may,
// under the frame latch with "allowtwins" on, per draw by prevworld's gate (draw_mirrored).
std::atomic<bool> g_auto_pw{false};
std::unordered_set<ID3D12PipelineState*> g_pw_twins;  // under g_mutex
std::atomic<size_t> g_pw_n{0};                        // its size (read without the mutex)
std::atomic<bool> g_pw_live{false};
std::atomic<bool> g_pw_written{false};  // (the status's "rows written in the last frame": the safe point's rows_written, as latched)
std::atomic<uint64_t> g_auto_held_pw{0}, g_pw_given{0}, g_pw_held_draws{0}, g_pw_frames_live{0}, g_pw_frames_held{0}, g_pw_releases{0},
    g_pw_restores{0}, g_pw_main_refused{0};
// ---- (GOAL-single-pass item 6c; test only) the frame latch of the eye2img and prevworld twins (pso.h prepass_open .. walk_close).
// The playback thread's (g_lat), except the switch and the counters. ans / shadow: per pass (1, 2), the answers given (the
// latched ones) and the unlatched rule's (the guard, freshness and the gate read live at each consult), as bits; compared at
// pass 2's walk close
std::atomic<bool> g_allow_want{false};  // "prepass mi allowtwins on|off" (off by default)
enum : uint32_t { kAnsE2iY = 1, kAnsE2iN = 2, kAnsPwY = 4, kAnsPwN = 8, kAnsPpY = 16, kAnsPpN = 32 };
struct Latch {
    int phase = 0;      // 0 none; 1 the first stage at pass 1's first pre-pass region; 2 pass 1's walk open; 3 pass 2's walk open
    bool allow = false;  // the switch
    bool live = false;   // the safe point's g_e2i_live
    bool pp = false;     // twin_of's eye2img answer for both passes' pre-pass regions
    bool pw = false;     // prevworld twins may be handed out this frame (per draw: prevworld::draw_mirrored())
    int e2i = -1;        // main_twin_of's eye2img answer (-1 not decided yet: at pass 1's walk's first draw)
    int e2i_why = 6;     // (item 8d's observe) its reason: 0 yes, 1 no_kind, 2 no_live, 3 no_guard, 4 no_fresh, 5 no_lend, 6 undecided
    uint32_t ans[3] = {}, shadow[3] = {};
};
Latch g_lat;
std::atomic<bool> g_lat_pp{false};  // (twin_of's read of g_lat.pp; atomic for the status)
using LCount = std::atomic<uint64_t>;
LCount g_l_first, g_l_at_walk, g_l_order, g_l_dec_yes, g_l_dec_no, g_l_no_kind, g_l_no_live, g_l_no_guard, g_l_no_fresh, g_l_no_lend, g_l_undecided,
    g_l_relend, g_l_relend_failed, g_l_cmp, g_l_disagree, g_l_live_disagree, g_l_flip, g_l_live_flip, g_l_pw_frames, g_l_pw_sel_m, g_l_pw_sel_u,
    g_l_e2i_given, g_l_e2i_held, g_l_pw_given, g_l_pw_held, g_l_gb_close, g_l_gb_owed, g_l_pp_yes, g_l_pp_no, g_l_draws1, g_l_draws2;
// (the review's SF5) the behavioural checks, the playback thread's, only with the switch latched on. The pre-pass: per pass the
// region's draws, its twinned draws (pass 1 drawn with a twin, pass 2 dropped for one) by count and a sum of the pipelines'
// mixed addresses, and the eye2img ones; compared at pass 2's walk close when pass 2's region drew (not under the walk cut).
// The main walk: pass 1's prevworld hand-outs per listed draw (g_pw_bits), for pass 2's would-drop; pass 1's twins against
// pass 2's would-drops on good frames (main_drop_compare)
struct PpAcc {
    uint64_t draws = 0, twinned = 0, sum = 0, e2i = 0;
};
PpAcc g_pp[2];
constexpr int kPwBits = 4096;
uint64_t g_pw_bits[kPwBits / 64];
thread_local ID3D12PipelineState* t_tw_last = nullptr;  // twin_of's last answer on this thread, and whether it is an eye2img twin
thread_local bool t_tw_e2i = false;
LCount g_pp_cmp, g_pp_skipped, g_pp_differ, g_pp_differ_e2i, g_pp_e2i1, g_md_cmp, g_md_differ, g_md_would;
// ---- run 25 item 3, [Stereo] MainInstanced (main_instanced.h; test only): the billboard family's game pipelines (VS
// ad643733f27ba4bd, PS 31e194eccc611691: research\run24\billboard.md section 3.1), noted at their creation with the switch on
// at start. Their twins (made by the ordinary rules: the folder's or the converter's) are handed to the main step only
// (main_twin_of); twin_of never hands them to the pre-pass. A small lock-free set: readers scan it per pipeline set
// while a main region is open; writers (the creation hooks) hold g_mutex.
constexpr uint64_t kMainVs = 0xad643733f27ba4bdull, kMainPs = 0x31e194eccc611691ull;
// (GOAL-single-pass items 6d/6e, test only) [Stereo] MainFamily=bb|cw|bs|fb, read with g_main_want: the one family listed this
// session (research\run25\families.md 1.1; bb, the billboards, the default and item 3's). Read-only after the first device's hooks
struct MainFam {
    const char* key;
    const char* name;
    uint64_t vs, ps;
};
constexpr MainFam kMainFams[] = {
    {"bb", "billboard", kMainVs, kMainPs},
    {"cw", "CliffWall", 0xaa512b24697e7cc3ull, 0xb1ab4ea75f227d11ull},
    {"bs", "BumpSpecAOShared", 0xf882a9134b579dc8ull, 0xcf6fee87e1999741ull},
    {"fb", "FlattenBlend", 0xb6691b1ad775e5e5ull, 0x3d78951957bf5135ull},
};
int g_main_fam = 0;  // (an index into kMainFams)
// (GOAL-single-pass item 8e, test only) [Stereo] MainFamily=region (read with g_main_want, its prerequisites checked there; read-only
// after): nothing enters the main set; main_inst lists from main_region's classifier and hands out by region_twin_of
bool g_main_region = false;
constexpr int kMainSlots = 64;  // (items 6d/6e: a family may have more pipelines than the billboards' few; the scan is per listed-region draw)
// (item 6e, test only) the vertex buffer noted per list for MainUnionCull's draw keys (as the census does): set at its install
std::atomic<bool> g_vb_note{false};
bool g_main_want = false;  // ([Stereo] MainInstanced=1, read at the first device's hooks; read-only after): the set collected
// (from the first pipeline: the family's may be made before main_inst::install). It is consulted only by the main step, and
// twin_of withholds the family's twins from the pre-pass only while g_main_withhold, latched at each pass 1 pre-pass region's
// open (latch_main_withhold) from main_inst::withholding(): installed, on and not stood down. Refused or off at run time,
// the family's twins are the pre-pass's again, from the next frame's pass 1 (never between a frame's two passes).
std::atomic<bool> g_main_withhold{false};
std::atomic<void*> g_main_pso[kMainSlots];
std::atomic<uint32_t> g_main_flags[kMainSlots];
std::atomic<uint64_t> g_main_made{0}, g_main_full{0}, g_main_gone{0};
// (run 25 item 5, [Stereo] MainSlice; test only) the listed pipelines' descs (under g_mutex) and their listings' serials
// (lock-free, stored before the slot's pipeline): kept only with g_main_slice_want (read with g_main_want; read-only after)
bool g_main_slice_want = false;
bool g_ho_root_want = false;  // ([Stereo] MainRegionHandover, item 8g: every graphics pipeline's root signature kept, read-only after)
std::unordered_map<const void*, ID3D12RootSignature*> g_ho_roots;  // (under g_mutex)
std::atomic<uint32_t> g_main_serial[kMainSlots];
MainDesc g_main_desc[kMainSlots];
uint32_t g_main_serial_next = 0;  // (under g_mutex)
// ---- [Stereo] MainRegion (GOAL-single-pass item 8d, research\run25\item8d.md 1.1; test only): the region table (pso.h region).
// Written only under g_mutex (the creation hooks, settle's branch, the creation-time decisions, the safe point's install, the
// builder's failure, the folder's skips), read lock-free: a slot's pipeline is stored last (release) when the slot is first taken;
// every write of a taken slot runs between an odd and the next even serial, and a reader keeps the fields only when it read the
// same even serial before and after them. A slot is never freed: an address made again gets its slot back with a new creation
// number (gen), as g_main_gone's re-made entries. Nothing here draws or decides anything: 8d's observe reads it
bool g_region_want = false;  // ([Stereo] MainRegion=1 with MainInstanced=1, read at the first device's hooks; read-only after)
// (item8d.md 11, test only) the table written: MainRegion, or [Stereo] PrepassCutHoldPending=1 with InstancedShadersAuto and the
// ini switches prepass's walk cut arms by (prepass::cut_hold_pending_wanted; the walk cut's hold reads each single's twin state
// through twin_state; region_on() and region_lookup stay MainRegion's). Read-only after
bool g_region_write = false;
constexpr unsigned kRegionSlots = 16384, kRegionProbe = 64;
struct RegionSlot {
    std::atomic<void*> pso{nullptr};
    std::atomic<uint32_t> serial{0};  // odd while written
    std::atomic<uint32_t> gen{0};
    std::atomic<uint64_t> vs{0}, ps{0};
    std::atomic<uint32_t> flags{0}, st{0};
    std::atomic<ID3D12PipelineState*> twin{nullptr};
    std::atomic<uint64_t> made_at{0};  // (PrepassCutHoldPending) kRsMade: state::presents at the install (0 otherwise)
    std::atomic<uint32_t> fmt{0};      // (item 8e) region::pack_formats of the desc's RTV 0-2 and DSV, written at the creation
};
RegionSlot g_region[kRegionSlots];  // (static: about 1 MB of zeroes, 64 bytes a slot, never allocated)
uint32_t g_region_gen = 0;           // (under g_mutex)
std::atomic<uint64_t> g_rg_listed{0}, g_rg_remade{0}, g_rg_full{0}, g_rg_writes{0}, g_rg_lost{0}, g_rg_busy{0};
std::atomic<uint64_t> g_rg_states[region::kRsStates];  // (each write's new state: the status's histogram of decisions)
thread_local int t_own = 0;  // pso::OwnCreation: the mod's own creations (no queueing, no drain)
thread_local bool t_builder = false;  // (run 20) the builder thread
struct Pending {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};  // its pointers into the members below (VS empty: the twin's at the build)
    std::vector<D3D12_INPUT_ELEMENT_DESC> il;
    std::vector<std::string> names;
    vi_auto::Bytes ps_code;  // (one copy a pixel shader: g_ps_copies)
    ID3D12Device2* dev = nullptr;  // (run 20) the device the game made it on, for the builder (a reference)
    uint64_t vs = 0, ps = 0, seq = 0;
    bool vs_ready = false, ps_ready = false, listed = false;  // listed: in g_ready
    Pending() = default;
    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;
    // (run 20) the references attach took (d.pRootSignature is null until then): released with the desc, wherever it goes
    ~Pending() {
        if (d.pRootSignature) d.pRootSignature->Release();
        if (dev) dev->Release();
    }
};
// lock order: g_pend_mutex, then g_mutex (and vi_auto's own lock inside either)
std::mutex g_pend_mutex;
// the game's pipeline -> its kept desc, under g_pend_mutex. (run 20, the review) Leaked on purpose, as g_build_q: ~Pending
// releases D3D objects, which must never run in the static destructors at the DLL's detach
auto& g_pending = *new std::unordered_map<void*, std::unique_ptr<Pending>>;
// each game pipeline address's creation number, under g_pend_mutex: a twin made later is handed to the pipeline it was
// made for only if no other pipeline has been made at that address since (the old one released, the address reused)
std::unordered_map<void*, uint64_t> g_seq;
uint64_t g_seq_next = 0;
// the kept descs by the shader they wait for (address, creation number; stale entries skipped), the kept descs whose
// shaders are both ready (to be made at the drain), vi_auto's results read so far, the pixel shaders' copies: all under
// g_pend_mutex
using Waiter = std::pair<void*, uint64_t>;
std::unordered_map<uint64_t, std::vector<Waiter>> g_wait_vs, g_wait_ps;
std::deque<Waiter> g_ready;
size_t g_results_seen = 0;
std::unordered_map<uint64_t, std::weak_ptr<const std::vector<uint8_t>>> g_ps_copies;
constexpr size_t kPendingMax = 16384;
// the drain's moves of ready kept descs to the builder at one call: until 0.5 ms are spent (one at least). (run 20: no
// device work in it any more; run 19's 2 ms was for builds. With the installs' 0.5 ms a frame end stays near 1 ms)
constexpr double kDrainBudgetMs = 0.5;
std::atomic<uint64_t> g_auto_now{0}, g_auto_later{0}, g_auto_kept{0}, g_auto_dropped{0}, g_auto_stale{0}, g_auto_over{0}, g_auto_held{0},
    g_auto_refused{0}, g_auto_own{0};
std::atomic<uint64_t> g_drain_us{0}, g_drain_us_max{0}, g_drains{0};  // the drain's time on the game's threads
std::atomic<uint64_t> g_drained_gen{~0ull};
std::atomic<bool> g_draining{false};
// g_ready not empty (set under g_pend_mutex with it): the drain's early-out never strands a desc settled ready outside it
std::atomic<bool> g_ready_any{false};
// ---- run 20 item 3: the builder thread. Lock order: g_pend_mutex, then g_mutex; g_build_mutex is taken alone.
struct Build {  // a desc to build: the desc owned (its copies live until the creation returns), the VS twin's container
    std::unique_ptr<Pending> p;
    void* game = nullptr;
    vi_auto::Bytes twin;
    vi_auto::Bytes ps_twin;  // (run 20) the rewritten PS's container, or empty: the game's PS (the desc's copy)
    bool wvp = false, ip = false, at_creation = false;
    bool e2i = false;  // (run 25) the PS twin reads eye 2's image indices
    bool pw = false;   // (run 26) the VS twin reads eye 2's rage_prev_world rows 5-8
};
struct Built {  // a finished twin waiting for the safe point
    void* game = nullptr;
    uint64_t seq = 0;
    ID3D12PipelineState* twin = nullptr;
    uint64_t vs = 0, ps = 0;
    bool wvp = false, ip = false, at_creation = false;
    bool e2i = false;  // (run 25)
    bool pw = false;   // (run 26)
};
std::mutex g_build_mutex;
std::condition_variable g_build_cv;
auto& g_build_q = *new std::deque<Build>;  // under g_build_mutex (leaked: see g_pending)
auto& g_built = *new std::deque<Built>;    // under g_build_mutex (leaked: twins are never released at exit)
std::atomic<bool> g_built_any{false}, g_builder_on{false};
constexpr double kInstallBudgetMs = 0.5;  // the safe point's installs at one frame's end: until 0.5 ms are spent (one at least)
std::atomic<uint64_t> g_build_queued{0}, g_builds{0}, g_build_failed{0}, g_build_us{0}, g_build_us_max{0}, g_built_on_game{0}, g_auto_held_ip{0};
std::atomic<uint64_t> g_call_us{0}, g_call_us_max{0}, g_calls{0};       // the mod's time in the game's creation calls
std::atomic<uint64_t> g_fe_us{0}, g_fe_us_max{0}, g_fe_calls{0};        // the frame end's drain and installs
std::atomic<uint64_t> g_install_us{0}, g_install_us_max{0}, g_installs{0};
bool twins_on() { return g_auto || !g_vi_vs.empty(); }
// run 12 item 12a, "pso ps reset|dump <file>" (single-pass stereo's reader census, a test's): every pixel shader drawn
// since the reset, by its hash (fnv of its bytecode, as the corpus's blobs hash offline), with its draws, a vertex
// shader it drew with and what it writes; per scene, joined offline to the corpus's screen-target samplers
struct PsAcc {
    uint64_t draws = 0, vs = 0;
    uint32_t writes = 0;
};
std::unordered_map<uint64_t, PsAcc> g_ps_acc;  // under g_mutex
std::atomic<bool> g_ps_acc_on{false};

uint64_t fnv(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// ---- [Stereo] MainRegion (item 8d) the region table's writers (under g_mutex) and its probe
unsigned region_home(const void* p) {
    uint64_t h = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)) >> 4;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return static_cast<unsigned>(h) & (kRegionSlots - 1);
}
// (under g_mutex) the slot of `game`, or (take) the first free one on its probe; nullptr: not listed (or the probe full)
RegionSlot* region_slot(const void* game, bool take) {
    unsigned i = region_home(game);
    for (unsigned p = 0; p < kRegionProbe; ++p, ++i) {
        RegionSlot& s = g_region[i & (kRegionSlots - 1)];
        const void* at = s.pso.load(std::memory_order_relaxed);
        if (at == game) return &s;
        if (!at) return take ? &s : nullptr;
    }
    return nullptr;
}
// (under g_mutex) a slot's fields between an odd and the next even serial (the readers' retry)
template <class F>
void region_write(RegionSlot& s, F&& f) {
    const uint32_t q = s.serial.load(std::memory_order_relaxed);
    s.serial.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    f();
    s.serial.store(q + 2, std::memory_order_release);
}
// (under g_mutex) a listed pipeline's twin state (and, for kRsMade, its twin); no slot: counted (lost)
void region_put_locked(const void* game, uint32_t st, ID3D12PipelineState* twin = nullptr) {
    if (!g_region_write || !game) return;
    RegionSlot* s = region_slot(game, false);
    if (!s) {
        g_rg_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t made_at = (st & 0xff) == region::kRsMade ? state::presents.load(std::memory_order_relaxed) : 0;
    region_write(*s, [&] {
        s->st.store(st, std::memory_order_relaxed);
        s->twin.store(twin, std::memory_order_relaxed);
        s->made_at.store(made_at, std::memory_order_relaxed);
    });
    g_rg_writes.fetch_add(1, std::memory_order_relaxed);
    g_rg_states[(st & 0xff) < region::kRsStates ? (st & 0xff) : region::kRsUnknown].fetch_add(1, std::memory_order_relaxed);
}
void region_put(const void* game, uint32_t st) {  // (no lock held: the game's creation thread)
    if (!g_region_write) return;
    std::lock_guard lock(g_mutex);
    region_put_locked(game, st);
}
// (no lock held) a later decision for the pipeline whose creation number was seq: written only if `game` still has it (lock order:
// g_pend_mutex, then g_mutex)
void region_put_seq(const void* game, uint64_t seq, uint32_t st) {
    if (!g_region_write) return;
    std::lock_guard pl(g_pend_mutex);
    auto it = g_seq.find(const_cast<void*>(game));
    if (it == g_seq.end() || it->second != seq) return;
    std::lock_guard lock(g_mutex);
    region_put_locked(game, st);
}
// a held shader's kind as the region table's kind bits
uint32_t held_kind(bool wvp, bool ip, bool e2i, bool pw) {
    return (wvp ? region::kKWvp : 0u) | (ip ? region::kKIp : 0u) | (e2i ? region::kKE2i : 0u) | (pw ? region::kKPw : 0u);
}
uint32_t made_kinds(bool wvp, bool ip, bool e2i, bool pw) {
    const uint32_t k = held_kind(wvp, ip, e2i, pw);
    return k ? k : region::kKPlain;
}
// next to every main_created: a game pipeline made at `pso` (d null: a stream creation, its desc not read) gets its slot, fresh
void region_created(void* pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    if (!g_region_write || !pso) return;
    const uint64_t vs = d && d->VS.pShaderBytecode && d->VS.BytecodeLength ? fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength) : 0;
    const uint64_t ps = d && d->PS.pShaderBytecode && d->PS.BytecodeLength ? fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength) : 0;
    uint32_t flags = 0;
    if (d) {  // (main_created's layout)
        const D3D12_DEPTH_STENCIL_DESC& ds = d->DepthStencilState;
        flags = (ds.DepthEnable ? main_inst_flags::kDepthOn : 0u) |
                (ds.DepthEnable && ds.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? main_inst_flags::kDepthWrite : 0u) |
                (ds.StencilEnable && ds.StencilWriteMask ? main_inst_flags::kStencilWrite : 0u) |
                (static_cast<uint32_t>(ds.DepthFunc & 0xff) << 8) | (static_cast<uint32_t>(d->NumRenderTargets & 0xf) << 16);
    }
    // (item 8e) the formats and the sample count region_twin_of checks against the side arrays' views (a stream creation: 0, never equal)
    const uint32_t fmt = d ? region::pack_formats(static_cast<uint32_t>(d->RTVFormats[0]), static_cast<uint32_t>(d->RTVFormats[1]),
                                                  static_cast<uint32_t>(d->RTVFormats[2]), static_cast<uint32_t>(d->DSVFormat),
                                                  d->SampleDesc.Count == 1 && d->SampleDesc.Quality == 0)
                           : 0u;
    std::lock_guard lock(g_mutex);
    RegionSlot* s = region_slot(pso, true);
    if (!s) {
        g_rg_full.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool fresh = s->pso.load(std::memory_order_relaxed) == nullptr;
    const uint32_t gen = ++g_region_gen;
    region_write(*s, [&] {
        s->gen.store(gen, std::memory_order_relaxed);
        s->vs.store(vs, std::memory_order_relaxed);
        s->ps.store(ps, std::memory_order_relaxed);
        s->flags.store(flags, std::memory_order_relaxed);
        // (the twin builder's decision points refine it: no twins this session, the mod's own or no desc stay as made here)
        s->st.store(d ? region::kRsNoRule : region::kRsUnknown, std::memory_order_relaxed);
        s->twin.store(nullptr, std::memory_order_relaxed);
        s->made_at.store(0, std::memory_order_relaxed);
        s->fmt.store(fmt, std::memory_order_relaxed);
    });
    if (fresh) s->pso.store(pso, std::memory_order_release);  // (last: a reader finds the slot only whole)
    (fresh ? g_rg_listed : g_rg_remade).fetch_add(1, std::memory_order_relaxed);
}

void record(void* pso, const D3D12_SHADER_BYTECODE& vs) {
    if (!pso) return;
    uint64_t h = vs.pShaderBytecode && vs.BytecodeLength ? fnv(vs.pShaderBytecode, vs.BytecodeLength) : 0;
    std::lock_guard lock(g_mutex);
    g_vs_of[pso] = h;
    if (h && !g_bytecode.count(h)) {
        const uint8_t* b = static_cast<const uint8_t*>(vs.pShaderBytecode);
        g_bytecode.emplace(h, std::vector<uint8_t>(b, b + vs.BytecodeLength));
    }
}

// [Debug] ShaderSwap=<folder> (single-pass stereo step 4, test only): at the start, every <16 hex digits>.dxil file there
// is a replacement for the vertex shader of that hash (fnv of its bytecode, as "pso dump" names them); a pipeline made
// with that vertex shader is made with the replacement instead, or with the game's own if that fails. Read-only map
// after load_swaps().
std::unordered_map<uint64_t, std::vector<uint8_t>> g_swap;
std::atomic<uint64_t> g_swaps{0}, g_swap_fails{0};
void load_swaps() {
    const std::string dir = config::get_string("Debug", "ShaderSwap", "");
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*.dxil").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log::warn("[pso] ShaderSwap: no .dxil files in %s", dir.c_str());
        return;
    }
    do {
        const std::string name = fd.cFileName;
        if (name.size() != 16 + 5) continue;
        const uint64_t hash = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        FILE* f = nullptr;
        if (!hash || fopen_s(&f, (dir + "\\" + name).c_str(), "rb") != 0 || !f) continue;
        std::vector<uint8_t> b(fd.nFileSizeLow);
        const size_t got = fread(b.data(), 1, b.size(), f);
        fclose(f);
        if (got == b.size() && b.size() > 32 && std::memcmp(b.data(), "DXBC", 4) == 0) g_swap.emplace(hash, std::move(b));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    log::info("[pso] ShaderSwap: %zu replacement vertex shaders from %s", g_swap.size(), dir.c_str());
}
const std::vector<uint8_t>* swap_for(const D3D12_SHADER_BYTECODE& vs) {
    if (g_swap.empty() || !vs.pShaderBytecode || !vs.BytecodeLength) return nullptr;
    auto it = g_swap.find(fnv(vs.pShaderBytecode, vs.BytecodeLength));
    return it == g_swap.end() ? nullptr : &it->second;
}

void load_twin_sources() {
    const std::string dir = config::get_string("Stereo", "InstancedShaders", "");
    if (dir.empty()) return;
    if (g_auto) {  // (run 19: one source of twins at a time)
        log::warn("[pso] InstancedShaders=%s ignored: [Stereo] InstancedShadersAuto=1 makes the twins from the converter's cache", dir.c_str());
        return;
    }
    for (int stage = 0; stage < 2; ++stage) {  // vs_<16 hex>.dxil, then ps_<16 hex>.dxil (run 14)
        auto& into = stage ? g_vi_ps : g_vi_vs;
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((dir + (stage ? "\\ps_*.dxil" : "\\vs_*.dxil")).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string name = fd.cFileName;
            if (name.size() != 3 + 16 + 5) continue;
            const uint64_t hash = std::strtoull(name.substr(3, 16).c_str(), nullptr, 16);
            FILE* f = nullptr;
            if (!hash || fopen_s(&f, (dir + "\\" + name).c_str(), "rb") != 0 || !f) continue;
            std::vector<uint8_t> b(fd.nFileSizeLow);
            const size_t got = fread(b.data(), 1, b.size(), f);
            fclose(f);
            if (got == b.size() && b.size() > 32 && std::memcmp(b.data(), "DXBC", 4) == 0) into.emplace(hash, std::move(b));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "\\twins.txt").c_str(), "r") == 0 && f) {
        const bool wvp_wanted = config::get_bool("Stereo", "PrepassWvpMirror", false);
        // (run 19 item 2) instprev<K> lines: only with the mirror on at start (=1, not the read-only modes) and the same K
        const int ip_mode = config::get_int("Stereo", "PrepassInstPrevMirror", 0), ip_k = config::get_int("Stereo", "PrepassInstPrevK", 512);
        char line[256], a[64] = {}, b[64] = {}, c[16] = {}, e[16] = {};
        while (std::fgets(line, sizeof(line), f)) {  // "vs ps [wvp] [instprev<K>]" per line; '#' lines and anything else skipped
            c[0] = 0;
            e[0] = 0;
            const int n = line[0] == '#' ? 0
                                         : sscanf_s(line, "%63s %63s %15s %15s", a, static_cast<unsigned>(sizeof(a)), b, static_cast<unsigned>(sizeof(b)), c,
                                                    static_cast<unsigned>(sizeof(c)), e, static_cast<unsigned>(sizeof(e)));
            const bool tag_wvp = (n >= 3 && std::strcmp(c, "wvp") == 0) || (n >= 4 && std::strcmp(e, "wvp") == 0);
            const char* ipt = n >= 3 && std::strncmp(c, "instprev", 8) == 0 ? c : n >= 4 && std::strncmp(e, "instprev", 8) == 0 ? e : nullptr;
            const bool tag_ip = ipt != nullptr;
            // (an instprev tag without its K, or with another K than the ini's: held)
            const bool ip_ok = !tag_ip || (ip_mode == 1 && ipt[8] >= '0' && ipt[8] <= '9' && std::atoi(ipt + 8) == ip_k);
            if (n >= 2 && (std::strcmp(a, "vs") == 0 || std::strcmp(a, "ps") == 0) && std::strlen(b) == 16) {  // (run 18) per shader
                const uint64_t h = std::strtoull(b, nullptr, 16);
                if (!h) continue;
                if (a[0] == 'p') {
                    g_vi_ps_ok.insert(h);
                } else if ((tag_wvp && !wvp_wanted) || !ip_ok) {
                    if (tag_wvp && !wvp_wanted) ++g_vi_vs_ok_held;
                    if (!ip_ok) ++g_vi_ip_held;
                    g_vi_vs_held.insert(h);
                } else {
                    g_vi_vs_ok.insert(h);
                    if (tag_wvp) g_vi_vs_ok_wvp.insert(h);
                    if (tag_ip) g_vi_vs_ok_ip.insert(h);
                }
                continue;
            }
            if (n < 2 || std::strlen(a) != 16 || std::strlen(b) != 16) continue;
            if ((tag_wvp && !wvp_wanted) || !ip_ok) {
                if (tag_wvp && !wvp_wanted) ++g_vi_wvp_held;
                if (!ip_ok) ++g_vi_ip_held;
                g_vi_pairs_held.insert(std::string(a) + " " + b);
                continue;
            }
            g_vi_pairs.insert(std::string(a) + " " + b);
            if (tag_wvp) g_vi_wvp_pairs.insert(std::string(a) + " " + b);
            if (tag_ip) g_vi_ip_pairs.insert(std::string(a) + " " + b);
        }
        fclose(f);
        for (const uint64_t h : g_vi_vs_held) g_vi_vs_ok.erase(h);  // (a held "wvp" or "instprev" line wins over a plain line, in either order)
        for (const auto& k : g_vi_pairs_held) g_vi_pairs.erase(k);
    }
    log::info("[pso] InstancedShaders: %zu rewritten vertex shaders, %zu pixel shaders and %zu twin pairs from %s (test only: twins "
              "made, not drawn with); per shader: %zu VS ready (%zu of them wvp, %zu wvp held), %zu PS ready; instprev: %zu VS and %zu pairs "
              "listed, %zu lines held",
              g_vi_vs.size(), g_vi_ps.size(), g_vi_pairs.size(), dir.c_str(), g_vi_vs_ok.size(), g_vi_vs_ok_wvp.size(), g_vi_vs_ok_held,
              g_vi_ps_ok.size(), g_vi_vs_ok_ip.size(), g_vi_ip_pairs.size(), g_vi_ip_held);
}

// a stream subobject, as research\sps\tools\vi\psotest.cpp builds it (its 441 twins made offline): the type at an
// 8-aligned offset, the value at +8 when it holds pointers (else +4), the whole rounded to 8
template <typename T>
void push_sub(std::vector<uint8_t>& s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& v, bool ptr) {
    const size_t off = (s.size() + 7) & ~size_t(7);
    s.resize(off, 0);
    const size_t inner = ptr ? 8 : 4;
    s.resize(off + ((inner + sizeof(T) + 7) & ~size_t(7)), 0);
    std::memcpy(s.data() + off, &type, 4);
    std::memcpy(s.data() + off + inner, &v, sizeof(T));
}

// A pipeline made at an address a released one had: its old twin forgotten (leaked, as the GPU may still use it), so
// twin_of never hands back another pipeline's twin (2026-10-14, the review).
void forget_twin(void* game) {
    if (!twins_on() || !game) return;
    std::unique_ptr<Pending> old;
    {
        std::unique_lock pl(g_pend_mutex, std::defer_lock);
        if (g_auto) {  // (run 19) a new pipeline at this address: a new creation number, an old kept desc dropped
            pl.lock();
            g_seq[game] = ++g_seq_next;
            auto it = g_pending.find(game);
            if (it != g_pending.end()) {
                old = std::move(it->second);
                g_pending.erase(it);
            }
        }
        std::lock_guard lock(g_mutex);
        g_twin.erase(game);
    }
    if (old) {
        old.reset();  // (its references released, outside the locks)
        g_auto_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}
void glow_forget(const void* p);  // ([Holsters] InteractionHighlights=parts, defined with the glow twins below)
// The twin of the game's desc d with vs_code as its vertex shader and ps_code (nullptr: d->PS) as its pixel shader, made
// by the original stream creation (not the hook: no ShaderSwap, no census of the twin). Any thread: the device is
// free-threaded (run 20: [Stereo] InstancedShadersAuto's builder thread; the folder's on the game's creation call).
// nullptr: refused (counted, the first 3 logged).
ID3D12PipelineState* create_twin(ID3D12Device2* d2, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, const D3D12_SHADER_BYTECODE& vs_code,
                                 const D3D12_SHADER_BYTECODE* ps_code, uint64_t vs, uint64_t ps, int64_t* ms_out) {
    std::vector<uint8_t> st;
    ID3D12RootSignature* rs = d->pRootSignature;
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, rs, true);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, vs_code, true);
    if (ps) push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, ps_code ? *ps_code : d->PS, true);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, d->BlendState, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, d->SampleMask, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, d->RasterizerState, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, d->DepthStencilState, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, d->InputLayout, true);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, d->IBStripCutValue, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, d->PrimitiveTopologyType, false);
    D3D12_RT_FORMAT_ARRAY fa{};
    fa.NumRenderTargets = d->NumRenderTargets;
    for (int k = 0; k < 8; ++k) fa.RTFormats[k] = d->RTVFormats[k];
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, fa, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, d->DSVFormat, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, d->SampleDesc, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, d->NodeMask, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, d->Flags, false);
    static const D3D12_VIEW_INSTANCE_LOCATION kLoc[2] = {{0, 0}, {0, 1}};  // (viewport 0, slices 0 and 1: psotest's array0)
    const D3D12_VIEW_INSTANCING_DESC vi{2, kLoc, D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING};
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, vi, true);
    const D3D12_PIPELINE_STATE_STREAM_DESC sd{st.size(), st.data()};
    ID3D12PipelineState* twin = nullptr;
    const DWORD t0 = GetTickCount();
    const HRESULT hr = o_CreateStream ? o_CreateStream(d2, &sd, IID_PPV_ARGS(&twin)) : d2->CreatePipelineState(&sd, IID_PPV_ARGS(&twin));
    const int64_t ms = static_cast<int64_t>(GetTickCount() - t0);
    if (g_auto && !t_builder) g_built_on_game.fetch_add(1, std::memory_order_relaxed);  // (run 20: never, by construction; "pso auto" shows it)
    if (ms > g_twin_ms_max.load()) g_twin_ms_max = ms;
    g_twin_ms_total.fetch_add(ms, std::memory_order_relaxed);
    if (ms_out) *ms_out = ms;
    if (SUCCEEDED(hr) && twin) glow_forget(twin);  // (the part glow: no older pipeline's glow twin at this address, the review)
    if (FAILED(hr) || !twin) {
        if (g_twins_failed.fetch_add(1, std::memory_order_relaxed) < 3)  // (the rest counted only: "pso twins")
            log::warn("[pso] twin of vs %016llx ps %016llx refused (%#lx): that pipeline stays single", static_cast<unsigned long long>(vs),
                      static_cast<unsigned long long>(ps), static_cast<unsigned long>(hr));
        return nullptr;
    }
    return twin;
}
// ([Holsters] InteractionHighlights=parts, defined with the glow twins below)
void glow_created(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, const D3D12_SHADER_BYTECODE& vs, const void* key, bool vi);
// [Stereo] InstancedShaders (the folder): the twin made inside the game's own creation of `game` and handed out at once.
bool build_twin(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, const D3D12_SHADER_BYTECODE& vs_code,
                const D3D12_SHADER_BYTECODE* ps_code, ID3D12PipelineState* game, uint64_t vs, uint64_t ps, bool wvp, bool by_shader, bool ip) {
    ID3D12Device2* d2 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d2))) || !d2) return false;
    int64_t ms = 0;
    ID3D12PipelineState* twin = create_twin(d2, d, vs_code, ps_code, vs, ps, &ms);
    d2->Release();
    if (!twin) {
        region_put(game, region::kRsBuildFailed);  // ([Stereo] MainRegion, item 8d)
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        g_twin.insert_or_assign(game, twin);  // (an old entry, if any, leaked: see forget_twin)
        if (wvp) g_wvp_twins.insert(twin);    // (a wvp VS by any line)
        if (ip && g_instprev_twins.insert(twin).second) g_instprev_n.fetch_add(1, std::memory_order_relaxed);  // (run 19: an instprev VS)
        region_put_locked(game, region::kRsMade | made_kinds(wvp, ip, false, false), twin);  // ([Stereo] MainRegion: in the same step)
    }
    // ([Holsters] InteractionHighlights=parts, run 29 item 2) a weapon pipeline's view-instanced twin gets its own glow twin
    // (the twin is the list's current pipeline wherever single pass draws with it)
    if (!t_own) glow_created(dev, d, vs_code, twin, true);
    if (by_shader) g_twins_by_shader.fetch_add(1, std::memory_order_relaxed);
    if (g_twins_made.fetch_add(1, std::memory_order_relaxed) < 3)
        log::info("[pso] twin made: vs %016llx ps %016llx (%lld ms)", static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
                  static_cast<long long>(ms));
    return true;
}
void make_twin(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, ID3D12PipelineState* game) {
    if (g_vi_vs.empty() || !d->VS.pShaderBytecode || !d->VS.BytecodeLength || !d->pRootSignature) return;
    const uint64_t vs = fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength);
    const uint64_t ps = d->PS.pShaderBytecode && d->PS.BytecodeLength ? fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength) : 0;
    auto it = g_vi_vs.find(vs);
    if (it == g_vi_vs.end()) return;
    char key[40];
    std::snprintf(key, sizeof(key), "%016llx %016llx", static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps));
    // held (a wvp VS or pair with the mirror off at start): no twin by any line
    if (g_vi_vs_held.count(vs) || g_vi_pairs_held.count(key)) {
        g_twins_skipped.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsHeld);  // ([Stereo] MainRegion, item 8d: the folder's held lines do not say which kind)
        return;
    }
    // a listed pair, or (run 18) a ready VS with a ready PS (or none)
    const bool by_shader = !g_vi_pairs.count(key) && g_vi_vs_ok.count(vs) && (ps == 0 || g_vi_ps_ok.count(ps));
    if (!g_vi_pairs.count(key) && !by_shader) {
        g_twins_skipped.fetch_add(1, std::memory_order_relaxed);  // (MainRegion: kRsNoRule, as region_created left it)
        return;
    }
    if (d->StreamOutput.NumEntries || d->DS.BytecodeLength || d->HS.BytecodeLength || d->GS.BytecodeLength) {
        g_twins_skipped.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsShape);  // ([Stereo] MainRegion, item 8d)
        return;
    }
    // the pixel shader's rewrite when the folder has one (run 14), else the game's
    auto pit = ps ? g_vi_ps.find(ps) : g_vi_ps.end();
    const D3D12_SHADER_BYTECODE ps_rw = pit == g_vi_ps.end() ? D3D12_SHADER_BYTECODE{} : D3D12_SHADER_BYTECODE{pit->second.data(), pit->second.size()};
    build_twin(dev, d, D3D12_SHADER_BYTECODE{it->second.data(), it->second.size()}, pit == g_vi_ps.end() ? nullptr : &ps_rw, game, vs, ps,
               g_vi_wvp_pairs.count(key) || g_vi_vs_ok_wvp.count(vs), by_shader, g_vi_ip_pairs.count(key) || g_vi_vs_ok_ip.count(vs));
}

// ---- run 19 item 4 / run 20 item 3, [Stereo] InstancedShadersAuto: at the game's creation of `game`
void drop_pending(std::unique_ptr<Pending> p) {  // (outside g_pend_mutex: its references released here)
    if (!p) return;
    p.reset();
    g_auto_dropped.fetch_add(1, std::memory_order_relaxed);
}
// A kept desc's state from one shader's result (under g_pend_mutex): ready on both sides -> g_ready; refused -> out of
// g_pending into *drop. (A ready VS whose twin needs a mirror that is not on at start, wvp or instprev, is refused.)
void settle(void* game, uint64_t seq, bool is_ps, vi_auto::State st, bool wvp, bool ip, std::vector<std::unique_ptr<Pending>>* drop, bool e2i = false,
            bool pw = false) {
    auto it = g_pending.find(game);
    if (it == g_pending.end() || it->second->seq != seq) return;  // (stale: dropped or another pipeline at that address)
    Pending& p = *it->second;
    // (run 20: a rewritten PS's wvp too; run 25: an eye2img PS without --eye2-images, never: the folder is keyed by it)
    // (run 26: a prevworld VS without --prevworld, never: the folder is keyed "-pw")
    const bool held = st == vi_auto::State::Ready &&
                      ((wvp && !g_auto_wvp) || (!is_ps && ip && !g_auto_ip) || (is_ps && e2i && !g_auto_e2i) || (!is_ps && pw && !g_auto_pw));
    const bool refused = st == vi_auto::State::Refused || st == vi_auto::State::Unknown || held;
    if (refused) {
        drop->push_back(std::move(it->second));
        g_pending.erase(it);
        if (g_region_write) {  // ([Stereo] MainRegion, item 8d: the reason kept per pipeline; lock order g_pend_mutex, then g_mutex)
            const uint32_t rs = held ? region::kRsHeld | held_kind(wvp && !g_auto_wvp, !is_ps && ip && !g_auto_ip, is_ps && e2i && !g_auto_e2i,
                                                                    !is_ps && pw && !g_auto_pw)
                                     : is_ps ? region::kRsPsRefused : region::kRsVsRefused;
            std::lock_guard lock(g_mutex);
            region_put_locked(game, rs);
        }
        return;
    }
    if (st != vi_auto::State::Ready) return;
    (is_ps ? p.ps_ready : p.vs_ready) = true;
    if (p.vs_ready && p.ps_ready && !p.listed) {
        p.listed = true;
        g_ready.emplace_back(game, seq);
        g_ready_any.store(true, std::memory_order_relaxed);
        if (g_region_write) {  // ([Stereo] MainRegion: both ready, the drain hands it to the builder)
            std::lock_guard lock(g_mutex);
            region_put_locked(game, region::kRsReadyNotBuilt);
        }
    }
}
// (run 20) A copy of the game's desc that lives until its twin is built (no locks): the input layout and its semantic
// names copied, no VS (the twin's, at the build), no DS/HS/GS/stream output/cached blob; the root signature, the pixel
// shader's copy and the device are attached under g_pend_mutex (attach).
std::unique_ptr<Pending> copy_desc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, uint64_t vs, uint64_t ps) {
    auto p = std::make_unique<Pending>();
    p->vs = vs;
    p->ps = ps;
    p->d = *d;
    p->d.pRootSignature = nullptr;  // (set with its reference by attach: a desc never attached releases nothing)
    p->d.VS = {};
    p->d.DS = p->d.HS = p->d.GS = {};
    p->d.StreamOutput = {};
    p->d.CachedPSO = {};
    const UINT ne = d->InputLayout.pInputElementDescs ? d->InputLayout.NumElements : 0;
    if (ne) {
        p->il.assign(d->InputLayout.pInputElementDescs, d->InputLayout.pInputElementDescs + ne);
        p->names.reserve(ne);  // (no reallocation below: the c_str()s stay put)
        for (UINT i = 0; i < ne; ++i) p->names.emplace_back(p->il[i].SemanticName ? p->il[i].SemanticName : "");
        for (UINT i = 0; i < ne; ++i)
            if (p->il[i].SemanticName) p->il[i].SemanticName = p->names[i].c_str();
    }
    p->d.InputLayout = {ne ? p->il.data() : nullptr, ne};
    p->d.PS = {};
    return p;
}
// (under g_pend_mutex) a copied desc tied to the game's pipeline `game`: its creation number (forget_twin numbers every
// pipeline before this), one copy a pixel shader shared by the descs that use it, a reference on the root signature and
// one on the device it was made on. False: not numbered (nothing attached, no twin).
bool attach(Pending* p, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, void* game, ID3D12Device2* dev) {
    auto s = g_seq.find(game);
    p->seq = s == g_seq.end() ? 0 : s->second;
    if (!p->seq) return false;
    if (p->ps) {
        auto& w = g_ps_copies[p->ps];
        p->ps_code = w.lock();
        if (!p->ps_code) {
            const uint8_t* b = static_cast<const uint8_t*>(d->PS.pShaderBytecode);
            p->ps_code = std::make_shared<const std::vector<uint8_t>>(b, b + d->PS.BytecodeLength);
            w = p->ps_code;
        }
        p->d.PS = {p->ps_code->data(), p->ps_code->size()};
    }
    p->d.pRootSignature = d->pRootSignature;
    p->d.pRootSignature->AddRef();  // (released when the desc goes: built, dropped or stale)
    p->dev = dev;
    p->dev->AddRef();
    return true;
}
// (run 20) A desc whose shaders are both ready, to the builder thread (no locks held). Its twin is installed at the safe
// point (install_built) only if `game` still has the creation number the desc was attached under.
void queue_build(std::unique_ptr<Pending> p, void* game, vi_auto::Bytes twin, vi_auto::Bytes ps_twin, bool wvp, bool ip, bool at_creation, bool e2i,
                 bool pw) {
    if (!g_builder_on.load(std::memory_order_relaxed)) {
        if (p) region_put_seq(game, p->seq, region::kRsBuildFailed);  // ([Stereo] MainRegion: never built)
        drop_pending(std::move(p));
        return;
    }
    const uint64_t seq = p ? p->seq : 0;  // ([Stereo] MainRegion)
    bool over = false;
    {
        std::lock_guard bl(g_build_mutex);
        if (g_build_q.size() >= kPendingMax) {
            over = true;
        } else {
            g_build_q.push_back(Build{std::move(p), game, std::move(twin), std::move(ps_twin), wvp, ip, at_creation, e2i, pw});
            g_build_queued.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (over) {
        g_auto_over.fetch_add(1, std::memory_order_relaxed);
        region_put_seq(game, seq, region::kRsBuildFailed);  // ([Stereo] MainRegion: the queue's cap)
        drop_pending(std::move(p));
        return;
    }
    g_build_cv.notify_one();
}
void keep_pending(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, ID3D12PipelineState* game, uint64_t vs, uint64_t ps, ID3D12Device2* dev) {
    auto p = copy_desc(d, vs, ps);
    std::vector<std::unique_ptr<Pending>> drop;
    {
        std::lock_guard pl(g_pend_mutex);
        if (g_pending.size() >= kPendingMax) {
            g_auto_over.fetch_add(1, std::memory_order_relaxed);
            if (g_region_write) {  // ([Stereo] MainRegion, item 8d: never built)
                std::lock_guard lock(g_mutex);
                region_put_locked(game, region::kRsBuildFailed);
            }
            return;  // (p not attached: it holds no reference)
        }
        if (!attach(p.get(), d, game, dev)) return;
        const uint64_t seq = p->seq;
        if (g_region_write) {  // ([Stereo] MainRegion: kept for its shaders' results; settle below may decide at once)
            std::lock_guard lock(g_mutex);
            region_put_locked(game, region::kRsQueued);
        }
        auto& slot = g_pending[game];
        if (slot) drop.push_back(std::move(slot));
        slot = std::move(p);
        // the states now (a result may have landed since auto_twin asked, before the drain reads it): a final one settled
        // here (and again, harmlessly, if the drain reads that result later); a queued one waits in the index
        bool wvp = false, ip = false, pwvp = false, pe2i = false, pw = false;
        const vi_auto::State sv = vi_auto::vs(vs, nullptr, 0, nullptr, &wvp, false, &ip, &pw);
        const vi_auto::State sp = ps ? vi_auto::ps(ps, nullptr, 0, false, nullptr, &pwvp, &pe2i) : vi_auto::State::Ready;
        if (sv == vi_auto::State::Queued) g_wait_vs[vs].emplace_back(game, seq);
        if (sp == vi_auto::State::Queued) g_wait_ps[ps].emplace_back(game, seq);
        settle(game, seq, false, sv, wvp, ip, &drop, false, pw);
        settle(game, seq, true, sp, pwvp, false, &drop, pe2i);
    }
    g_auto_kept.fetch_add(1, std::memory_order_relaxed);
    for (auto& x : drop) drop_pending(std::move(x));
}
void auto_twin(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, ID3D12PipelineState* game) {
    if (!d->VS.pShaderBytecode || !d->VS.BytecodeLength || !d->pRootSignature) return;
    // ([Stereo] MainRegion, item 8d) each creation-time decision below kept in the pipeline's slot (region_put: g_mutex, once a pipeline)
    if (d->StreamOutput.NumEntries || d->DS.BytecodeLength || d->HS.BytecodeLength || d->GS.BytecodeLength) {
        g_twins_skipped.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsShape);
        return;
    }
    using vi_auto::State;
    const uint64_t vs = fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength);
    const uint64_t ps = d->PS.pShaderBytecode && d->PS.BytecodeLength ? fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength) : 0;
    vi_auto::Bytes twin;
    bool wvp = false, ip = false, pw = false;
    const State sv = vi_auto::vs(vs, d->VS.pShaderBytecode, d->VS.BytecodeLength, &twin, &wvp, true, &ip, &pw);
    if (sv == State::Refused) {
        g_auto_refused.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsVsRefused);
        return;
    }
    if (sv == State::Ready && wvp && !g_auto_wvp) {  // (as a held "vs <hash> wvp" line: eye 2's gWorldViewProj unwritten)
        g_auto_held.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsHeld | region::kKWvp);
        return;
    }
    if (sv == State::Ready && ip && !g_auto_ip) {  // (run 20; never: the cache's instprev twins are in the -ip<K> folder only)
        g_auto_held_ip.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsHeld | region::kKIp);
        return;
    }
    if (sv == State::Ready && pw && !g_auto_pw) {  // (run 26; never: the cache's prevworld twins are in the -pw folder only)
        g_auto_held_pw.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsHeld | region::kKPw);
        return;
    }
    vi_auto::Bytes ps_twin;  // (run 20) a rewritten PS's twin, or empty
    bool pwvp = false, pe2i = false;
    const State sp = ps ? vi_auto::ps(ps, d->PS.pShaderBytecode, d->PS.BytecodeLength, true, &ps_twin, &pwvp, &pe2i) : State::Ready;
    if (sp == State::Refused) {
        g_auto_refused.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsPsRefused);
        return;
    }
    if (sp == State::Ready && pwvp && !g_auto_wvp) {  // (run 20: a rewritten PS that reads gWorldViewProj, as a wvp VS)
        g_auto_held.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsHeld | region::kKWvp);
        return;
    }
    if (sp == State::Ready && pe2i && !g_auto_e2i) {  // (run 25; never: the cache's eye2img twins are in the -e2i folder only)
        g_auto_held_e2i.fetch_add(1, std::memory_order_relaxed);
        region_put(game, region::kRsHeld | region::kKE2i);
        return;
    }
    ID3D12Device2* d2 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d2))) || !d2) return;
    if (sv == State::Ready && sp == State::Ready && twin) {
        // (run 20) both ready: the desc copied and handed to the builder thread; nothing is built on the game's thread
        auto p = copy_desc(d, vs, ps);
        bool ok = false;
        {
            std::lock_guard pl(g_pend_mutex);
            ok = attach(p.get(), d, game, d2);
            if (ok && g_region_write) {  // ([Stereo] MainRegion: to the builder, installed at a safe point; queue_build marks a refusal)
                std::lock_guard lock(g_mutex);
                region_put_locked(game, region::kRsReadyNotBuilt);
            }
        }
        if (ok) queue_build(std::move(p), game, std::move(twin), std::move(ps_twin), wvp || pwvp, ip, true, pe2i, pw);
    } else {
        keep_pending(d, game, vs, ps, d2);
    }
    d2->Release();
}
// After the game's own creation of a pipeline (any; never the mod's own), on its thread, and (run 20) at each frame's end
// on the present thread: the results that landed since the last drain are read once and applied through the by-shader
// index; the kept descs ready on both sides go to the builder thread (no device work here) until kDrainBudgetMs are spent
// (one at least); the refused are dropped.
void drain() {
    const uint64_t gen = vi_auto::generation();
    if ((gen == g_drained_gen.load(std::memory_order_relaxed) && !g_ready_any.load(std::memory_order_relaxed)) || g_draining.exchange(true)) return;
    const double t0 = log::now_ms();
    using vi_auto::State;
    std::vector<vi_auto::Result> res;
    std::vector<std::unique_ptr<Pending>> drop;
    bool more = false;
    {
        std::lock_guard pl(g_pend_mutex);
        vi_auto::results_since(&g_results_seen, &res);
        for (const auto& r : res) {
            auto& idx = r.ps ? g_wait_ps : g_wait_vs;
            auto w = idx.find(r.hash);
            if (w == idx.end()) continue;
            const std::vector<Waiter> list = std::move(w->second);
            idx.erase(w);  // (a hash's result is final: its waiters settle now)
            for (const auto& [game, seq] : list) settle(game, seq, r.ps, r.st, r.wvp, r.ip, &drop, r.e2i, r.pw);
            if (r.ps) g_ps_copies.erase(r.hash);  // (the kept descs keep their copy; a new one for later pipelines)
        }
    }
    for (auto& x : drop) drop_pending(std::move(x));
    for (;;) {
        std::unique_ptr<Pending> p;
        void* game = nullptr;
        {
            std::lock_guard pl(g_pend_mutex);
            while (!g_ready.empty() && !p) {
                const Waiter w = g_ready.front();
                g_ready.pop_front();
                auto it = g_pending.find(w.first);
                if (it == g_pending.end() || it->second->seq != w.second) continue;  // (stale)
                game = w.first;
                p = std::move(it->second);
                g_pending.erase(it);
            }
            more = !g_ready.empty();
            g_ready_any.store(more, std::memory_order_relaxed);
        }
        if (!p) break;
        vi_auto::Bytes twin, ps_twin;
        bool wvp = false, ip = false, pwvp = false, pe2i = false, pw = false;
        const bool ps_ok = !p->ps || (vi_auto::ps(p->ps, nullptr, 0, false, &ps_twin, &pwvp, &pe2i) == State::Ready && !(pwvp && !g_auto_wvp) &&
                                      !(pe2i && !g_auto_e2i));
        const State dvs = ps_ok ? vi_auto::vs(p->vs, nullptr, 0, &twin, &wvp, false, &ip, &pw) : State::Unknown;
        if (ps_ok && dvs == State::Ready && twin && !(wvp && !g_auto_wvp) && !(ip && !g_auto_ip) && !(pw && !g_auto_pw)) {
            queue_build(std::move(p), game, std::move(twin), std::move(ps_twin), wvp || pwvp, ip, false, pe2i, pw);
        } else {
            if (g_region_write) {  // ([Stereo] MainRegion, item 8d: why the ready desc is not built after all)
                const uint32_t pk = held_kind(pwvp && !g_auto_wvp, false, pe2i && !g_auto_e2i, false);
                const uint32_t vk = held_kind(wvp && !g_auto_wvp, ip && !g_auto_ip, false, pw && !g_auto_pw);
                const uint32_t rs = !ps_ok ? (pk ? region::kRsHeld | pk : region::kRsPsRefused)
                                           : (dvs == State::Ready && twin && vk ? region::kRsHeld | vk : region::kRsVsRefused);
                region_put_seq(game, p->seq, rs);
            }
            drop_pending(std::move(p));
        }
        if (log::now_ms() - t0 >= kDrainBudgetMs) break;
    }
    if (!more) g_drained_gen.store(gen, std::memory_order_relaxed);  // (else the next creation call goes on with g_ready)
    const uint64_t us = static_cast<uint64_t>((log::now_ms() - t0) * 1000.0);
    g_drain_us.fetch_add(us, std::memory_order_relaxed);
    g_drains.fetch_add(1, std::memory_order_relaxed);
    if (us > g_drain_us_max.load(std::memory_order_relaxed)) g_drain_us_max.store(us, std::memory_order_relaxed);
    g_draining.store(false);
}
// (run 20) The builder thread: each queued desc's twin made here, never on a game thread. A desc whose pipeline was
// replaced at its address while it waited is not built. The finished twin waits in g_built for the safe point.
DWORD WINAPI builder(void*) {
    t_builder = true;
    t_own = 1;  // (a debug layer's forward of its creation into the hooks: neither queued nor drained, as the mod's own)
    for (;;) {
        Build b;
        {
            std::unique_lock bl(g_build_mutex);
            g_build_cv.wait(bl, [] { return !g_build_q.empty(); });
            b = std::move(g_build_q.front());
            g_build_q.pop_front();
        }
        bool stale = false;
        {
            std::lock_guard pl(g_pend_mutex);
            auto s = g_seq.find(b.game);
            stale = s == g_seq.end() || s->second != b.p->seq;
        }
        if (stale) {
            b.p.reset();
            g_auto_stale.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const double t0 = log::now_ms();
        int64_t ms = 0;
        // (run 20) a rewritten PS in place of the game's, as the folder mode passes its g_vi_ps rewrite
        const D3D12_SHADER_BYTECODE ps_rw = b.ps_twin ? D3D12_SHADER_BYTECODE{b.ps_twin->data(), b.ps_twin->size()} : D3D12_SHADER_BYTECODE{};
        ID3D12PipelineState* twin = create_twin(b.p->dev, &b.p->d, D3D12_SHADER_BYTECODE{b.twin->data(), b.twin->size()},
                                                b.ps_twin ? &ps_rw : nullptr, b.p->vs, b.p->ps, &ms);
        const uint64_t us = static_cast<uint64_t>((log::now_ms() - t0) * 1000.0);
        g_build_us.fetch_add(us, std::memory_order_relaxed);
        if (us > g_build_us_max.load(std::memory_order_relaxed)) g_build_us_max.store(us, std::memory_order_relaxed);
        const uint64_t seq = b.p->seq, vs = b.p->vs, ps = b.p->ps;
        b.p.reset();  // (the desc's copies and its references: the creation has returned)
        b.twin.reset();
        b.ps_twin.reset();
        if (!twin) {
            g_build_failed.fetch_add(1, std::memory_order_relaxed);
            region_put_seq(b.game, seq, region::kRsBuildFailed);  // ([Stereo] MainRegion, item 8d: only if the address still has that creation)
            continue;
        }
        g_builds.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard bl(g_build_mutex);
            g_built.push_back(Built{b.game, seq, twin, vs, ps, b.wvp, b.ip, b.at_creation, b.e2i, b.pw});
        }
        g_built_any.store(true, std::memory_order_release);
    }
}
// (run 20) The safe point: each frame's end, on the game's present thread, between the frame's two passes and the next
// frame's (twin_of's answers change only here: a frame's pass 1 and pass 2 see the same twins). Under g_pend_mutex and
// g_mutex together, each finished twin is handed to its pipeline only if the address's creation number is unchanged
// (else released unused: the GPU has never seen it), with its wvp and instprev marks in the same step, until
// kInstallBudgetMs are spent (one at least; the rest at the next frame's end).
void install_built() {
    if (!g_built_any.load(std::memory_order_acquire)) return;
    const double t0 = log::now_ms();
    std::deque<Built> got;
    {
        std::lock_guard bl(g_build_mutex);
        got.swap(g_built);
        g_built_any.store(false, std::memory_order_relaxed);
    }
    std::vector<ID3D12PipelineState*> stale;
    uint64_t at_creation = 0, later = 0;
    size_t i = 0, first[3] = {}, nfirst = 0;
    {
        std::lock_guard pl(g_pend_mutex);
        std::lock_guard lock(g_mutex);
        for (; i < got.size(); ++i) {
            if (i && log::now_ms() - t0 >= kInstallBudgetMs) break;
            const Built& b = got[i];
            auto s = g_seq.find(b.game);
            if (s == g_seq.end() || s->second != b.seq) {
                stale.push_back(b.twin);
                continue;
            }
            g_twin.insert_or_assign(b.game, b.twin);
            if (b.wvp) g_wvp_twins.insert(b.twin);
            if (b.ip && g_instprev_twins.insert(b.twin).second) g_instprev_n.fetch_add(1, std::memory_order_relaxed);
            // (run 25) an eye2img twin marked in the same step as it is mapped: twin_of never sees it unmarked
            if (b.e2i && g_e2i_twins.insert(b.twin).second) g_e2i_n.fetch_add(1, std::memory_order_relaxed);
            // (run 26) a prevworld twin, likewise
            if (b.pw && g_pw_twins.insert(b.twin).second) g_pw_n.fetch_add(1, std::memory_order_relaxed);
            // ([Stereo] MainRegion, item 8d) the slot's kRsMade with the twin and its kinds, in the same step (both locks held)
            region_put_locked(b.game, region::kRsMade | made_kinds(b.wvp, b.ip, b.e2i, b.pw), b.twin);
            ++(b.at_creation ? at_creation : later);
            if (nfirst < 3) first[nfirst++] = i;
        }
    }
    if (i < got.size()) {  // (the budget spent: the rest first at the next frame's end)
        std::lock_guard bl(g_build_mutex);
        g_built.insert(g_built.begin(), got.begin() + static_cast<std::ptrdiff_t>(i), got.end());
        g_built_any.store(true, std::memory_order_release);
    }
    for (ID3D12PipelineState* t : stale) t->Release();  // (never handed out)
    g_auto_stale.fetch_add(stale.size(), std::memory_order_relaxed);
    g_auto_now.fetch_add(at_creation, std::memory_order_relaxed);
    g_auto_later.fetch_add(later, std::memory_order_relaxed);
    const uint64_t n = at_creation + later;
    g_twins_by_shader.fetch_add(n, std::memory_order_relaxed);
    const uint64_t before = g_twins_made.fetch_add(n, std::memory_order_relaxed);  // (after the map: twin_of's early-out)
    for (size_t k = 0; k < nfirst && before + k < 3; ++k)
        log::info("[pso] twin made (the builder thread, installed at the frame's end): vs %016llx ps %016llx",
                  static_cast<unsigned long long>(got[first[k]].vs), static_cast<unsigned long long>(got[first[k]].ps));
    const uint64_t us = static_cast<uint64_t>((log::now_ms() - t0) * 1000.0);
    g_install_us.fetch_add(us, std::memory_order_relaxed);
    g_installs.fetch_add(1, std::memory_order_relaxed);
    if (us > g_install_us_max.load(std::memory_order_relaxed)) g_install_us_max.store(us, std::memory_order_relaxed);
}
// the mod's time in one of the game's creation calls past its own creation (InstancedShadersAuto only)
void note_call(double t0) {
    const uint64_t us = static_cast<uint64_t>((log::now_ms() - t0) * 1000.0);
    g_call_us.fetch_add(us, std::memory_order_relaxed);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (us > g_call_us_max.load(std::memory_order_relaxed)) g_call_us_max.store(us, std::memory_order_relaxed);
}
void frame_end_auto() {  // (run 20: the drain and the safe point at each frame's end; nothing to do: three loads)
    if (!g_built_any.load(std::memory_order_relaxed) && !g_ready_any.load(std::memory_order_relaxed) &&
        vi_auto::generation() == g_drained_gen.load(std::memory_order_relaxed))
        return;
    const double t0 = log::now_ms();
    drain();
    install_built();
    const uint64_t us = static_cast<uint64_t>((log::now_ms() - t0) * 1000.0);
    g_fe_us.fetch_add(us, std::memory_order_relaxed);
    g_fe_calls.fetch_add(1, std::memory_order_relaxed);
    if (us > g_fe_us_max.load(std::memory_order_relaxed)) g_fe_us_max.store(us, std::memory_order_relaxed);
}
std::string auto_status() {
    size_t kept = 0, ready = 0, queue = 0, built = 0, ip = 0;
    {
        std::lock_guard pl(g_pend_mutex);
        kept = g_pending.size();
        ready = g_ready.size();
    }
    {
        std::lock_guard bl(g_build_mutex);
        queue = g_build_q.size();
        built = g_built.size();
    }
    ip = g_instprev_n.load();
    char b[1400];
    std::snprintf(b, sizeof(b),
                  " | pipelines: twins at creation %llu, kept for later %llu (now %zu, %zu ready; twins made later %llu, dropped %llu, stale %llu, over "
                  "the cap %llu), refused %llu, wvp held %llu (PrepassWvpMirror %s at start), the mod's own %llu; twins made %llu; drain %llu calls, "
                  "%.1f ms on the game's threads (max %.2f ms) | the game's thread: the mod's time per creation call %llu calls, %.1f ms (max %.2f ms); "
                  "frame end %llu calls, %.1f ms (max %.2f ms), installs %llu (%.1f ms, max %.2f ms); twin builds on the game's threads %llu | builder %s: "
                  "queued %llu, built %llu, refused %llu, %.1f ms on its thread (max %.2f ms), queue %zu, waiting for the safe point %zu | instprev "
                  "twins %zu (K %d at start, held %llu)",
                  static_cast<unsigned long long>(g_auto_now.load()), static_cast<unsigned long long>(g_auto_kept.load()), kept, ready,
                  static_cast<unsigned long long>(g_auto_later.load()), static_cast<unsigned long long>(g_auto_dropped.load()),
                  static_cast<unsigned long long>(g_auto_stale.load()), static_cast<unsigned long long>(g_auto_over.load()),
                  static_cast<unsigned long long>(g_auto_refused.load()), static_cast<unsigned long long>(g_auto_held.load()), g_auto_wvp ? "on" : "off",
                  static_cast<unsigned long long>(g_auto_own.load()), static_cast<unsigned long long>(g_twins_made.load()),
                  static_cast<unsigned long long>(g_drains.load()), g_drain_us.load() / 1000.0, g_drain_us_max.load() / 1000.0,
                  static_cast<unsigned long long>(g_calls.load()), g_call_us.load() / 1000.0, g_call_us_max.load() / 1000.0,
                  static_cast<unsigned long long>(g_fe_calls.load()), g_fe_us.load() / 1000.0, g_fe_us_max.load() / 1000.0,
                  static_cast<unsigned long long>(g_installs.load()), g_install_us.load() / 1000.0, g_install_us_max.load() / 1000.0,
                  static_cast<unsigned long long>(g_built_on_game.load()), g_builder_on.load() ? "on" : "OFF (its thread not started)",
                  static_cast<unsigned long long>(g_build_queued.load()), static_cast<unsigned long long>(g_builds.load()),
                  static_cast<unsigned long long>(g_build_failed.load()), g_build_us.load() / 1000.0, g_build_us_max.load() / 1000.0, queue, built, ip,
                  vi_auto::instprev_k(), static_cast<unsigned long long>(g_auto_held_ip.load()));
    std::string s = vi_auto::status() + b;
    if (g_auto_e2i) {  // (run 25) nothing with PrepassEye2Shaders off (the line as before)
        char e[600];
        std::snprintf(e, sizeof(e),
                      " | eye2img twins %zu (guard valid %d, live %d; frames live %llu, held %llu; released %llu, restored %llu; handed out %llu, held at "
                      "the draw %llu; held at creation %llu)",
                      g_e2i_n.load(), eye2_images::guard_valid() ? 1 : 0, g_e2i_live.load() ? 1 : 0, static_cast<unsigned long long>(g_e2i_frames_live.load()),
                      static_cast<unsigned long long>(g_e2i_frames_held.load()), static_cast<unsigned long long>(g_e2i_releases.load()),
                      static_cast<unsigned long long>(g_e2i_restores.load()), static_cast<unsigned long long>(g_e2i_given.load()),
                      static_cast<unsigned long long>(g_e2i_held_draws.load()), static_cast<unsigned long long>(g_auto_held_e2i.load()));
        s += e;
    }
    if (g_auto_pw) {  // (run 26) nothing with PrepassPrevWorldTwins off (the line as before)
        char e[600];
        std::snprintf(e, sizeof(e),
                      " | prevworld twins %zu (mirror installed %d, rows written in the last frame %d, live %d; frames live %llu, held %llu; released "
                      "%llu, restored %llu; handed out %llu, held at the draw %llu, refused to the main step %llu; held at creation %llu)",
                      g_pw_n.load(), prevworld::installed() ? 1 : 0, g_pw_written.load() ? 1 : 0, g_pw_live.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_pw_frames_live.load()), static_cast<unsigned long long>(g_pw_frames_held.load()),
                      static_cast<unsigned long long>(g_pw_releases.load()), static_cast<unsigned long long>(g_pw_restores.load()),
                      static_cast<unsigned long long>(g_pw_given.load()), static_cast<unsigned long long>(g_pw_held_draws.load()),
                      static_cast<unsigned long long>(g_pw_main_refused.load()), static_cast<unsigned long long>(g_auto_held_pw.load()));
        s += e;
    }
    return s;
}

// [Debug] PsoDump=<folder> (single-pass stereo step 5, milestone 1, test only): every graphics pipeline the game makes
// written to <folder>\\pso_<n>.bin (its desc, its shaders and its root signature's serialized blob), for creating its
// view-instanced twin outside the game (research\\sps\\tools\\vi\\psotest.cpp). The root signatures' blobs come
// from a CreateRootSignature hook. Format "PSO1", little-endian: each field below in order, blobs as u32 length + bytes.
using CreateRootSig_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
CreateRootSig_t o_CreateRootSig = nullptr;
std::string g_dump_dir;
std::mutex g_dump_mutex;
std::unordered_map<void*, std::vector<uint8_t>> g_root_blobs;  // under g_dump_mutex
int g_dumped = 0;
// ([Stereo] MainRegionHandover, item 8g, the review's SF1) each root signature's SRV ranges in register space 2 visible to the pixel shader:
// (parameter, base register, count; UINT_MAX unbounded), from its serialized desc (d3d12's versioned deserializer)
struct BindlessRange {
    int param;
    uint32_t base, count;
};
std::unordered_map<const void*, std::vector<BindlessRange>> g_bindless;  // under g_dump_mutex
void note_bindless(const void* root, const void* blob, SIZE_T len) {
    using Make_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    static Make_t make = reinterpret_cast<Make_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateVersionedRootSignatureDeserializer"));
    if (!make) return;
    ID3D12VersionedRootSignatureDeserializer* ds = nullptr;
    if (FAILED(make(blob, len, IID_PPV_ARGS(&ds))) || !ds) return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    std::vector<BindlessRange> v;
    if (SUCCEEDED(ds->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd && vd->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        const D3D12_ROOT_SIGNATURE_DESC1& d = vd->Desc_1_1;
        for (UINT i = 0; i < d.NumParameters && d.pParameters; ++i) {
            const D3D12_ROOT_PARAMETER1& p = d.pParameters[i];
            if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
                (p.ShaderVisibility != D3D12_SHADER_VISIBILITY_ALL && p.ShaderVisibility != D3D12_SHADER_VISIBILITY_PIXEL))
                continue;
            // (the delta review) only a table whose first range is the space-2 SRVs from register 0 at the table's start: register t<n>
            // is then the table's descriptor n, so a table at partition 1's GPU base reads the record at index n
            if (p.DescriptorTable.NumDescriptorRanges && p.DescriptorTable.pDescriptorRanges) {
                const D3D12_DESCRIPTOR_RANGE1& r = p.DescriptorTable.pDescriptorRanges[0];
                if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && r.RegisterSpace == 2 && r.BaseShaderRegister == 0 &&
                    (r.OffsetInDescriptorsFromTableStart == 0 || r.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND))
                    v.push_back({static_cast<int>(i), r.BaseShaderRegister, r.NumDescriptors});
            }
        }
    }
    ds->Release();
    std::lock_guard lock(g_dump_mutex);
    g_bindless[root] = std::move(v);
}
HRESULT STDMETHODCALLTYPE hk_CreateRootSig(ID3D12Device* dev, UINT mask, const void* blob, SIZE_T len, REFIID riid, void** out) {
    const HRESULT hr = o_CreateRootSig(dev, mask, blob, len, riid, out);
    if (SUCCEEDED(hr) && out && *out && blob && len) {
        if (g_ho_root_want) note_bindless(*out, blob, len);  // ([Stereo] MainRegionHandover, item 8g)
        if (!g_dump_dir.empty()) {
            std::lock_guard lock(g_dump_mutex);
            const uint8_t* b = static_cast<const uint8_t*>(blob);
            g_root_blobs[*out] = std::vector<uint8_t>(b, b + len);
        }
    }
    return hr;
}
void put_blob(FILE* f, const void* p, size_t n) {
    const uint32_t len = static_cast<uint32_t>(p ? n : 0);
    fwrite(&len, 4, 1, f);
    if (len) fwrite(p, 1, len, f);
}
void dump_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    std::vector<uint8_t> root;
    int n = 0;
    {
        std::lock_guard lock(g_dump_mutex);
        auto it = g_root_blobs.find(d->pRootSignature);
        if (it != g_root_blobs.end()) root = it->second;
        n = g_dumped++;
    }
    char path[600];
    std::snprintf(path, sizeof(path), "%s\\pso_%04d.bin", g_dump_dir.c_str(), n);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    fwrite("PSO1", 1, 4, f);
    put_blob(f, d->VS.pShaderBytecode, d->VS.BytecodeLength);
    put_blob(f, d->PS.pShaderBytecode, d->PS.BytecodeLength);
    put_blob(f, d->DS.pShaderBytecode, d->DS.BytecodeLength);
    put_blob(f, d->HS.pShaderBytecode, d->HS.BytecodeLength);
    put_blob(f, d->GS.pShaderBytecode, d->GS.BytecodeLength);
    put_blob(f, root.data(), root.size());
    const uint32_t so = d->StreamOutput.NumEntries;
    fwrite(&so, 4, 1, f);
    fwrite(&d->BlendState, sizeof(d->BlendState), 1, f);
    fwrite(&d->SampleMask, 4, 1, f);
    fwrite(&d->RasterizerState, sizeof(d->RasterizerState), 1, f);
    fwrite(&d->DepthStencilState, sizeof(d->DepthStencilState), 1, f);
    const uint32_t ne = d->InputLayout.pInputElementDescs ? d->InputLayout.NumElements : 0;
    fwrite(&ne, 4, 1, f);
    for (uint32_t i = 0; i < ne; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = d->InputLayout.pInputElementDescs[i];
        put_blob(f, e.SemanticName, e.SemanticName ? std::strlen(e.SemanticName) : 0);
        const uint32_t v[6] = {e.SemanticIndex, static_cast<uint32_t>(e.Format), e.InputSlot, e.AlignedByteOffset,
                               static_cast<uint32_t>(e.InputSlotClass), e.InstanceDataStepRate};
        fwrite(v, 4, 6, f);
    }
    const uint32_t tail[5] = {static_cast<uint32_t>(d->IBStripCutValue), static_cast<uint32_t>(d->PrimitiveTopologyType), d->NumRenderTargets,
                              static_cast<uint32_t>(d->DSVFormat), d->NodeMask};
    fwrite(tail, 4, 5, f);
    for (int i = 0; i < 8; ++i) {
        const uint32_t fmt = static_cast<uint32_t>(d->RTVFormats[i]);
        fwrite(&fmt, 4, 1, f);
    }
    const uint32_t sd[3] = {d->SampleDesc.Count, d->SampleDesc.Quality, static_cast<uint32_t>(d->Flags)};
    fwrite(sd, 4, 3, f);
    const uint64_t vs_hash = d->VS.pShaderBytecode && d->VS.BytecodeLength ? fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength) : 0;
    fwrite(&vs_hash, 8, 1, f);
    fclose(f);
}

void record_info(void* pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    PsoInfo in;
    in.ps = d->PS.pShaderBytecode && d->PS.BytecodeLength ? fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength) : 0;
    const UINT n = d->NumRenderTargets < 8 ? d->NumRenderTargets : 8;
    for (UINT i = 0; i < n; ++i) {
        const auto& bt = d->BlendState.RenderTarget[d->BlendState.IndependentBlendEnable ? i : 0];
        if (bt.RenderTargetWriteMask && d->RTVFormats[i] != DXGI_FORMAT_UNKNOWN) in.writes |= 1u << i;
    }
    if (d->DepthStencilState.DepthEnable && d->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL) in.writes |= 1u << 8;
    std::lock_guard lock(g_mutex);
    g_info[pso] = in;
}

// ([Stereo] MainInstanced, run 25 item 3) a game pipeline made at `pso`: out of the main set, then in it if it is the
// billboard family's (d null: a stream creation, never the family's). Logs the family's depth, stencil and blend state
// (billboard.md section 3.1's log; section 8.2 read them statically).
void main_created(void* pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    if (!g_main_want || !pso) return;
    if (g_ho_root_want && d && d->pRootSignature) {  // ([Stereo] MainRegionHandover, item 8g: the root signature its merge is made with)
        std::lock_guard lock(g_mutex);
        ID3D12RootSignature*& slot = g_ho_roots[pso];
        if (slot != d->pRootSignature) {
            d->pRootSignature->AddRef();  // (never released: test only; an address made again keeps the old one's reference)
            slot = d->pRootSignature;
        }
    }
    const uint64_t vs = d && d->VS.pShaderBytecode && d->VS.BytecodeLength ? fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength) : 0;
    const uint64_t ps = d && d->PS.pShaderBytecode && d->PS.BytecodeLength ? fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength) : 0;
    // (items 6d/6e: [Stereo] MainFamily, default the billboards; item 8e: MainFamily=region lists nothing here, the set stays empty)
    const bool family = !g_main_region && vs == kMainFams[g_main_fam].vs && ps == kMainFams[g_main_fam].ps;
    uint32_t flags = 0;
    if (family) {
        const D3D12_DEPTH_STENCIL_DESC& ds = d->DepthStencilState;
        flags = (ds.DepthEnable ? main_inst_flags::kDepthOn : 0u) |
                (ds.DepthEnable && ds.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? main_inst_flags::kDepthWrite : 0u) |
                (ds.StencilEnable && ds.StencilWriteMask ? main_inst_flags::kStencilWrite : 0u) |
                (static_cast<uint32_t>(ds.DepthFunc & 0xff) << 8) | (static_cast<uint32_t>(d->NumRenderTargets & 0xf) << 16);
    }
    bool full = false, gone = false;
    {
        std::lock_guard lock(g_mutex);
        for (auto& s : g_main_pso)
            if (s.load(std::memory_order_relaxed) == pso) {
                s.store(nullptr, std::memory_order_release);
                gone = true;
            }
        if (family) {
            full = true;
            for (int i = 0; i < kMainSlots; ++i)
                if (!g_main_pso[i].load(std::memory_order_relaxed)) {
                    g_main_flags[i].store(flags, std::memory_order_relaxed);
                    const uint32_t serial = ++g_main_serial_next;
                    if (g_main_slice_want) {  // ([Stereo] MainSlice: the desc kept; the old one's root signature reference is left as is)
                        MainDesc& m = g_main_desc[i];
                        m = MainDesc{};
                        m.root = d->pRootSignature;
                        if (m.root) m.root->AddRef();  // (never released: test only)
                        m.blend = d->BlendState;
                        m.rast = d->RasterizerState;
                        m.ds = d->DepthStencilState;
                        m.sample_mask = d->SampleMask;
                        m.sample = d->SampleDesc;
                        m.num_rt = d->NumRenderTargets;
                        for (int r = 0; r < 8; ++r) m.rtv[r] = d->RTVFormats[r];
                        m.dsv = d->DSVFormat;
                        m.topo = d->PrimitiveTopologyType;
                        m.node_mask = d->NodeMask;
                        m.flags = d->Flags;
                        m.stream_out = d->StreamOutput.NumEntries != 0 || d->StreamOutput.pSODeclaration != nullptr;
                        m.serial = serial;
                    }
                    g_main_serial[i].store(serial, std::memory_order_relaxed);
                    g_main_pso[i].store(pso, std::memory_order_release);
                    full = false;
                    break;
                }
        }
    }
    if (gone) g_main_gone.fetch_add(1, std::memory_order_relaxed);
    if (!family) return;
    if (full) {
        g_main_full.fetch_add(1, std::memory_order_relaxed);
        log::limited("pso.mainfull", 4, "[pso] MainInstanced: the %s family's set is full (%d): that pipeline is not listed", kMainFams[g_main_fam].name, kMainSlots);
        return;
    }
    g_main_made.fetch_add(1, std::memory_order_relaxed);
    const D3D12_DEPTH_STENCIL_DESC& ds = d->DepthStencilState;
    const D3D12_RENDER_TARGET_BLEND_DESC& b0 = d->BlendState.RenderTarget[0];
    log::limited("pso.mainfam", 8,
                 "[pso] MainInstanced: a %s family pipeline %p listed: depth %s func %d write %s, stencil %s read %#x write %#x "
                 "front fail %d depth-fail %d pass %d func %d, back fail %d depth-fail %d pass %d func %d; blend RT0 %s (independent %d), "
                 "RTs %u (%d %d %d), DSV %d, cull %d; write masks %#x %#x %#x, sample mask %#x, depth bias %d slope %g clip %d",
                 kMainFams[g_main_fam].name, pso, ds.DepthEnable ? "on" : "off", static_cast<int>(ds.DepthFunc), ds.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? "all" : "zero",
                 ds.StencilEnable ? "on" : "off", ds.StencilReadMask, ds.StencilWriteMask, static_cast<int>(ds.FrontFace.StencilFailOp),
                 static_cast<int>(ds.FrontFace.StencilDepthFailOp), static_cast<int>(ds.FrontFace.StencilPassOp),
                 static_cast<int>(ds.FrontFace.StencilFunc), static_cast<int>(ds.BackFace.StencilFailOp),
                 static_cast<int>(ds.BackFace.StencilDepthFailOp), static_cast<int>(ds.BackFace.StencilPassOp),
                 static_cast<int>(ds.BackFace.StencilFunc), b0.BlendEnable ? "on" : "off", d->BlendState.IndependentBlendEnable ? 1 : 0,
                 d->NumRenderTargets, static_cast<int>(d->RTVFormats[0]), static_cast<int>(d->RTVFormats[1]), static_cast<int>(d->RTVFormats[2]),
                 static_cast<int>(d->DSVFormat), static_cast<int>(d->RasterizerState.CullMode),
                 // (run 25 r61/r62: CliffWall's duplicates wrote nothing) what each target takes, and the rasterizer's depth terms
                 static_cast<unsigned>(d->BlendState.RenderTarget[0].RenderTargetWriteMask), static_cast<unsigned>(d->BlendState.RenderTarget[1].RenderTargetWriteMask),
                 static_cast<unsigned>(d->BlendState.RenderTarget[2].RenderTargetWriteMask), d->SampleMask, d->RasterizerState.DepthBias,
                 static_cast<double>(d->RasterizerState.SlopeScaledDepthBias), d->RasterizerState.DepthClipEnable ? 1 : 0);
}
// lock-free: `pso` is in the main set (its flags)
bool main_in_set(const void* pso, uint32_t* flags, uint32_t* serial = nullptr) {
    if (!pso) return false;
    for (int i = 0; i < kMainSlots; ++i)
        if (g_main_pso[i].load(std::memory_order_acquire) == pso) {
            if (flags) *flags = g_main_flags[i].load(std::memory_order_relaxed);
            if (serial) *serial = g_main_serial[i].load(std::memory_order_relaxed);
            return true;
        }
    return false;
}

// ---- [Holsters] InteractionHighlights=parts (run 29 item 2, research\run29\glow.md approach a; pso.h glow): the glow twins.
// The weapon family is matched by its pixel shaders' bytecode (FNV-1a 64 over the container, as every hash here), read with
// dxc from the player's own rdr2_weapon.fxc (research\shaders\dis_weapon, gitignored): PSWeapon (w_036, 10,304 bytes) and
// PSWeaponGold (w_037, 12,084 bytes); the length is checked first, so a creation of any other pipeline costs two compares.
// PSWeaponUI (the menus') and the shadow, reflection and point-light pixel shaders of the same file are never twinned. A
// weapon vertex shader (VSWeapon*, VSWeaponEnv*: eight blobs) with a pixel shader that is none of the file's eleven is
// logged (a game update's new variant: no twin until its hash is added), and one made through a pipeline stream too.
constexpr uint64_t kPsWeapon = 0x3e2d3228b70f21bfull, kPsWeaponGold = 0xf3f91cb2336dac0cull;
constexpr size_t kPsWeaponLen = 10304, kPsWeaponGoldLen = 12084;
struct WeaponVs {
    uint64_t hash;
    size_t len;
    const char* name;
};
constexpr WeaponVs kWeaponVs[] = {
    {0x2c7e6851f29a85feull, 7496, "VSWeapon"},          {0xdc49de7bb79710ffull, 7676, "VSWeaponEnv"},
    {0xa5354f6c4c346a44ull, 7368, "VSWeaponPacked"},    {0xc9671c7cae7f09bbull, 7496, "VSWeaponEnvPacked"},
    {0xc6b0678c40ff6117ull, 10228, "VSWeaponSkinned"},  {0x34ba8556abc52d62ull, 10428, "VSWeaponEnvSkinned"},
    {0xa006bb79672c9da5ull, 9920, "VSWeaponSkinnedPacked"}, {0xaaddb9ee9a1b0885ull, 10072, "VSWeaponEnvSkinnedPacked"},
};
// the file's other pixel shaders (never twinned; a weapon VS with one of these is not "unknown")
constexpr uint64_t kWeaponOtherPs[] = {0x66ca4ab247c84e51ull, 0x0aa89ca2e6f7a74aull, 0x674cfe5f9fa56f82ull, 0xe32f172543868abcull,
                                       0x858ab5df64f1c589ull, 0x4f71a83f3e7d6e20ull, 0x898a355728938f6dull, 0x262e76c8d5000241ull,
                                       0x33cf65b6b3ed8452ull};
const char* weapon_vs_name(const D3D12_SHADER_BYTECODE& vs) {
    if (!vs.pShaderBytecode || !vs.BytecodeLength) return nullptr;
    for (const WeaponVs& w : kWeaponVs)
        if (vs.BytecodeLength == w.len && fnv(vs.pShaderBytecode, vs.BytecodeLength) == w.hash) return w.name;
    return nullptr;
}
// -1 not the family; 0 PSWeapon, 1 PSWeaponGold
int glow_kind(const D3D12_SHADER_BYTECODE& ps) {
    if (!ps.pShaderBytecode) return -1;
    if (ps.BytecodeLength == kPsWeaponLen && fnv(ps.pShaderBytecode, ps.BytecodeLength) == kPsWeapon) return 0;
    if (ps.BytecodeLength == kPsWeaponGoldLen && fnv(ps.pShaderBytecode, ps.BytecodeLength) == kPsWeaponGold) return 1;
    return -1;
}
// A game pipeline's desc kept for its glow twin (its bytecode, input layout and names owned; its root signature and device
// referenced: kept for the session, so "holster glow bias" can make the twin again). key: the pipeline the list will have
// current when the gun draws: the game's, or (vi) its view-instanced twin, made with the folder's rewritten VS
struct GlowKept {
    const void* key = nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    std::vector<uint8_t> vs;
    std::vector<D3D12_INPUT_ELEMENT_DESC> il;
    std::vector<std::string> names;
    ID3D12Device* dev = nullptr;
    int kind = 0;
    bool vi = false;
    uint64_t vs_hash = 0;
    uint32_t built_gen = 0;  // the bias generation its twin was taken to be made with (0: none yet; under g_glow_mx)
    std::atomic<bool> failed{false};  // the last creation failed (logged once a kept desc)
    GlowKept() = default;
    GlowKept(const GlowKept&) = delete;
    GlowKept& operator=(const GlowKept&) = delete;
    ~GlowKept() {  // (never run: the list is leaked, as g_pending, so no D3D release runs in a static destructor)
        if (d.pRootSignature) d.pRootSignature->Release();
        if (dev) dev->Release();
    }
};
std::mutex g_glow_mx;  // the kept list, the builds; taken alone (never with g_mutex or g_pend_mutex)
auto& g_glow_kept = *new std::vector<std::unique_ptr<GlowKept>>;
constexpr size_t kGlowKeptMax = 512;  // (a pipeline made again at another address keeps its old entry: never freed, the builds hold them)
constexpr int kGlowSlots = 256;
struct GlowSlot {  // the playback's table (lock-free reads; written under g_glow_mx)
    std::atomic<const void*> key{nullptr};
    std::atomic<ID3D12PipelineState*> twin{nullptr};
};
GlowSlot g_glow_tab[kGlowSlots];
// each kept desc's key by its place in g_glow_kept (written under g_glow_mx; read lock-free by glow_forget at every creation,
// so a creation takes the lock only when its address was a kept weapon pipeline's)
std::atomic<const void*> g_kept_key[kGlowKeptMax];
std::atomic<bool> g_glow_want{false}, g_glow_track{false}, g_glow_worker{false};
std::atomic<int> g_glow_bias{4};
std::atomic<uint32_t> g_glow_gen{1};  // the bias's generation (a twin made with an older one is made again)
std::atomic<int> g_glow_twins{0};
std::atomic<uint64_t> g_glow_matched[2] = {}, g_glow_made{0}, g_glow_made_vi{0}, g_glow_failed{0}, g_glow_kept_n{0}, g_glow_kept_over{0},
    g_glow_unknown_ps{0}, g_glow_stream_vs{0}, g_glow_forgot{0}, g_glow_func[9] = {};
std::atomic<uint64_t> g_glow_build_us{0}, g_glow_bf_users{0};
// the list's pipeline as the game (or the mod's set_raw) last set it on this thread, while any glow twin exists
thread_local const ID3D12GraphicsCommandList* t_gcur_cl = nullptr;
thread_local ID3D12PipelineState* t_gcur_ps = nullptr;

ID3D12PipelineState* glow_lookup(const void* key) {  // (lock-free: the playback thread, per glow draw only)
    if (!key) return nullptr;
    for (GlowSlot& s : g_glow_tab)
        if (s.key.load(std::memory_order_acquire) == key) return s.twin.load(std::memory_order_acquire);
    return nullptr;
}
void glow_install_locked(const void* key, ID3D12PipelineState* twin) {  // (under g_glow_mx) an old twin leaked: the GPU may use it
    GlowSlot* fresh = nullptr;
    for (GlowSlot& s : g_glow_tab) {
        const void* k = s.key.load(std::memory_order_relaxed);
        if (k == key) {
            s.twin.store(twin, std::memory_order_release);
            return;
        }
        if (!k && !fresh) fresh = &s;
    }
    if (!fresh) {
        log::limited("pso.glowfull", 2, "[pso] part glow: the twins' table is full (%d): a twin not used", kGlowSlots);
        return;
    }
    fresh->twin.store(twin, std::memory_order_relaxed);
    fresh->key.store(key, std::memory_order_release);
    g_glow_twins.fetch_add(1, std::memory_order_relaxed);
    g_glow_track.store(true, std::memory_order_relaxed);
}
// a pipeline made at `p` (any creation): an older pipeline's glow at that address forgotten (its kept desc dropped too)
void glow_forget(const void* p) {
    if (!p) return;
    bool hit = false;
    for (GlowSlot& s : g_glow_tab)
        if (s.key.load(std::memory_order_relaxed) == p) hit = true;
    for (auto& k : g_kept_key)
        if (k.load(std::memory_order_relaxed) == p) hit = true;
    if (!hit) return;
    std::lock_guard lock(g_glow_mx);
    for (GlowSlot& s : g_glow_tab)
        if (s.key.load(std::memory_order_relaxed) == p) {
            s.key.store(nullptr, std::memory_order_release);  // (its twin leaked: the GPU may still use it)
            s.twin.store(nullptr, std::memory_order_relaxed);
            g_glow_twins.fetch_sub(1, std::memory_order_relaxed);
            g_glow_forgot.fetch_add(1, std::memory_order_relaxed);
        }
    for (size_t i = 0; i < g_glow_kept.size(); ++i)
        if (g_glow_kept[i] && g_glow_kept[i]->key == p) {
            g_glow_kept[i]->key = nullptr;  // (left in the list, never built again: its references are kept for the session)
            g_kept_key[i].store(nullptr, std::memory_order_relaxed);
        }
}
#if RDRVR_HAVE_PART_GLOW
#include "part_glow_gold.h"
#include "part_glow_weapon.h"
#endif
// The glow twin of a kept desc (any thread: the device is free-threaded; under g_glow_mx). nullptr: refused (counted, logged
// once a desc) or no pixel shader built in
ID3D12PipelineState* glow_create(GlowKept& k, const void* key) {
#if RDRVR_HAVE_PART_GLOW
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = k.d;
    d.PS = k.kind == 1 ? D3D12_SHADER_BYTECODE{kPartGlowGold, sizeof(kPartGlowGold)} : D3D12_SHADER_BYTECODE{kPartGlowWeapon, sizeof(kPartGlowWeapon)};
    // RT0: the part's diffuse (our PS) times the blend factor, added to the lit colour; its alpha kept. Every other target
    // (the motion vectors, RT1) not written
    D3D12_BLEND_DESC& b = d.BlendState;
    b.AlphaToCoverageEnable = FALSE;
    b.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 8; ++i) {
        D3D12_RENDER_TARGET_BLEND_DESC& r = b.RenderTarget[i];
        r = D3D12_RENDER_TARGET_BLEND_DESC{};
        r.SrcBlend = r.SrcBlendAlpha = D3D12_BLEND_ONE;
        r.DestBlend = r.DestBlendAlpha = D3D12_BLEND_ZERO;
        r.BlendOp = r.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        r.LogicOp = D3D12_LOGIC_OP_NOOP;
        r.RenderTargetWriteMask = 0;
    }
    D3D12_RENDER_TARGET_BLEND_DESC& r0 = b.RenderTarget[0];
    r0.BlendEnable = TRUE;
    r0.SrcBlend = D3D12_BLEND_BLEND_FACTOR;
    r0.DestBlend = D3D12_BLEND_ONE;
    r0.BlendOp = D3D12_BLEND_OP_ADD;
    r0.SrcBlendAlpha = D3D12_BLEND_ZERO;
    r0.DestBlendAlpha = D3D12_BLEND_ONE;
    r0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    r0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    // the depth read, never written; the stencil never written. The game's own test kept for EQUAL (the gun's colour pass
    // after the pre-pass: the same VS gives the same depth, as the game itself relies on), an "or equal" for LESS and
    // GREATER (the game's depth is reversed: GREATER is nearer), with a small bias toward the eye there so the glow wins over
    // its own surface and still loses to a hand in front (a few units of the depth's resolution against centimetres)
    D3D12_DEPTH_STENCIL_DESC& ds = d.DepthStencilState;
    const D3D12_COMPARISON_FUNC f = ds.DepthFunc;
    g_glow_func[static_cast<unsigned>(f) < 9 ? static_cast<unsigned>(f) : 0].fetch_add(1, std::memory_order_relaxed);
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.StencilWriteMask = 0;
    const int bias = g_glow_bias.load(std::memory_order_relaxed);
    if (ds.DepthEnable && (f == D3D12_COMPARISON_FUNC_LESS || f == D3D12_COMPARISON_FUNC_LESS_EQUAL)) {
        ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.RasterizerState.DepthBias = d.RasterizerState.DepthBias - bias;
    } else if (ds.DepthEnable && (f == D3D12_COMPARISON_FUNC_GREATER || f == D3D12_COMPARISON_FUNC_GREATER_EQUAL)) {
        ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        d.RasterizerState.DepthBias = d.RasterizerState.DepthBias + bias;
    }
    ID3D12PipelineState* twin = nullptr;
    HRESULT hr = E_FAIL;
    const int64_t t0 = static_cast<int64_t>(log::now_ms() * 1000.0);
    if (!k.vi) {
        d.VS = {k.vs.data(), k.vs.size()};
        hr = o_CreateGraphics ? o_CreateGraphics(k.dev, &d, IID_PPV_ARGS(&twin)) : k.dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&twin));
    } else {  // single pass's view-instanced twin's: its desc as create_twin builds it, with our PS and these states
        ID3D12Device2* d2 = nullptr;
        if (SUCCEEDED(k.dev->QueryInterface(IID_PPV_ARGS(&d2))) && d2) {
            std::vector<uint8_t> st;
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, d.pRootSignature, true);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE{k.vs.data(), k.vs.size()}, true);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, d.PS, true);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, d.BlendState, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, d.SampleMask, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, d.RasterizerState, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, d.DepthStencilState, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, d.InputLayout, true);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, d.IBStripCutValue, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, d.PrimitiveTopologyType, false);
            D3D12_RT_FORMAT_ARRAY fa{};
            fa.NumRenderTargets = d.NumRenderTargets;
            for (int i = 0; i < 8; ++i) fa.RTFormats[i] = d.RTVFormats[i];
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, fa, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, d.DSVFormat, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, d.SampleDesc, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, d.NodeMask, false);
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, d.Flags, false);
            static const D3D12_VIEW_INSTANCE_LOCATION kLoc[2] = {{0, 0}, {0, 1}};  // (create_twin's: viewport 0, slices 0 and 1)
            const D3D12_VIEW_INSTANCING_DESC vi{2, kLoc, D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING};
            push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, vi, true);
            const D3D12_PIPELINE_STATE_STREAM_DESC sd{st.size(), st.data()};
            hr = o_CreateStream ? o_CreateStream(d2, &sd, IID_PPV_ARGS(&twin)) : d2->CreatePipelineState(&sd, IID_PPV_ARGS(&twin));
            d2->Release();
        }
    }
    const int64_t us = static_cast<int64_t>(log::now_ms() * 1000.0) - t0;
    g_glow_build_us.fetch_add(static_cast<uint64_t>(us > 0 ? us : 0), std::memory_order_relaxed);
    if (FAILED(hr) || !twin) {
        g_glow_failed.fetch_add(1, std::memory_order_relaxed);
        if (!k.failed.exchange(true))
            log::warn("[pso] part glow: the glow twin of %s pipeline %p (VS %016llx, %s) refused (%#lx): that pipeline's parts do not glow",
                      k.kind == 1 ? "a PSWeaponGold" : "a PSWeapon", key, static_cast<unsigned long long>(k.vs_hash), k.vi ? "view-instanced" : "per eye",
                      static_cast<unsigned long>(hr));
        return nullptr;
    }
    k.failed = false;
    (k.vi ? g_glow_made_vi : g_glow_made).fetch_add(1, std::memory_order_relaxed);
    log::limited("pso.glowmade", 12, "[pso] part glow: a glow twin made for %s pipeline %p (VS %016llx, %s; depth func %d -> %d, bias %d) in %lld us",
                 k.kind == 1 ? "a PSWeaponGold" : "a PSWeapon", key, static_cast<unsigned long long>(k.vs_hash), k.vi ? "view-instanced" : "per eye",
                 static_cast<int>(f), static_cast<int>(ds.DepthFunc), d.RasterizerState.DepthBias, static_cast<long long>(us));
    return twin;
#else
    (void)k;
    (void)key;
    return nullptr;
#endif
}
// The kept descs to build (their twin missing or older than the bias's generation), each taken with its key and marked with the
// generation under g_glow_mx, so no other builder takes it; built with no lock held (a compile can take tens of ms: a game
// creation meanwhile must not wait on g_glow_mx, the review), each installed under the lock only if its key is still the one
// it was taken for (a pipeline made again at that address meanwhile: the twin leaked, never handed out). Entries are never
// freed, so the pointers stay good
struct GlowJob {
    GlowKept* k;
    const void* key;
    uint32_t gen;  // the bias's generation it was taken with: installed only while it is still the entry's (the review)
};
using GlowTake = std::vector<GlowJob>;
GlowTake glow_take_locked() {
    GlowTake v;
    const uint32_t gen = g_glow_gen.load(std::memory_order_relaxed);
    for (auto& kp : g_glow_kept)
        if (kp->key && kp->built_gen != gen) {
            kp->built_gen = gen;  // (a refusal is not tried again until the next generation)
            v.push_back({kp.get(), kp->key, gen});
        }
    return v;
}
void glow_build(const GlowTake& v) {
    for (const GlowJob& j : v) {
        ID3D12PipelineState* t = glow_create(*j.k, j.key);
        if (!t) continue;
        std::lock_guard lock(g_glow_mx);
        if (j.k->key == j.key && j.k->built_gen == j.gen) glow_install_locked(j.key, t);  // (else leaked: a newer build follows)
    }
}
DWORD WINAPI glow_worker(void*) {  // the one-shot builder: the kept descs made (want, or a new bias)
    ++t_own;  // (the mod's own creations: a layer's forward into the hooks makes no twin of them, the review)
    for (;;) {
        GlowTake v;
        {
            std::lock_guard lock(g_glow_mx);
            v = glow_take_locked();
        }
        glow_build(v);
        g_glow_worker.store(false, std::memory_order_release);
        // (a request that came while it built: built again; else done)
        bool again = false;
        {
            std::lock_guard lock(g_glow_mx);
            const uint32_t gen = g_glow_gen.load(std::memory_order_relaxed);
            for (auto& k : g_glow_kept) again = again || (k->key && k->built_gen != gen);
        }
        if (!again || g_glow_worker.exchange(true)) return 0;
    }
}
void glow_kick() {  // the worker started unless running (any thread but the presenting one's frame path: a CreateThread)
    if (g_glow_worker.exchange(true)) return;
    HANDLE th = CreateThread(nullptr, 0, glow_worker, nullptr, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        g_glow_worker.store(false);
        log::warn("[pso] part glow: the twins' worker thread not started (error %lu): the kept pipelines get no glow yet", GetLastError());
    }
}
// At the game's creation of `key` (its desc d; vs: the VS the pipeline was made with, the folder's rewrite for a view-
// instanced twin). The family kept (and made now while the mode is parts); a weapon VS with an unknown PS logged
void glow_created(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, const D3D12_SHADER_BYTECODE& vs, const void* key, bool vi) {
    if (!d || !key || !dev || !d->pRootSignature || !vs.pShaderBytecode || !vs.BytecodeLength) return;
    if (!vi) {  // (the review) the glow puts the blend factor back as (1, 1, 1, 1): a game pipeline that blends by it is counted and logged
        bool bf = false;
        for (int i = 0; i < 8; ++i) {
            const D3D12_RENDER_TARGET_BLEND_DESC& r = d->BlendState.RenderTarget[d->BlendState.IndependentBlendEnable ? i : 0];
            auto uses = [](D3D12_BLEND b) { return b == D3D12_BLEND_BLEND_FACTOR || b == D3D12_BLEND_INV_BLEND_FACTOR; };
            bf = bf || (r.BlendEnable && (uses(r.SrcBlend) || uses(r.DestBlend) || uses(r.SrcBlendAlpha) || uses(r.DestBlendAlpha)));
        }
        if (bf && g_glow_bf_users.fetch_add(1, std::memory_order_relaxed) < 4)
            log::info("[pso] part glow: a game pipeline %p blends by the blend factor (the glow restores (1, 1, 1, 1) after its draws)", key);
    }
    const int kind = glow_kind(d->PS);
    if (kind < 0) {
        if (!vi && d->PS.pShaderBytecode && d->PS.BytecodeLength) {
            if (const char* vn = weapon_vs_name(d->VS)) {
                const uint64_t ph = fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength);
                bool known = false;
                for (uint64_t h : kWeaponOtherPs) known = known || h == ph;
                if (!known) {
                    g_glow_unknown_ps.fetch_add(1, std::memory_order_relaxed);
                    log::limited("pso.glowunk", 6, "[pso] part glow: a weapon vertex shader (%s) with an unknown pixel shader %016llx (%zu bytes): not twinned",
                                 vn, static_cast<unsigned long long>(ph), static_cast<size_t>(d->PS.BytecodeLength));
                }
            }
        }
        return;
    }
    if (d->StreamOutput.NumEntries || d->DS.BytecodeLength || d->HS.BytecodeLength || d->GS.BytecodeLength) return;
    g_glow_matched[kind].fetch_add(1, std::memory_order_relaxed);
    auto k = std::make_unique<GlowKept>();
    k->key = key;
    k->d = *d;
    k->kind = kind;
    k->vi = vi;
    const uint8_t* vb = static_cast<const uint8_t*>(vs.pShaderBytecode);
    k->vs.assign(vb, vb + vs.BytecodeLength);
    k->vs_hash = fnv(vs.pShaderBytecode, vs.BytecodeLength);
    k->d.VS = {};
    k->d.PS = {};
    k->d.StreamOutput = {};
    k->d.CachedPSO = {};
    const UINT ne = d->InputLayout.pInputElementDescs ? d->InputLayout.NumElements : 0;
    k->names.reserve(ne);
    for (UINT i = 0; i < ne; ++i) k->names.emplace_back(d->InputLayout.pInputElementDescs[i].SemanticName ? d->InputLayout.pInputElementDescs[i].SemanticName : "");
    for (UINT i = 0; i < ne; ++i) {
        D3D12_INPUT_ELEMENT_DESC e = d->InputLayout.pInputElementDescs[i];
        e.SemanticName = k->names[i].c_str();
        k->il.push_back(e);
    }
    k->d.InputLayout = {k->il.empty() ? nullptr : k->il.data(), static_cast<UINT>(k->il.size())};
    k->d.pRootSignature->AddRef();
    dev->AddRef();
    k->dev = dev;
    log::limited("pso.glowmatch", 16, "[pso] part glow: %s pipeline %p matched (PS %016llx; VS %016llx%s%s; depth %s func %d, RTs %u, RT1 mask %#x)",
                 kind == 1 ? "a PSWeaponGold" : "a PSWeapon", key, static_cast<unsigned long long>(kind == 1 ? kPsWeaponGold : kPsWeapon),
                 static_cast<unsigned long long>(k->vs_hash), vi ? ", the folder's view-instanced twin" : "",
                 vi ? "" : (weapon_vs_name(d->VS) ? "" : ", not a weapon VS"), d->DepthStencilState.DepthEnable ? "on" : "off",
                 static_cast<int>(d->DepthStencilState.DepthFunc), d->NumRenderTargets,
                 static_cast<unsigned>(d->BlendState.RenderTarget[d->BlendState.IndependentBlendEnable ? 1 : 0].RenderTargetWriteMask));
    std::unique_lock lock(g_glow_mx);
    if (g_glow_kept.size() >= kGlowKeptMax) {
        g_glow_kept_over.fetch_add(1, std::memory_order_relaxed);
        log::limited("pso.glowover", 2, "[pso] part glow: %zu weapon pipelines kept already: this one gets no glow", kGlowKeptMax);
        return;  // (k released here: its two references with it)
    }
    GlowKept* kr = k.get();
    g_kept_key[g_glow_kept.size()].store(key, std::memory_order_relaxed);
    g_glow_kept.push_back(std::move(k));
    g_glow_kept_n.fetch_add(1, std::memory_order_relaxed);
    if (!g_glow_want.load(std::memory_order_relaxed)) return;
    // the mode is parts: made now, on the game's creation thread (taken under the lock, built after it: glow_build)
    kr->built_gen = g_glow_gen.load(std::memory_order_relaxed);
    const GlowTake v{{kr, key, kr->built_gen}};
    lock.unlock();
    ++t_own;
    glow_build(v);
    --t_own;
}

HRESULT STDMETHODCALLTYPE hk_CreateGraphics(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** out) {
    if (d && !g_dump_dir.empty()) dump_pso(d);
    if (d && out) {
        if (const std::vector<uint8_t>* r = swap_for(d->VS)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d2 = *d;
            d2.VS = {r->data(), r->size()};
            const HRESULT hs = o_CreateGraphics(dev, &d2, riid, out);
            if (SUCCEEDED(hs) && *out) {
                forget_twin(*out);
                g_swaps.fetch_add(1, std::memory_order_relaxed);
                g_graphics.fetch_add(1, std::memory_order_relaxed);
                record(*out, d->VS);  // counted under the game's own hash
                record_info(*out, d);
                main_created(*out, d);  // ([Stereo] MainInstanced: the game's desc, its own shaders' hashes)
                region_created(*out, d);  // ([Stereo] MainRegion, item 8d: its slot)
                glow_forget(*out);
                if (!t_own) glow_created(dev, d, d2.VS, *out, false);  // ([Holsters] InteractionHighlights=parts: the VS it was made with)
                return hs;
            }
            g_swap_fails.fetch_add(1, std::memory_order_relaxed);
            log::warn("[pso] ShaderSwap: the replacement for %016llx refused (%#lx): the game's own used",
                      static_cast<unsigned long long>(fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength)), static_cast<unsigned long>(hs));
        }
    }
    HRESULT hr = o_CreateGraphics(dev, d, riid, out);
    const double t_call = g_auto && !t_own ? log::now_ms() : 0.0;  // (run 20: the mod's time past the game's own creation)
    if (SUCCEEDED(hr) && d && out && *out) {
        g_graphics.fetch_add(1, std::memory_order_relaxed);
        record(*out, d->VS);
        record_info(*out, d);
        main_created(*out, d);  // ([Stereo] MainInstanced, test only; off: one bool)
        // ([Holsters] InteractionHighlights=parts, run 29 item 2) an older pipeline's glow at this address forgotten; a weapon
        // pipeline's desc kept (its twin made now while the mode is parts). Before the folder's twin, so its glow twin follows
        glow_forget(*out);
        if (!t_own) glow_created(dev, d, d->VS, *out, false);
        if (twins_on()) {  // [Stereo] InstancedShaders, or (run 19) InstancedShadersAuto
            forget_twin(*out);
            // ([Stereo] MainRegion, item 8d, test only; off: one bool) after forget_twin's g_seq bump (the crash review's SF1: an old
            // creation's install_built or settle can no longer land in the new pipeline's slot), before this pipeline's own decision
            region_created(*out, d);
            if (!g_auto) make_twin(dev, d, static_cast<ID3D12PipelineState*>(*out));
            else if (t_own) g_auto_own.fetch_add(1, std::memory_order_relaxed);  // (the mod's own: no twin, nothing queued)
            else auto_twin(dev, d, static_cast<ID3D12PipelineState*>(*out));
        } else {
            region_created(*out, d);
        }
    }
    if (g_auto && !t_own && SUCCEEDED(hr)) drain();  // (run 19: after the game's own creation; run 20: no device work in it)
    if (g_auto && !t_own) note_call(t_call);
    return hr;
}

// The inner size of a stream subobject and whether it holds pointers (8-byte aligned after the 4-byte type).
bool subobject(UINT type, size_t* inner, bool* ptr) {
    switch (type) {
        case 0: *inner = 8; *ptr = true; return true;                                     // root signature
        case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25:
            *inner = sizeof(D3D12_SHADER_BYTECODE); *ptr = true; return true;            // VS PS DS HS GS CS AS MS
        case 7: *inner = sizeof(D3D12_STREAM_OUTPUT_DESC); *ptr = true; return true;
        case 8: *inner = sizeof(D3D12_BLEND_DESC); *ptr = false; return true;
        case 9: case 13: case 14: case 16: case 18: case 20: *inner = 4; *ptr = false; return true;
        case 10: *inner = sizeof(D3D12_RASTERIZER_DESC); *ptr = false; return true;
        case 11: case 21: *inner = sizeof(D3D12_DEPTH_STENCIL_DESC); *ptr = false; return true;
        case 12: *inner = sizeof(D3D12_INPUT_LAYOUT_DESC); *ptr = true; return true;
        case 15: *inner = sizeof(D3D12_RT_FORMAT_ARRAY); *ptr = false; return true;
        case 17: *inner = sizeof(DXGI_SAMPLE_DESC); *ptr = false; return true;
        case 19: *inner = sizeof(D3D12_CACHED_PIPELINE_STATE); *ptr = true; return true;
        case 22: *inner = sizeof(D3D12_VIEW_INSTANCING_DESC); *ptr = true; return true;
        default: return false;
    }
}

HRESULT STDMETHODCALLTYPE hk_CreateStream(ID3D12Device* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** out) {
    if (!g_swap.empty() && d && out && d->pPipelineStateSubobjectStream && d->SizeInBytes) {  // [Debug] ShaderSwap: a copy of the stream
        std::vector<uint8_t> copy(static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream),
                                  static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream) + d->SizeInBytes);
        uint8_t* p = copy.data();
        uint8_t* end = p + copy.size();
        D3D12_SHADER_BYTECODE orig{};
        const std::vector<uint8_t>* r = nullptr;
        while (p + 4 <= end) {
            UINT type = *reinterpret_cast<const UINT*>(p);
            size_t inner = 0;
            bool ptr = false;
            if (!subobject(type, &inner, &ptr)) break;
            size_t off = ptr ? 8 : 4;
            if (type == 1) {
                auto* bc = reinterpret_cast<D3D12_SHADER_BYTECODE*>(p + off);
                orig = *bc;
                if ((r = swap_for(*bc)) != nullptr) *bc = {r->data(), r->size()};
                break;
            }
            p += (off + inner + 7) & ~size_t(7);
        }
        if (r) {
            D3D12_PIPELINE_STATE_STREAM_DESC d2{copy.size(), copy.data()};
            const HRESULT hs = o_CreateStream(dev, &d2, riid, out);
            if (SUCCEEDED(hs) && *out) {
                forget_twin(*out);
                g_swaps.fetch_add(1, std::memory_order_relaxed);
                g_stream.fetch_add(1, std::memory_order_relaxed);
                record(*out, orig);
                main_created(*out, nullptr);  // ([Stereo] MainInstanced: never the family's; an old entry at that address dropped)
                region_created(*out, nullptr);  // ([Stereo] MainRegion: a stream creation, its desc not read)
                return hs;
            }
            g_swap_fails.fetch_add(1, std::memory_order_relaxed);
            log::warn("[pso] ShaderSwap: a stream replacement refused (%#lx): the game's own used", static_cast<unsigned long>(hs));
        }
    }
    HRESULT hr = o_CreateStream(dev, d, riid, out);
    const double t_call = g_auto && !t_own ? log::now_ms() : 0.0;  // (run 20: the mod's time past the game's own creation)
    if (SUCCEEDED(hr) && d && out && *out && d->pPipelineStateSubobjectStream) {
        forget_twin(*out);
        glow_forget(*out);  // ([Holsters] InteractionHighlights=parts: a stream creation is never twinned; an old glow forgotten)
        main_created(*out, nullptr);  // ([Stereo] MainInstanced: never the family's; an old entry at that address dropped)
        region_created(*out, nullptr);  // ([Stereo] MainRegion: a stream creation, its desc not read)
        g_stream.fetch_add(1, std::memory_order_relaxed);
        const uint8_t* p = static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream);
        const uint8_t* end = p + d->SizeInBytes;
        bool found = false;
        while (p + 4 <= end) {
            UINT type = *reinterpret_cast<const UINT*>(p);
            size_t inner = 0;
            bool ptr = false;
            if (!subobject(type, &inner, &ptr)) break;
            size_t off = ptr ? 8 : 4;
            if (type == 1) {
                record(*out, *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(p + off));
                if (const char* vn = t_own ? nullptr : weapon_vs_name(*reinterpret_cast<const D3D12_SHADER_BYTECODE*>(p + off))) {
                    g_glow_stream_vs.fetch_add(1, std::memory_order_relaxed);  // (the part glow: a weapon pipeline it cannot twin)
                    log::limited("pso.glowstream", 4, "[pso] part glow: a weapon vertex shader (%s) in a pipeline stream: not twinned", vn);
                }
                found = true;
                break;
            }
            p += (off + inner + 7) & ~size_t(7);
        }
        if (!found) g_unhashed.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_auto && !t_own && SUCCEEDED(hr)) drain();  // (run 19: after the game's own creation; run 20: no device work in it)
    if (g_auto && !t_own) note_call(t_call);
    return hr;
}

void STDMETHODCALLTYPE hk_SetPipelineState(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    if (d3d::nested(d3d::kNPipeline)) return o_SetPipelineState(cl, ps);  // (the debug layer's forward: t_cl stays the game's list)
    if (g_glow_track.load(std::memory_order_relaxed)) {  // ([Holsters] InteractionHighlights=parts: once any glow twin exists)
        t_gcur_cl = cl;
        t_gcur_ps = ps;
    }
    if (prepass::active()) prepass::note_pipeline(cl, ps);  // ([Stereo] PrepassInstanced: the game's pipeline in a region)
    if (main_inst::noting()) main_inst::note_pipeline(cl, ps);  // ([Stereo] MainInstanced, test only: the list's pipeline)
    if (!g_census.load(std::memory_order_relaxed)) {
        d3d::InOriginal orig(d3d::kNPipeline);
        return o_SetPipelineState(cl, ps);
    }
    uint64_t h = 0;
    PsoInfo in;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_vs_of.find(ps);
        if (it != g_vs_of.end()) h = it->second;
        auto ii = g_info.find(ps);
        if (ii != g_info.end()) in = ii->second;
    }
    t_cl = cl;
    t_vs = h;
    t_info = in;
    t_raw_cl = nullptr;
    d3d::InOriginal orig(d3d::kNPipeline);
    o_SetPipelineState(cl, ps);
}

void STDMETHODCALLTYPE hk_SetVB(ID3D12GraphicsCommandList* cl, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    if (d3d::nested(d3d::kNVertexBuffers)) return o_SetVB(cl, start, n, v);
    if ((g_census.load(std::memory_order_relaxed) || g_vb_note.load(std::memory_order_relaxed)) && start == 0 && n >= 1 && v) {  // (item 6e: MainUnionCull's keys)
        t_vb_cl = cl;
        t_vb = v[0];
    }
    d3d::InOriginal orig(d3d::kNVertexBuffers);
    o_SetVB(cl, start, n, v);
}

void e2i_frame_end() {  // (run 25) the safe point's look at the guard: eye2img twins live for the next frame, or held
    vi_auto::eye2_check();  // (one warn line if eye2_images is not installed or stood down: every eye2img twin held)
    const bool v = eye2_images::guard_valid();
    const bool was = g_e2i_live.exchange(v, std::memory_order_acq_rel);
    (v ? g_e2i_frames_live : g_e2i_frames_held).fetch_add(1, std::memory_order_relaxed);
    if (v == was) return;
    (v ? g_e2i_restores : g_e2i_releases).fetch_add(1, std::memory_order_relaxed);
    log::limited("pso.e2i", 20, "[pso] eye2img twins (%zu) %s: eye2_images' guard %s", g_e2i_n.load(), v ? "live from the next frame" : "held",
                 v ? "valid" : "not valid");
}
void pw_frame_end() {  // (run 26) the safe point's look at the PrevWorld mirror: prevworld twins live for the next frame, or held
    // (state::presents is still this frame's here: the frame-end listeners run before the present's count)
    const bool v = prevworld::rows_written(state::presents.load(std::memory_order_relaxed));
    g_pw_written.store(v, std::memory_order_relaxed);
    const bool was = g_pw_live.exchange(v, std::memory_order_acq_rel);
    (v ? g_pw_frames_live : g_pw_frames_held).fetch_add(1, std::memory_order_relaxed);
    if (v == was) return;
    (v ? g_pw_restores : g_pw_releases).fetch_add(1, std::memory_order_relaxed);
    log::limited("pso.pw", 20, "[pso] prevworld twins (%zu) %s: the PrevWorld mirror %s", g_pw_n.load(), v ? "live from the next frame" : "held",
                 v ? "wrote eye 2's rows 5-8 in pass 1" : "did not write eye 2's rows 5-8 for every write of pass 1 (or is off)");
}
void on_frame_end(uint64_t) {
    if (g_auto.load(std::memory_order_relaxed)) frame_end_auto();  // (run 20: InstancedShadersAuto's drain and its safe point)
    if (g_auto_e2i.load(std::memory_order_relaxed)) e2i_frame_end();  // (run 25: PrepassEye2Shaders; off: one load)
    if (g_auto_pw.load(std::memory_order_relaxed)) pw_frame_end();    // (run 26: PrepassPrevWorldTwins; off: one load)
    int st = g_cap_state.load();
    if (st == 1) g_cap_state = 2;
    else if (st == 2) {
        std::lock_guard lock(g_mutex);
        FILE* f = nullptr;
        if (fopen_s(&f, g_cap_file.c_str(), "w") == 0 && f) {
            fputs(g_cap_text.c_str(), f);
            fclose(f);
        }
        g_cap_state = 3;
    }
    if (!g_census.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mutex);
    g_last.swap(g_draws);
    g_draws.clear();
}

}  // namespace

void arm_census() {
    if (!g_census.exchange(true)) log::info("[pso] the draw census started (the frame graph)");
}

const void* raw_current(const ID3D12GraphicsCommandList* cl) { return cl && cl == t_raw_cl ? t_raw_ps : nullptr; }
bool current(const ID3D12GraphicsCommandList* cl, uint64_t* vs, uint64_t* ps, uint32_t* writes) {
    if (cl != t_cl) return false;
    *vs = t_vs;
    *ps = t_info.ps;
    *writes = t_info.writes;
    return true;
}

void install_device_hooks(ID3D12Device* dev) {
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        load_swaps();
        g_main_want = config::get_int("Stereo", "MainInstanced", 0) == 1;  // ([Stereo] MainInstanced, run 25 item 3; test only)
        g_main_slice_want = g_main_want && config::get_int("Stereo", "MainSlice", 0) == 1;  // ([Stereo] MainSlice, run 25 item 5: the descs kept)
        g_ho_root_want = g_main_want && config::get_int("Stereo", "MainRegionHandover", 0) == 1;  // (item 8g, test only: the root signatures kept)
        if (g_main_want) {  // (items 6d/6e, test only) [Stereo] MainFamily: the one family listed (default bb, the billboards)
            const std::string f = config::get_string("Stereo", "MainFamily", "bb");
            int pick = -1;
            for (int i = 0; i < static_cast<int>(sizeof(kMainFams) / sizeof(kMainFams[0])); ++i)
                if (f == kMainFams[i].key) pick = i;
            if (f == "region") {  // (GOAL-single-pass item 8e, test only: the region's draws listed by main_region's classifier)
                const bool ok = config::get_int("Stereo", "MainRegion", 0) == 1 && config::get_int("Stereo", "MainUnionCull", 0) == 1 &&
                                config::get_int("Stereo", "MainPredication", 0) == 1 && config::get_bool("Stereo", "InstancedShadersAuto", false);
                if (ok) {
                    g_main_region = true;
                    log::info("[pso] [Stereo] MainFamily=region (test only, item 8e): no pipeline enters the main set; MainInstanced lists e1's draws "
                              "from main_region's classifier and takes their twins from the region table (region_twin_of, lock-free)");
                } else {
                    log::warn("[pso] [Stereo] MainFamily=region needs [Stereo] MainRegion=1, MainUnionCull=1, MainPredication=1 and "
                              "InstancedShadersAuto=1: the billboards");
                }
                pick = 0;
            }
            if (pick < 0) log::warn("[pso] [Stereo] MainFamily=%s is not bb, cw, bs or fb: the billboards", f.c_str());
            g_main_fam = pick < 0 ? 0 : pick;
            // (GOAL-single-pass item 8d, test only) [Stereo] MainRegion=1: the region table (every pipeline's slot and twin state)
            g_region_want = config::get_int("Stereo", "MainRegion", 0) == 1;
            if (g_region_want)
                log::info("[pso] [Stereo] MainRegion=1 (test only): the region table on (%u slots; every pipeline's twin state, read by "
                          "\"prepass mr observe\"; nothing hands its twins out)", kRegionSlots);
            if (g_main_fam) log::info("[pso] [Stereo] MainFamily=%s (test only): MainInstanced lists the %s family (VS %016llx, PS %016llx) instead of the billboards",
                                      kMainFams[g_main_fam].key, kMainFams[g_main_fam].name, static_cast<unsigned long long>(kMainFams[g_main_fam].vs),
                                      static_cast<unsigned long long>(kMainFams[g_main_fam].ps));
        }
        g_auto = vi_auto::init();  // (run 19: [Stereo] InstancedShadersAuto, test only; off: false, nothing started)
        if (g_auto) {
            g_auto_wvp = config::get_bool("Stereo", "PrepassWvpMirror", false);  // (atomics: read on every creation thread)
            g_auto_ip = vi_auto::instprev_k() > 0;  // (run 20: the converter's instprev twins, for PrepassInstPrevMirror=1 and its K)
            g_auto_e2i = vi_auto::eye2_shaders();   // (run 25: the converter's eye2img twins, for PrepassEye2Shaders=1)
            g_auto_pw = vi_auto::prevworld_shaders();  // (run 26: the converter's prevworld twins, for PrepassPrevWorldTwins=1)
            // (run 20) the twins' builder thread, at normal priority (a driver lock it holds is one the game's threads may wait
            // for); never joined: at exit a build in flight ends with the process
            HANDLE th = CreateThread(nullptr, 0, builder, nullptr, 0, nullptr);
            if (th) {
                CloseHandle(th);
                g_builder_on = true;
            } else {
                log::warn("[pso] InstancedShadersAuto: the twins' builder thread not started (error %lu): no twins this session", GetLastError());
            }
        }
        // (item8d.md 11, test only) [Stereo] PrepassCutHoldPending=1 with InstancedShadersAuto: the region table written (not
        // MainRegion's observe: region_on() stays off), so the walk cut's hold can tell a pending twin from one never made. Only
        // with the ini switches the walk cut's hold arms by (the review's should-fix): prepass::install runs after the first
        // device's hooks (bootstrap), so its run-time part (the walk's hooks, the draw masks' hook) is not known here; when that
        // part fails, prepass logs it and the table is written for nothing (the creation hooks' writes only: nothing reads it)
        g_region_write = g_region_want || (g_auto && prepass::cut_hold_pending_wanted());
        if (g_region_write && !g_region_want)
            log::info("[pso] [Stereo] PrepassCutHoldPending=1 (test only): the region table written (%u slots; each pipeline's twin state, read by "
                      "the walk cut's hold; MainRegion stays off)", kRegionSlots);
        else if (!g_region_want && config::get_bool("Stereo", "PrepassCutHoldPending", false))
            log::warn("[pso] [Stereo] PrepassCutHoldPending=1 needs InstancedShadersAuto=1 and PrepassWalkCut's switches (PrepassWalkCut, "
                      "PrepassInstanced, PrepassArrays, PrepassViews=2, [Compat] DrawMaskInit): the region table not written");
        load_twin_sources();
        if (config::get_bool("Debug", "PsoCensus", false)) g_census = true;
        g_dump_dir = config::get_string("Debug", "PsoDump", "");
        if (!g_dump_dir.empty()) {
            CreateDirectoryA(g_dump_dir.c_str(), nullptr);
            log::info("[pso] PsoDump: the game's pipelines written to %s", g_dump_dir.c_str());
        }
        // (PsoDump's blobs; [Stereo] MainRegionHandover, item 8g: the root signatures' bindless ranges)
        if (!g_dump_dir.empty() || g_ho_root_want)
            hooks::install("ID3D12Device::CreateRootSignature", hooks::vtable_entry(dev, 16), hk_CreateRootSig, &o_CreateRootSig);
    }
    hooks::install("ID3D12Device::CreateGraphicsPipelineState", hooks::vtable_entry(dev, 10), hk_CreateGraphics, &o_CreateGraphics);
    ID3D12Device2* d2 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d2))) && d2) {
        hooks::install("ID3D12Device2::CreatePipelineState", hooks::vtable_entry(d2, 47), hk_CreateStream, &o_CreateStream);
        d2->Release();
    }
    d3d::add_frame_end_listener(on_frame_end);
}

void on_draw_args(ID3D12GraphicsCommandList* cl, int64_t first_vertex, uint32_t count) {
    if (g_cap_state.load(std::memory_order_relaxed) != 2 || cl != t_cl || t_vs != g_cap_vs.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mutex);
    if (g_cap_draws >= 16) return;
    ++g_cap_draws;
    char buf[256];
    D3D12_VERTEX_BUFFER_VIEW vb = cl == t_vb_cl ? t_vb : D3D12_VERTEX_BUFFER_VIEW{};
    std::snprintf(buf, sizeof(buf), "draw %d: vb %#llx size %u stride %u, first vertex %lld, count %u\n", g_cap_draws,
                  static_cast<unsigned long long>(vb.BufferLocation), vb.SizeInBytes, vb.StrideInBytes,
                  static_cast<long long>(first_vertex), count);
    g_cap_text += buf;
    if (!vb.BufferLocation || !vb.StrideInBytes || first_vertex < 0) return;
    uint32_t n = count < 1024 ? count : 1024;
    uint64_t at = vb.BufferLocation + static_cast<uint64_t>(first_vertex) * vb.StrideInBytes;
    const uint8_t* p = ring_probe::cpu_of(at, static_cast<uint64_t>(n) * vb.StrideInBytes);
    if (!p) {
        g_cap_text += "  (not in a tracked upload buffer)\n";
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const float* f = reinterpret_cast<const float*>(p + static_cast<size_t>(i) * vb.StrideInBytes);
        int nf = static_cast<int>(vb.StrideInBytes / 4);
        std::string line = "  v" + std::to_string(i) + ":";
        for (int k = 0; k < nf && k < 12; ++k) {
            std::snprintf(buf, sizeof(buf), " %.4f", f[k]);
            line += buf;
        }
        g_cap_text += line + "\n";
    }
}

void install_list_hooks(ID3D12GraphicsCommandList* cl) {
    hooks::install("CL::IASetVertexBuffers", hooks::vtable_entry(cl, 44), hk_SetVB, &o_SetVB);
    hooks::install("CL::SetPipelineState", hooks::vtable_entry(cl, 25), hk_SetPipelineState, &o_SetPipelineState);
}

bool on_draw(ID3D12GraphicsCommandList* cl) {
    if (!g_census.load(std::memory_order_relaxed)) return false;
    uint64_t h = cl == t_cl ? t_vs : 0;
    if (cl == t_cl && t_info.ps && g_ps_acc_on.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_mutex);
        PsAcc& e = g_ps_acc[t_info.ps];
        ++e.draws;
        e.vs = h;
        e.writes = t_info.writes;
    }
    if (h) {
        std::lock_guard lock(g_mutex);
        ++g_draws[h];
        if (g_skip_any.load(std::memory_order_relaxed) && g_skip_set.count(h)) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    uint64_t skip = g_skip.load(std::memory_order_relaxed);
    if (skip && h == skip) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

uint64_t twins_made() { return g_twins_made.load(std::memory_order_relaxed); }
OwnCreation::OwnCreation() { ++t_own; }
OwnCreation::~OwnCreation() { --t_own; }

bool describe(const void* game, uint64_t* vs, uint64_t* ps) {  // (status only: takes g_mutex)
    std::lock_guard lock(g_mutex);
    auto v = g_vs_of.find(const_cast<void*>(game));
    auto i = g_info.find(const_cast<void*>(game));
    *vs = v != g_vs_of.end() ? v->second : 0;
    *ps = i != g_info.end() ? i->second.ps : 0;
    return v != g_vs_of.end();
}
void set_raw(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    t_raw_cl = cl;
    t_raw_ps = ps;
    if (g_glow_track.load(std::memory_order_relaxed)) {  // (the part glow's current pipeline: a twin the mod set is current too)
        t_gcur_cl = cl;
        t_gcur_ps = ps;
    }
    if (o_SetPipelineState) {
        d3d::InOriginal orig(d3d::kNPipeline);  // (a debug layer's forward inside it passes straight through the hook)
        o_SetPipelineState(cl, ps);
    } else {
        cl->SetPipelineState(ps);
    }
}
bool main_family_on() { return g_main_want; }
const char* main_family_key() { return g_main_region ? "region" : kMainFams[g_main_fam].key; }
bool main_family_region() { return g_main_region; }
void set_vb_note(bool on) { g_vb_note.store(on, std::memory_order_relaxed); }
bool last_vb(const ID3D12GraphicsCommandList* cl, uint64_t* loc, uint32_t* size) {  // (the playback thread: this thread's last IASetVertexBuffers slot 0)
    if (!g_vb_note.load(std::memory_order_relaxed) || cl != t_vb_cl) return false;
    *loc = t_vb.BufferLocation;
    *size = t_vb.SizeInBytes;
    return true;
}
bool last_vb_stride(const ID3D12GraphicsCommandList* cl, uint32_t* stride) {  // (item 8d's dynamic-ring keys: the same note's stride)
    if (!g_vb_note.load(std::memory_order_relaxed) || cl != t_vb_cl) return false;
    *stride = t_vb.StrideInBytes;
    return true;
}
bool main_withheld() { return g_main_withhold.load(std::memory_order_relaxed); }
void latch_main_withhold() {  // (the playback thread, at pass 1's pre-pass region open: both passes' lookups agree)
    // (item 8e) never with MainFamily=region: its set is empty, so twin_of, twin_given_now and the walk cut's hold read as in observe
    g_main_withhold.store(g_main_want && !g_main_region && main_inst::withholding(), std::memory_order_relaxed);
}
bool main_listed(const void* game, uint32_t* flags, uint32_t* serial) { return g_main_want && main_in_set(game, flags, serial); }
bool main_slice_kept() { return g_main_slice_want; }
int bindless_param(const void* root, uint32_t reg) {  // (item 8g, the review's SF1; once a decision)
    if (!g_ho_root_want || !root) return -1;
    std::lock_guard lock(g_dump_mutex);
    auto it = g_bindless.find(root);
    if (it == g_bindless.end()) return -1;
    for (const BindlessRange& r : it->second)
        if (reg >= r.base && (r.count == UINT_MAX || reg - r.base < r.count)) return r.param;
    return -1;
}
ID3D12RootSignature* root_of(const void* game) {  // ([Stereo] MainRegionHandover, item 8g; once a decision)
    if (!g_ho_root_want || !game) return nullptr;
    std::lock_guard lock(g_mutex);
    auto it = g_ho_roots.find(game);
    return it == g_ho_roots.end() ? nullptr : it->second;
}
bool main_desc(const void* game, uint32_t serial, MainDesc* out) {  // ([Stereo] MainSlice, test only: once per pipeline)
    if (!g_main_slice_want || !game || !out) return false;
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kMainSlots; ++i)
        if (g_main_pso[i].load(std::memory_order_relaxed) == game && g_main_desc[i].serial == serial && serial) {
            *out = g_main_desc[i];
            return true;
        }
    return false;
}
ID3D12PipelineState* main_twin_of(ID3D12PipelineState* game, ID3D12GraphicsCommandList* cl, int idx) {  // ([Stereo] MainInstanced, test only)
    if (!g_main_want || g_twins_made.load(std::memory_order_relaxed) == 0 || !main_in_set(game, nullptr)) return nullptr;
    std::lock_guard lock(g_mutex);
    auto it = g_twin.find(game);
    if (it == g_twin.end()) return nullptr;
    if (g_wvp_twins.count(it->second) || g_instprev_twins.count(it->second)) return nullptr;
    const bool e2i = !g_e2i_twins.empty() && g_e2i_twins.count(it->second);
    const bool pw = !g_pw_twins.empty() && g_pw_twins.count(it->second);
    if (e2i || pw) {
        // (GOAL-single-pass item 6c) only under the frame latch with "allowtwins" on (the playback thread: g_lat is its own);
        // off, as before 6c: neither (run 26's count of the prevworld refusals kept)
        if (!g_lat.allow) {
            if (pw) g_pw_main_refused.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        // (only inside pass 1's walk, phase 2: pass 2 draws no twin; its later drop reads the same record, walk_draw's answers)
        if (e2i && !(g_lat.phase == 2 && g_lat.e2i == 1 && eye2_light::lent_on(cl))) {  // (the record; SC2L and LB2 readable on this list)
            g_l_e2i_held.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (pw && !(g_lat.phase == 2 && g_lat.pw && prevworld::draw_mirrored())) {  // (the CB's last write before this draw mirrored for eye 2)
            g_l_pw_held.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (e2i) g_l_e2i_given.fetch_add(1, std::memory_order_relaxed);
        if (pw) {
            g_l_pw_given.fetch_add(1, std::memory_order_relaxed);
            if (idx >= 0 && idx < kPwBits) g_pw_bits[idx / 64] |= 1ull << (idx % 64);  // (SF5: pass 2's would-drop reads it)
        }
    }
    return it->second;
}
std::string main_status() {
    char b[240];
    std::snprintf(b, sizeof(b), "%s family %s (withheld from the pre-pass %d): pipelines listed %llu, set full %llu, entries dropped at a re-made address %llu",
                  g_main_region ? "region" : kMainFams[g_main_fam].name, g_main_want ? "on" : "off", g_main_withhold.load() ? 1 : 0, static_cast<unsigned long long>(g_main_made.load()), static_cast<unsigned long long>(g_main_full.load()),
                  static_cast<unsigned long long>(g_main_gone.load()));
    return b;
}
// ---- [Stereo] MainRegion (GOAL-single-pass item 8d; test only): the region table's readers and 8a's pairs (pso.h region)
bool region_on() { return g_region_want; }
namespace {
bool region_read(const void* game, RegionInfo* out) {  // (lock-free: any thread; the table written)
    unsigned i = region_home(game);
    for (unsigned p = 0; p < kRegionProbe; ++p, ++i) {
        const RegionSlot& s = g_region[i & (kRegionSlots - 1)];
        const void* at = s.pso.load(std::memory_order_acquire);
        if (!at) return false;
        if (at != game) continue;
        for (int t = 0; t < 4; ++t) {  // (a writer holds a slot for a few stores: retried, then "busy")
            const uint32_t q = s.serial.load(std::memory_order_acquire);
            if (q & 1) {
                YieldProcessor();
                continue;
            }
            RegionInfo r;
            r.vs = s.vs.load(std::memory_order_relaxed);
            r.ps = s.ps.load(std::memory_order_relaxed);
            r.flags = s.flags.load(std::memory_order_relaxed);
            r.st = s.st.load(std::memory_order_relaxed);
            r.gen = s.gen.load(std::memory_order_relaxed);
            r.twin = s.twin.load(std::memory_order_relaxed);
            r.made_at = s.made_at.load(std::memory_order_relaxed);
            r.fmt = s.fmt.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.serial.load(std::memory_order_relaxed) == q) {
                if (out) *out = r;
                return true;
            }
        }
        g_rg_busy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return false;
}
}  // namespace
bool region_lookup(const void* game, RegionInfo* out) {  // (lock-free: any thread)
    if (!g_region_want || !game) return false;
    return region_read(game, out);
}
bool twin_states_on() { return g_region_write; }  // ([Stereo] PrepassCutHoldPending)
bool twin_state(const void* game, RegionInfo* out) {  // (lock-free: any thread; MainRegion's table or PrepassCutHoldPending's)
    if (!g_region_write || !game) return false;
    return region_read(game, out);
}
// (pso.h) twin_of's rules for an installed twin, without its lock: the kinds the slot was written with at the install are the sets
// twin_of consults (install_built, build_twin: g_wvp_twins, g_instprev_twins, g_e2i_twins, g_pw_twins marked in the same step)
bool twin_given_now(const void* game, uint32_t st) {
    using namespace region;
    if ((st & 0xff) != kRsMade) return false;
    if (st & (kKPw | kKE2i)) return false;  // (prevworld: never by twin_of; eye2img: only under g_lat_pp, taken as never)
    if ((st & kKWvp) && !g_wvp_live.load(std::memory_order_relaxed)) return false;
    if ((st & kKIp) && !g_instprev_live.load(std::memory_order_relaxed)) return false;
    if (g_main_withhold.load(std::memory_order_relaxed) && main_in_set(game, nullptr)) return false;  // (lock-free: 64 slots)
    return true;
}
uint32_t region_verdict(const void* game, ID3D12GraphicsCommandList* cl, RegionInfo* out) {  // (the playback thread: g_lat is its own)
    using namespace region;
    RegionInfo r;
    if (!region_lookup(game, &r)) {
        if (out) *out = RegionInfo{};
        return kVUnknownPso;
    }
    if (out) *out = r;
    const uint32_t word = r.st << 8;
    if ((r.st & 0xff) != kRsMade) return kVNoTwin | word;
    const uint32_t k = r.st & kKindMask;
    if (k & kKWvp) return kVKindWvp | word;  // (the main walk has no WVP mirror)
    if ((k & kKE2i) && !(g_lat.phase == 2 && g_lat.e2i == 1 && eye2_light::lent_on(cl))) return kVGateE2i | word;
    if ((k & kKPw) && !(g_lat.pw && prevworld::draw_mirrored())) return kVGatePw | word;
    if ((k & kKIp) && !(inst_prev::main_live() && inst_prev::draw_ok())) return kVGateIp | word;
    return kVTwin | word;
}
// (item 8e) main_inst's hand-out (pso.h): region_verdict's rule, the latch, the formats; no g_mutex, no g_twin, no g_pw_bits (6c's
// would-drop check is index-keyed: main_inst skips it in region mode). The twin is the slot's: written under g_mutex inside the seqlock
// only by install_built (the address's creation number unchanged) or build_twin (the folder's, at the creation), cleared by
// region_created at a re-creation (after forget_twin's creation-number bump); a twin once in a slot is never released
ID3D12PipelineState* region_twin_of(const void* game, ID3D12GraphicsCommandList* cl, uint32_t fmt, bool ip_ok, uint32_t* verdict) {
    using namespace region;
    if (!g_main_region) {
        if (verdict) *verdict = kVUnknownPso;
        return nullptr;
    }
    RegionInfo r;
    uint32_t v = region_verdict(game, cl, &r);
    if ((v & 0xff) == kVTwin) {
        const uint32_t word = r.st << 8;
        if (!g_lat.allow || g_lat.phase != 2) v = kVLatch | word;  // (every kind, plain included: pass 2 draws no twin)
        // (the must-have: RTV 0-2, DSV, single-sampled, NumRT 3)
        else if (((r.flags >> 16) & 0xf) != 3 || !(fmt & kFmtSingle) || !(r.fmt & kFmtSingle) || r.fmt != fmt) v = kVFormat | word;
        else if ((r.st & kKIp) && !ip_ok) v = kVIpOff | word;  // ("prepass mi region ip off": the treerock's twins kept refused)
        else if (!r.twin) v = kVNoTwin | word;  // (kRsMade always carries one: never expected)
    }
    if (verdict) *verdict = v;
    return (v & 0xff) == kVTwin ? r.twin : nullptr;
}
std::string region_status() {
    using namespace region;
    const auto c = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
    char b[640];
    std::snprintf(b, sizeof(b),
                  "region table %s: slots listed %llu (re-made at an old address %llu, full %llu), writes %llu (no slot %llu), lookups busy %llu; "
                  "decisions written: unknown %llu queued %llu ready %llu made %llu vs_refused %llu ps_refused %llu held %llu build_failed %llu "
                  "shape %llu norule %llu",
                  g_region_want ? "on" : g_region_write ? "written (PrepassCutHoldPending only)" : "off", c(g_rg_listed), c(g_rg_remade), c(g_rg_full), c(g_rg_writes), c(g_rg_lost), c(g_rg_busy),
                  c(g_rg_states[kRsUnknown]), c(g_rg_states[kRsQueued]), c(g_rg_states[kRsReadyNotBuilt]), c(g_rg_states[kRsMade]),
                  c(g_rg_states[kRsVsRefused]), c(g_rg_states[kRsPsRefused]), c(g_rg_states[kRsHeld]), c(g_rg_states[kRsBuildFailed]),
                  c(g_rg_states[kRsShape]), c(g_rg_states[kRsNoRule]));
    return b;
}
namespace {
using namespace region;
constexpr uint32_t kPwE2 = kKPw | kKE2i;
// research\run25\item8a.md 1 (the max3 frame graph's segment e, pass 1 and 2; the verdicts with --prevworld --eye2-images 58,57,56
// --instprev 64): reporting only ("expected twin, got X"; "a pair not in 8a's census"), never a verdict
constexpr E1Pair kE1Pairs[] = {
    {0xb6691b1a, 0x3d789519, kPwE2, 0, "FlattenBlend"},
    {0xaa512b24, 0xb1ab4ea7, kPwE2, 0, "CliffWall"},
    {0xf882a913, 0xcf6fee87, kPwE2, 0, "BumpSpecAOShared"},
    {0x6ff13a3c, 0x4de08745, kPwE2, 0, "AlphaBumpSpecAOShared"},
    {0x66be95e9, 0xcf6fee87, kPwE2, 0, "BumpSpecAOSharedSkinned"},
    {0x05b8f341, 0xe5c8acb4, kPwE2, 0, "FlattenTerrain"},
    {0x77c8934f, 0xcd16b2d9, kPwE2, 0, "LowLodNoDirtSingleSided"},
    {0x6dfa72c4, 0x90ddc91a, kPwE2, 0, "Layer2AO"},
    {0x68eac036, 0xc92c563d, kPwE2, 0, "BumpAO"},
    {0x0f75198b, 0x1428cd79, kPwE2, 0, "BumpSpecAOCharSkinned"},
    {0x3b8cac2b, 0x9a193b90, kPwE2, 0, "CliffWall (variant)"},
    {0x12ac1449, 0x2ff12efd, kPwE2, 0, "TerrainBlend"},
    {0xd3746d7a, 0x0e0d5694, kPwE2, 0, "CliffWallAlpha"},
    {0xcc384d25, 0xd890542f, kPwE2, 0, "CharacterLodSkinned"},
    {0x09050c6e, 0x9cb03c95, kPwE2, 0, "BumpAOCPV"},
    {0x8ccb2c86, 0x5c4d8e1f, kPwE2, 0, "Terrain"},
    {0xad643733, 0x31e194ec, kKPlain, 1, "Billboard_NoCloudShadows"},
    {0x28270287, 0xa7621faf, kPwE2, 0, "Layer2AODecal"},
    {0xa393f3d8, 0xb6600dc1, kPwE2, 0, "SharedUVHairSkinned"},
    {0x09050c6e, 0x10cea7b8, kPwE2, 0, "BumpAOCPV"},
    {0xd420020b, 0x832744e3, kKIp | kKE2i, 1, "TreerockPrototype (instanced)"},
    {0xf0d116e7, 0xa2ab9459, kKE2i, 0, "CurvedModel"},
    {0x532d459d, 0xab519cc9, kPwE2, 0, "TreerockPrototype"},
    {0x50396911, 0xc8af26c6, kPwE2, 0, "Layer3NormalAO"},
    {0xf64f7704, 0x9750e5db, kPwE2, 0, "LowLodNoDirt"},
    {0xeb834372, 0xeeb09c8d, kPwE2, 0, "BumpSpecAOCharSkin"},
    {0xfd52eaa0, 0xb3c0c2e6, kKPw, 0, "TerrainLowLOD"},
    {0x8ff69503, 0xc5abd65e, kPwE2, 0, "Diffuse"},
    {0x29ec294b, 0x4de08745, kPwE2, 0, "AlphaBumpSpecAOSharedSkinned"},
    {0xd999e90a, 0x3477f85f, kPwE2, 0, "BumpSpecAOSharedUVSkinned"},
    {0x66312185, 0xad905f09, kKPlain, 0, "ShadowOnly_NonShadowPass"},
    {0x9f274d7f, 0x1428cd79, kPwE2, 0, "BumpSpecAOCharacter"},
    {0xebbbe26e, 0x25e89f61, kPwE2, 0, "AlphaBumpSpecAOSharedUVSkinned"},
    {0x6706d807, 0x273538c1, kPwE2, 0, "Alpha"},
    {0x57ee4135, 0x3107d7f0, kPwE2, 0, "Poster"},
    {0xa006bb79, 0x3e2d3228, kPwE2, 0, "WeaponSkinned"},
    {0x73d2ef9e, 0x28bbae41, kE1Refused, 2, "PointLight / Grass_Fullscreen"},
    {0x04b96099, 0x1faa6d18, kPwE2, 0, "BumpSpecAOClothPacked"},
    {0x0d99d69e, 0x03131e2b, kKPlain, 0, "PolyFiller"},
    {0xcb4543d4, 0x0289258e, kKPlain, 3, "FullscreenQuad / PS_NULL"},
};
constexpr int kE1PairN = static_cast<int>(sizeof(kE1Pairs) / sizeof(kE1Pairs[0]));
static_assert(kE1PairN == 40, "item8a.md 1's 40 pairs");
}  // namespace
int e1_pairs(const E1Pair** out) {
    if (out) *out = kE1Pairs;
    return kE1PairN;
}
int e1_pair(uint64_t vs, uint64_t ps) {
    const uint32_t v = static_cast<uint32_t>(vs >> 32), p = static_cast<uint32_t>(ps >> 32);
    for (int i = 0; i < kE1PairN; ++i)
        if (kE1Pairs[i].vs == v && kE1Pairs[i].ps == p) return i;
    return -1;
}
void latch_view(LatchView* out) {  // (the playback thread: g_lat is its own)
    out->phase = g_lat.phase;
    out->e2i = g_lat.e2i;
    out->e2i_why = g_lat.e2i_why;
    out->allow = g_lat.allow;
    out->pw = g_lat.pw;
}
void auto_kinds(bool* e2i, bool* pw) {
    *e2i = g_auto_e2i.load(std::memory_order_relaxed);
    *pw = g_auto_pw.load(std::memory_order_relaxed);
}

ID3D12PipelineState* twin_of(ID3D12PipelineState* game) {
    if (g_twins_made.load(std::memory_order_relaxed) == 0) return nullptr;
    if (g_main_withhold.load(std::memory_order_relaxed) && main_in_set(game, nullptr)) return nullptr;  // ([Stereo] MainInstanced: the main step's only)
    std::lock_guard lock(g_mutex);
    auto it = g_twin.find(game);
    if (it == g_twin.end()) return nullptr;
    if (!g_wvp_twins.empty() && !g_wvp_live.load(std::memory_order_relaxed) && g_wvp_twins.count(it->second)) return nullptr;
    if (!g_instprev_twins.empty() && !g_instprev_live.load(std::memory_order_relaxed) && g_instprev_twins.count(it->second)) return nullptr;
    // (run 25) the guard. (GOAL-single-pass item 6c) Latched at pass 1's first pre-pass region (prepass_open(0)): the safe
    // point's live, guard_valid() and fresh_this_frame() read there, one answer for the pre-pass regions of both passes, so a
    // guard falling (or this frame's check landing) between them never leaves pass 1 drawing a twin that pass 2 does not drop,
    // or the reverse. The pre-pass precedes this frame's check, so it is false in practice: no pre-pass draws an eye2img twin
    const bool e2i = !g_e2i_twins.empty() && g_e2i_twins.count(it->second);
    if (e2i) {
        if (!g_lat_pp.load(std::memory_order_relaxed)) {
            g_e2i_held_draws.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        g_e2i_given.fetch_add(1, std::memory_order_relaxed);
    }
    // (run 26) a prevworld twin: never handed out before item 6c (the run 26 review: enforced, not only observed; the
    // prevworld VS are main-pass only, never in a pre-pass region). (Item 6c) Still never here: only main_twin_of hands one
    // out, under the frame latch and prevworld's per-draw gate (g_pw_given stays 0).
    if (!g_pw_twins.empty() && g_pw_twins.count(it->second)) {
        g_pw_held_draws.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (g_lat.allow) {  // (item 6c's check, SF5: prepass_draw counts this pipeline's twinned draws as eye2img ones)
        t_tw_last = it->second;
        t_tw_e2i = e2i;
    }
    return it->second;
}
// ---- (GOAL-single-pass item 6c) the frame latch (pso.h). The playback thread's, in the frame's order
namespace {
// the latch's first stage (pass 1's start): a new frame's record
// (the review's SF4) only pass 1's first pre-pass region (prepass_open(0)) latches twin_of's answer; a first stage at the walk
// keeps the one the pre-pass regions of this frame use
void latch_first(bool prepass) {
    Latch& L = g_lat;
    L = Latch{};
    L.phase = 1;
    L.allow = g_allow_want.load(std::memory_order_relaxed);
    L.live = g_e2i_live.load(std::memory_order_acquire);
    if (prepass) {
        L.pp = g_auto_e2i.load(std::memory_order_relaxed) && L.live && eye2_images::guard_valid() && eye2_images::fresh_this_frame();
        g_lat_pp.store(L.pp, std::memory_order_relaxed);
        g_pp[0] = PpAcc{};
        g_pp[1] = PpAcc{};
    } else {
        L.pp = g_lat_pp.load(std::memory_order_relaxed);
    }
    L.pw = L.allow && g_auto_pw.load(std::memory_order_relaxed) && prevworld::gate_live();
    g_l_first.fetch_add(1, std::memory_order_relaxed);
    if (L.pw) g_l_pw_frames.fetch_add(1, std::memory_order_relaxed);
}
// the unlatched rule's answers now (what an answer read at each consult would be): the positive control of the agreement
uint32_t shadow_now(bool prepass) {
    const bool e2i = g_auto_e2i.load(std::memory_order_relaxed) && g_e2i_live.load(std::memory_order_acquire) && eye2_images::guard_valid() &&
                     eye2_images::fresh_this_frame();
    if (prepass) return e2i ? kAnsPpY : kAnsPpN;
    const bool pw = g_lat.allow && g_auto_pw.load(std::memory_order_relaxed) && prevworld::gate_live();
    return ((g_lat.allow && e2i) ? kAnsE2iY : kAnsE2iN) | (pw ? kAnsPwY : kAnsPwN);
}
uint32_t answers() { return (g_lat.e2i == 1 ? kAnsE2iY : kAnsE2iN) | (g_lat.pw ? kAnsPwY : kAnsPwN); }
// the second stage: inside pass 1's walk (its first draw, after the draw's poll ran the queued steps), on the thread that
// hands the twins out, where 6b's fresh_this_frame() answers for this frame
void latch_second(ID3D12GraphicsCommandList* cl) {
    Latch& L = g_lat;
    const bool gv = eye2_images::guard_valid(), fr = eye2_images::fresh_this_frame();
    bool yes = false;
    int why = 0;  // (item 8d's observe: the reason kept for the frame, LatchView)
    if (!g_auto_e2i.load(std::memory_order_relaxed)) why = 1;
    else if (!L.live) why = 2;
    else if (!gv) why = 3;
    else if (!fr) why = 4;
    else if (!eye2_light::lend(cl)) why = 5;  // (SC2L and LB2 not lent: none handed out)
    else yes = true;
    LCount* const no[6] = {nullptr, &g_l_no_kind, &g_l_no_live, &g_l_no_guard, &g_l_no_fresh, &g_l_no_lend};
    if (why) no[why]->fetch_add(1, std::memory_order_relaxed);
    L.e2i = yes ? 1 : 0;
    L.e2i_why = why;
    (yes ? g_l_dec_yes : g_l_dec_no).fetch_add(1, std::memory_order_relaxed);
}
bool both(uint32_t a, uint32_t y, uint32_t n) { return (a & y) && (a & n); }
bool flipped(uint32_t a) { return both(a, kAnsE2iY, kAnsE2iN) || both(a, kAnsPwY, kAnsPwN) || both(a, kAnsPpY, kAnsPpN); }
// the two passes' answers of one kind compared only when both passes consulted it (a pass 2 without a pre-pass region, under
// the walk cut, has no pre-pass answer)
bool agree_kind(uint32_t a, uint32_t b, uint32_t mask) {
    const uint32_t x = a & mask, y = b & mask;
    return !x || !y || x == y;
}
bool agree(uint32_t a, uint32_t b) {
    return agree_kind(a, b, kAnsE2iY | kAnsE2iN) && agree_kind(a, b, kAnsPwY | kAnsPwN) && agree_kind(a, b, kAnsPpY | kAnsPpN);
}
}  // namespace

void prepass_open(int slice) {
    if (slice == 0) {
        latch_first(true);
        g_lat.ans[1] |= g_lat.pp ? kAnsPpY : kAnsPpN;
        g_lat.shadow[1] |= shadow_now(true);
        (g_lat.pp ? g_l_pp_yes : g_l_pp_no).fetch_add(1, std::memory_order_relaxed);
    } else if (slice == 1 && g_lat.phase >= 1) {  // (pass 2's region: the same latched answer; the unlatched rule's for the control)
        g_lat.ans[2] |= g_lat.pp ? kAnsPpY : kAnsPpN;
        g_lat.shadow[2] |= shadow_now(true);
    }
}
void walk_open(int pass) {
    Latch& L = g_lat;
    if (pass == 1) {
        if (L.phase != 1) {  // (no pre-pass region this frame: the first stage here)
            latch_first(false);
            g_l_at_walk.fetch_add(1, std::memory_order_relaxed);
        }
        L.phase = 2;
        L.e2i = -1;
        std::memset(g_pw_bits, 0, sizeof(g_pw_bits));
        prevworld::pb_walk_reset();  // (the verdict "not" until the walk's own markers play)
        return;
    }
    if (pass != 2) return;
    if (L.phase != 2) {  // (pass 2 without this frame's pass 1 walk: nothing handed out, nothing compared)
        g_l_order.fetch_add(1, std::memory_order_relaxed);
        const bool pp = L.pp;
        L = Latch{};
        L.pp = pp;  // (SF4: twin_of's answer is only prepass_open(0)'s to write)
        return;
    }
    L.phase = 3;
}
void walk_draw(ID3D12GraphicsCommandList* cl, int pass) {
    Latch& L = g_lat;
    if (!L.allow) return;  // (off: at once)
    if (pass == 1 && L.phase == 2) {
        g_l_draws1.fetch_add(1, std::memory_order_relaxed);
        if (L.e2i < 0) {
            latch_second(cl);
        } else if (L.e2i == 1 && !eye2_light::lent_on(cl)) {  // (a later list of the walk: lent on it too, the last one given back at its Close)
            (eye2_light::lend(cl) ? g_l_relend : g_l_relend_failed).fetch_add(1, std::memory_order_relaxed);
        }
        L.ans[1] |= answers();
        L.shadow[1] |= shadow_now(false);
        if (L.pw) (prevworld::draw_mirrored() ? g_l_pw_sel_m : g_l_pw_sel_u).fetch_add(1, std::memory_order_relaxed);
    } else if (pass == 2 && L.phase == 3) {
        g_l_draws2.fetch_add(1, std::memory_order_relaxed);
        L.ans[2] |= answers();
        L.shadow[2] |= shadow_now(false);
    }
}
void walk_close(ID3D12GraphicsCommandList* cl, int pass) {
    Latch& L = g_lat;
    if (pass == 1) {
        if (L.phase == 2 && L.allow) {
            if (L.e2i < 0) {  // (no draw in the walk: decided "no" here)
                L.e2i = 0;
                g_l_undecided.fetch_add(1, std::memory_order_relaxed);
            }
            L.ans[1] |= answers();
        }
        if (eye2_light::lent_any()) {  // SC2L and LB2 back to COPY_DEST after pass 1's walk, before eye2_light's next copies
            if (cl && eye2_light::lent_on(cl)) {
                eye2_light::give_back(cl);
                g_l_gb_close.fetch_add(1, std::memory_order_relaxed);
            } else {
                eye2_light::owe_give_back();  // (the lent list's next call, or its Close)
                g_l_gb_owed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    if (pass != 2 || L.phase != 3) return;
    if (L.allow) {
        L.ans[2] |= answers();
        if (g_pp[1].draws == 0) {  // (SF5: pass 2's pre-pass region drew nothing, as under the walk cut: not compared)
            g_pp_skipped.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_pp_cmp.fetch_add(1, std::memory_order_relaxed);
            g_pp_e2i1.fetch_add(g_pp[0].e2i, std::memory_order_relaxed);
            if (g_pp[0].twinned != g_pp[1].twinned || g_pp[0].sum != g_pp[1].sum) g_pp_differ.fetch_add(1, std::memory_order_relaxed);
            if (g_pp[0].e2i != g_pp[1].e2i) g_pp_differ_e2i.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_l_cmp.fetch_add(1, std::memory_order_relaxed);
    if (!agree(L.ans[1], L.ans[2])) g_l_disagree.fetch_add(1, std::memory_order_relaxed);
    if (!agree(L.shadow[1], L.shadow[2])) g_l_live_disagree.fetch_add(1, std::memory_order_relaxed);
    if (flipped(L.ans[1]) || flipped(L.ans[2])) g_l_flip.fetch_add(1, std::memory_order_relaxed);
    if (flipped(L.shadow[1]) || flipped(L.shadow[2])) g_l_live_flip.fetch_add(1, std::memory_order_relaxed);
    L.phase = 0;
}
// (SF5) a pre-pass region's draw (prepass_arrays before_draw, past its empty-draw case): twin, the twin drawn (pass 1) or the
// reason it is dropped (pass 2); nullptr drawn single
void prepass_draw(int slice, const void* game, ID3D12PipelineState* twin) {
    if (!g_lat.allow || slice < 0 || slice > 1) return;
    PpAcc& a = g_pp[slice];
    ++a.draws;
    if (!twin) return;
    ++a.twinned;
    a.sum += (reinterpret_cast<uintptr_t>(game) ^ 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    if (twin == t_tw_last && t_tw_e2i) ++a.e2i;
}
// (SF5) pass 2's would-drop of a listed draw, by the rule a drop will use: a twin exists and, for an eye2img twin, the frame's
// record; for a prevworld twin, pass 1 handed it out at the same listed index (idx). Takes the twins' mutex (per listed draw)
bool main_would_drop(ID3D12PipelineState* game, int idx) {
    if (!g_lat.allow || g_lat.phase != 3 || !g_main_want || g_twins_made.load(std::memory_order_relaxed) == 0 || !main_in_set(game, nullptr))
        return false;
    std::lock_guard lock(g_mutex);
    auto it = g_twin.find(game);
    if (it == g_twin.end() || g_wvp_twins.count(it->second) || g_instprev_twins.count(it->second)) return false;
    if (!g_e2i_twins.empty() && g_e2i_twins.count(it->second) && g_lat.e2i != 1) return false;
    if (!g_pw_twins.empty() && g_pw_twins.count(it->second) && (idx < 0 || idx >= kPwBits || !(g_pw_bits[idx / 64] >> (idx % 64) & 1))) return false;
    g_md_would.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void main_drop_compare(int twins1, int would2) {
    g_md_cmp.fetch_add(1, std::memory_order_relaxed);
    if (twins1 != would2) g_md_differ.fetch_add(1, std::memory_order_relaxed);
}
bool allow_latched() { return g_lat.allow; }
void set_allow_twins(bool on) {
    g_allow_want.store(on, std::memory_order_relaxed);
    prevworld::set_gate(on);
    log::info("[pso] allowtwins %s (test only: main_twin_of may hand eye2img and prevworld twins to the listed families under the frame latch, "
              "from the next pass 1)", on ? "on" : "off");
}
bool allow_twins() { return g_allow_want.load(std::memory_order_relaxed); }
std::string latch_kv() {
    const auto u = [](const LCount& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
    char b[1800];
    std::snprintf(b, sizeof(b),
                  "allow=%d auto_e2i=%d auto_pw=%d e2i_twins=%zu pw_twins=%zu lat_pp=%d first=%llu at_walk=%llu order=%llu dec_yes=%llu dec_no=%llu "
                  "no_kind=%llu no_live=%llu no_guard=%llu no_fresh=%llu no_lend=%llu undecided=%llu relend=%llu relend_failed=%llu cmp=%llu "
                  "disagree=%llu live_disagree=%llu flip=%llu live_flip=%llu pw_frames=%llu pw_sel_m=%llu pw_sel_u=%llu e2i_given=%llu e2i_held=%llu "
                  "pw_given=%llu pw_held=%llu gb_close=%llu gb_owed=%llu pp_yes=%llu pp_no=%llu draws1=%llu draws2=%llu pp_cmp=%llu pp_skipped=%llu "
                  "pp_differ=%llu pp_differ_e2i=%llu pp_e2i1=%llu md_cmp=%llu md_differ=%llu md_would=%llu",
                  g_allow_want.load() ? 1 : 0, g_auto_e2i.load() ? 1 : 0, g_auto_pw.load() ? 1 : 0, g_e2i_n.load(), g_pw_n.load(), g_lat_pp.load() ? 1 : 0,
                  u(g_l_first), u(g_l_at_walk), u(g_l_order), u(g_l_dec_yes), u(g_l_dec_no), u(g_l_no_kind), u(g_l_no_live), u(g_l_no_guard),
                  u(g_l_no_fresh), u(g_l_no_lend), u(g_l_undecided), u(g_l_relend), u(g_l_relend_failed), u(g_l_cmp), u(g_l_disagree),
                  u(g_l_live_disagree), u(g_l_flip), u(g_l_live_flip), u(g_l_pw_frames), u(g_l_pw_sel_m), u(g_l_pw_sel_u), u(g_l_e2i_given),
                  u(g_l_e2i_held), u(g_l_pw_given), u(g_l_pw_held), u(g_l_gb_close), u(g_l_gb_owed), u(g_l_pp_yes), u(g_l_pp_no), u(g_l_draws1),
                  u(g_l_draws2), u(g_pp_cmp), u(g_pp_skipped), u(g_pp_differ), u(g_pp_differ_e2i), u(g_pp_e2i1), u(g_md_cmp), u(g_md_differ),
                  u(g_md_would));
    return std::string(b) + " " + eye2_light::lend_kv() + " " + prevworld::gate_kv();
}

void set_wvp_live(bool on) { g_wvp_live.store(on, std::memory_order_relaxed); }
void set_instprev_live(bool on) { g_instprev_live.store(on, std::memory_order_relaxed); }
bool instprev_twin(ID3D12PipelineState* twin) {
    if (!twin || g_instprev_n.load(std::memory_order_relaxed) == 0) return false;
    std::lock_guard lock(g_mutex);
    return g_instprev_twins.count(twin) != 0;
}
std::string twins_status() {
    char b[420];
    std::snprintf(b, sizeof(b), "twins %s: %zu rewritten VS, %zu pairs listed; made %llu, refused %llu, not listed %llu; slowest %lld ms, all %lld ms",
                  twins_on() ? (g_auto ? "on (auto)" : "on") : "off", g_vi_vs.size(), g_vi_pairs.size(), static_cast<unsigned long long>(g_twins_made.load()),
                  static_cast<unsigned long long>(g_twins_failed.load()), static_cast<unsigned long long>(g_twins_skipped.load()),
                  static_cast<long long>(g_twin_ms_max.load()), static_cast<long long>(g_twin_ms_total.load()));
    std::string s = b;
    if (!g_vi_wvp_pairs.empty() || g_vi_wvp_held) {
        size_t made = 0;
        {
            std::lock_guard lock(g_mutex);
            made = g_wvp_twins.size();
        }
        std::snprintf(b, sizeof(b), " | WVP-mirror pairs: %zu listed (%zu held: [Stereo] PrepassWvpMirror off at start), %zu twins made, %s",
                      g_vi_wvp_pairs.size(), g_vi_wvp_held, made, g_wvp_live.load() ? "drawn (the mirror on)" : "not drawn (the mirror off)");
        s += b;
    }
    if (!g_vi_vs_ok_ip.empty() || !g_vi_ip_pairs.empty() || g_vi_ip_held) {  // (run 19 item 2) [Stereo] PrepassInstPrevMirror's VS
        std::snprintf(b, sizeof(b), " | instprev: %zu VS and %zu pairs listed, %zu lines held (PrepassInstPrevMirror not 1 or another K at start), "
                      "%zu twins made, %s", g_vi_vs_ok_ip.size(), g_vi_ip_pairs.size(), g_vi_ip_held, g_instprev_n.load(),
                      g_instprev_live.load() ? "drawn where mirrored (the mirror on)" : "not drawn (the mirror off)");
        s += b;
    }
    if (!g_vi_vs_ok.empty() || g_vi_vs_ok_held) {  // (run 18) the per-shader rule
        std::snprintf(b, sizeof(b), " | by shader: %zu VS ready (%zu wvp, %zu wvp held), %zu PS ready; twins made by it %llu", g_vi_vs_ok.size(),
                      g_vi_vs_ok_wvp.size(), g_vi_vs_ok_held, g_vi_ps_ok.size(), static_cast<unsigned long long>(g_twins_by_shader.load()));
        s += b;
    }
    if (g_auto) s += " | " + auto_status();  // (run 19)
    return s;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string word, sub, arg;
    in >> word >> sub >> arg;
    if (sub == "twins") return twins_status();  // pso twins ([Stereo] InstancedShaders; before the census is armed)
    if (sub == "auto") {  // pso auto [reset] (run 19: [Stereo] InstancedShadersAuto; never arms the census)
        if (!g_auto) return "auto off ([Stereo] InstancedShadersAuto=0 at start)";
        if (arg == "reset") vi_auto::reset_counters();
        return auto_status();
    }
    char buf[256];
    if (!g_census.exchange(true)) {
        Sleep(250);  // a few frames counted (a pipeline set before now counts from its next set)
        log::info("[pso] the draw census started");
    }
    if (sub == "vbcap") {
        // pso vbcap <vs hash> <file>: capture the next frame's draws of that vertex shader
        std::string file;
        in >> file;
        if (arg.empty() || file.empty()) return "ERROR usage: pso vbcap <vs hash> <file>";
        {
            std::lock_guard lock(g_mutex);
            g_cap_file = file;
            g_cap_text.clear();
            g_cap_draws = 0;
        }
        g_cap_vs = std::strtoull(arg.c_str(), nullptr, 16);
        g_cap_state = 1;
        for (int i = 0; i < 100 && g_cap_state.load() != 3; ++i) Sleep(20);
        return g_cap_state.load() == 3 ? "captured " + std::to_string(g_cap_draws) + " draws to " + file : "ERROR no capture within 2 s";
    }
    if (sub == "skipset") {
        // pso skipset h1,h2,... | pso skipset off
        std::lock_guard lock(g_mutex);
        g_skip_set.clear();
        if (arg != "off") {
            std::stringstream ss(arg);
            std::string item;
            while (std::getline(ss, item, ',')) if (!item.empty()) g_skip_set.insert(std::strtoull(item.c_str(), nullptr, 16));
        }
        g_skip_any = !g_skip_set.empty();
        return "skip set " + std::to_string(g_skip_set.size()) + " vertex shaders";
    }
    if (sub == "ps") {  // pso ps reset | pso ps dump <file> | pso ps off (run 12 item 12a)
        std::string file;
        in >> file;
        if (arg == "reset") {
            {
                std::lock_guard lock(g_mutex);
                g_ps_acc.clear();
            }
            g_ps_acc_on = true;
            return "pixel shader tally reset (on)";
        }
        if (arg == "off") {
            g_ps_acc_on = false;
            return "pixel shader tally off";
        }
        if (arg == "dump" && !file.empty()) {
            FILE* f = nullptr;
            if (fopen_s(&f, file.c_str(), "w") != 0 || !f) return "ERROR cannot write " + file;
            std::lock_guard lock(g_mutex);
            uint64_t total = 0;
            for (auto& [h2, e] : g_ps_acc) {
                std::fprintf(f, "%016llx %016llx %03x %llu\n", static_cast<unsigned long long>(h2), static_cast<unsigned long long>(e.vs), e.writes,
                             static_cast<unsigned long long>(e.draws));
                total += e.draws;
            }
            std::fclose(f);
            return "dumped " + std::to_string(g_ps_acc.size()) + " pixel shaders (" + std::to_string(total) + " draws)";
        }
        std::lock_guard lock(g_mutex);
        return "pixel shader tally " + std::string(g_ps_acc_on.load() ? "on" : "off") + ", " + std::to_string(g_ps_acc.size()) + " pixel shaders";
    }
    if (sub == "list") {
        // all vertex shaders that drew last frame, comma separated
        std::lock_guard lock(g_mutex);
        std::string out;
        for (auto& [h2, n2] : g_last) {
            std::snprintf(buf, sizeof(buf), "%s%016llx", out.empty() ? "" : ",", static_cast<unsigned long long>(h2));
            out += buf;
        }
        return out;
    }
    if (sub == "skip") {
        uint64_t h = arg == "off" || arg.empty() ? 0 : std::strtoull(arg.c_str(), nullptr, 16);
        g_skip = h;
        log::info("[pso] skip draws of vertex shader %016llx", static_cast<unsigned long long>(h));
        std::snprintf(buf, sizeof(buf), "skipping %016llx (%llu skipped so far)", static_cast<unsigned long long>(h),
                      static_cast<unsigned long long>(g_skipped.load()));
        return buf;
    }
    if (sub == "dump" && !arg.empty()) {
        std::lock_guard lock(g_mutex);
        int n = 0;
        for (auto& [h, draws] : g_last) {
            auto it = g_bytecode.find(h);
            if (it == g_bytecode.end()) continue;
            std::snprintf(buf, sizeof(buf), "%s\\vs_%016llx.dxbc", arg.c_str(), static_cast<unsigned long long>(h));
            FILE* f = nullptr;
            if (fopen_s(&f, buf, "wb") == 0 && f) {
                fwrite(it->second.data(), 1, it->second.size(), f);
                fclose(f);
                ++n;
            }
        }
        return "dumped " + std::to_string(n) + " vertex shaders";
    }
    // census
    int n = std::atoi(arg.c_str());
    if (n <= 0) n = 40;
    std::vector<std::pair<uint64_t, uint64_t>> v;
    {
        std::lock_guard lock(g_mutex);
        v.assign(g_last.begin(), g_last.end());
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::snprintf(buf, sizeof(buf), "pipelines: graphics %llu, stream %llu (unhashed %llu); swapped %llu (refused %llu); %zu vertex shaders drew last frame:",
                  static_cast<unsigned long long>(g_graphics.load()), static_cast<unsigned long long>(g_stream.load()),
                  static_cast<unsigned long long>(g_unhashed.load()), static_cast<unsigned long long>(g_swaps.load()),
                  static_cast<unsigned long long>(g_swap_fails.load()), v.size());
    std::string out = buf;
    for (int i = 0; i < n && i < static_cast<int>(v.size()); ++i) {
        std::snprintf(buf, sizeof(buf), " %016llx:%llu", static_cast<unsigned long long>(v[i].first),
                      static_cast<unsigned long long>(v[i].second));
        out += buf;
    }
    return out;
}

// ---- [Holsters] InteractionHighlights=parts (run 29 item 2; pso.h glow): the playback of the part's glow draws
namespace glow {
thread_local bool t_on = false;
namespace {
thread_local Payload t_gp;         // the pending "glow on"'s copy (the playback thread's)
thread_local uint64_t t_used = 0;  // its runs drawn (or dropped) so far
thread_local int t_foreign = 0;    // its draws that were none of its runs (dropped)
thread_local uint64_t t_on_frame = 0;  // state::presents at its "on"
// (the review's safety) a pending glow ends at more draws that are none of its runs than the extra draw can hold (its runs and
// one draw recorded another way), or two presents after its "on": the "off" taken as lost, and that draw the game's again
std::atomic<uint64_t> g_on_marks{0}, g_off_marks{0}, g_bad_marks{0}, g_played{0}, g_unplayed{0}, g_overlap{0}, g_expired{0}, g_drop[kDrops] = {};
std::atomic<int64_t> g_played_us{0};  // log::now_ms() x 1000 at the last glow draw played
std::atomic<uint64_t> g_played_by_place[32] = {};
std::atomic<int64_t> g_played_place_us[32] = {};  // (each place's last, as g_played_us)
const char* const kDropName[kDrops] = {"pre-pass region", "not a run", "no pipeline", "no twin", "not indexed", "indirect"};
void finish_run(int run) {
    t_used |= 1ull << run;
    int n = 0;
    for (uint64_t u = t_used; u; u &= u - 1) ++n;
    if (n >= t_gp.nruns) t_on = false;  // (every run met: nothing after it is the glow's, even without the "off")
}
}  // namespace
void on_cb(void* data) {
    if (t_on) {  // (an "on" without its "off": the new one replaces it)
        g_overlap.fetch_add(1, std::memory_order_relaxed);
        t_on = false;
    }
    if (!data) {
        g_bad_marks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Payload hdr;
    std::memcpy(&hdr, data, offsetof(Payload, runs));  // (the stream's copy: at least the header, its size payload_size(nruns))
    if (hdr.magic != kMagic || hdr.nruns == 0 || hdr.nruns > kMaxRuns) {
        g_bad_marks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::memcpy(&t_gp, data, payload_size(hdr.nruns));
    for (float& c : t_gp.rgb) c = std::isfinite(c) ? std::fmin(64.0f, std::fmax(0.0f, c)) : 0.0f;
    t_used = 0;
    t_foreign = 0;
    t_on_frame = state::presents.load(std::memory_order_relaxed);
    t_on = true;
    g_on_marks.fetch_add(1, std::memory_order_relaxed);
}
void off_cb(void*) {
    g_off_marks.fetch_add(1, std::memory_order_relaxed);
    if (!t_on) return;
    int n = 0;
    for (uint64_t u = t_used; u; u &= u - 1) ++n;
    g_unplayed.fetch_add(static_cast<uint64_t>(t_gp.nruns > n ? t_gp.nruns - n : 0), std::memory_order_relaxed);
    t_on = false;
}
void drop(int why) { g_drop[why >= 0 && why < kDrops ? why : 0].fetch_add(1, std::memory_order_relaxed); }
bool stale_on() { return state::presents.load(std::memory_order_relaxed) - t_on_frame >= 2; }
bool expire() {
    t_on = false;
    g_expired.fetch_add(1, std::memory_order_relaxed);
    return false;
}
bool foreign(int why, UINT count) {
    if (stale_on()) return expire();  // (first: a stale "on" never drops another frame's draws, the review)
    int used = 0;
    for (uint64_t u = t_used; u; u &= u - 1) ++used;
    bool run_count = false;  // (an indexed draw of a run's size not drawn yet: only the extra draw's, dropped)
    for (int i = 0; count && i < t_gp.nruns && i < kMaxRuns; ++i) run_count = run_count || (!((t_used >> i) & 1) && t_gp.runs[i].count == count);
    if (!run_count && ++t_foreign > t_gp.nruns - used + 1) return expire();  // (more than the runs left and one other draw)
    drop(why);
    return true;
}
void list_reset(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    if (!g_glow_track.load(std::memory_order_relaxed)) return;
    t_gcur_cl = cl;
    t_gcur_ps = ps;
}
bool draw_indexed(ID3D12GraphicsCommandList* cl, UINT count, UINT inst, UINT start, INT base, UINT first_inst, DrawIndexed_t draw) {
    if (stale_on()) return expire();
    int run = -1;
    for (int i = 0; i < t_gp.nruns && i < kMaxRuns; ++i)
        if (t_gp.runs[i].count == count && t_gp.runs[i].start == start && !((t_used >> i) & 1)) {
            run = i;
            break;
        }
    if (run < 0 || inst != 1) return foreign(kDropNoRun, count);  // (not one of the part's runs: another packet of the extra draw)
    if (prepass::active()) {  // ([Stereo] PrepassArrays: the pre-pass's six targets, never a glow there)
        drop(kDropRegion);
        finish_run(run);
        return true;
    }
    ID3D12PipelineState* game = t_gcur_cl == cl ? t_gcur_ps : nullptr;
    ID3D12PipelineState* twin = game ? glow_lookup(game) : nullptr;
    if (!twin || !o_SetPipelineState || !draw) {  // (a shadow's, the pre-pass's, or a pipeline without its twin)
        drop(game ? kDropNoTwin : kDropNoPipeline);
        finish_run(run);
        return true;
    }
    {
        d3d::InOriginal orig(d3d::kNPipeline);
        o_SetPipelineState(cl, twin);
    }
    const float bf[4] = {t_gp.rgb[0], t_gp.rgb[1], t_gp.rgb[2], 1.0f};
    cl->OMSetBlendFactor(bf);
    {
        d3d::InOriginal orig(d3d::kNDrawIndexed);
        draw(cl, count, inst, start, base, first_inst);
    }
    static const float kOne[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    cl->OMSetBlendFactor(kOne);
    {
        d3d::InOriginal orig(d3d::kNPipeline);
        o_SetPipelineState(cl, game);
    }
    g_played.fetch_add(1, std::memory_order_relaxed);
    g_played_by_place[t_gp.place >= 0 && t_gp.place < 32 ? t_gp.place : 31].fetch_add(1, std::memory_order_relaxed);
    const int64_t now_us = static_cast<int64_t>(log::now_ms() * 1000.0);
    if (t_gp.eye) g_played_us.store(now_us, std::memory_order_relaxed);  // (live(): the eyes' passes only, the review)
    if (t_gp.eye) g_played_place_us[t_gp.place >= 0 && t_gp.place < 32 ? t_gp.place : 31].store(now_us, std::memory_order_relaxed);
    finish_run(run);
    return true;
}
void want(bool on) {
    if (g_glow_want.exchange(on) == on) return;
    log::info("[pso] part glow: %s (%llu weapon pipelines kept, %d twins)", on ? "wanted: the kept pipelines' glow twins made on a worker" : "not wanted",
              static_cast<unsigned long long>(g_glow_kept_n.load()), g_glow_twins.load());
#if RDRVR_HAVE_PART_GLOW
    if (on && g_glow_kept_n.load(std::memory_order_relaxed)) glow_kick();  // (none kept yet: each is made at its creation)
#else
    if (on) log::warn("[pso] part glow: this build has no glow pixel shader (dxc was not found at its build): the gun's parts cannot glow");
#endif
}
bool live(int place) {
    const int64_t t = place < 0 ? g_played_us.load(std::memory_order_relaxed) : place < 32 ? g_played_place_us[place].load(std::memory_order_relaxed) : 0;
    return t > 0 && static_cast<int64_t>(log::now_ms() * 1000.0) - t < 250000;
}
int twins() { return g_glow_twins.load(std::memory_order_relaxed); }
void set_bias(int units) {
    units = units < 0 ? 0 : units > 4096 ? 4096 : units;
    if (g_glow_bias.exchange(units) == units) return;
    g_glow_gen.fetch_add(1, std::memory_order_relaxed);
    log::info("[pso] part glow: depth bias %d (the twins made again)", units);
    if (g_glow_want.load(std::memory_order_relaxed)) glow_kick();
}
std::string status() {
    char b[1600];
    const int64_t t = g_played_us.load(std::memory_order_relaxed);
    const double ago = t > 0 ? log::now_ms() - static_cast<double>(t) / 1000.0 : -1.0;
#if RDRVR_HAVE_PART_GLOW
    const char* ps = "built in";
#else
    const char* ps = "missing";
#endif
    int k = std::snprintf(b, sizeof(b),
                          "glow twins %d (made %llu, view-instanced %llu, refused %llu; build %.1f ms) | matched PSWeapon %llu PSWeaponGold %llu, kept %llu "
                          "(over %llu), unknown weapon PS %llu, weapon VS in a stream %llu, forgotten %llu, game pipelines blending by the factor %llu | bias "
                          "%d gen %u want %d ps %s | depth funcs",
                          g_glow_twins.load(), static_cast<unsigned long long>(g_glow_made.load()), static_cast<unsigned long long>(g_glow_made_vi.load()),
                          static_cast<unsigned long long>(g_glow_failed.load()), static_cast<double>(g_glow_build_us.load()) / 1000.0,
                          static_cast<unsigned long long>(g_glow_matched[0].load()), static_cast<unsigned long long>(g_glow_matched[1].load()),
                          static_cast<unsigned long long>(g_glow_kept_n.load()), static_cast<unsigned long long>(g_glow_kept_over.load()),
                          static_cast<unsigned long long>(g_glow_unknown_ps.load()), static_cast<unsigned long long>(g_glow_stream_vs.load()),
                          static_cast<unsigned long long>(g_glow_forgot.load()), static_cast<unsigned long long>(g_glow_bf_users.load()), g_glow_bias.load(), g_glow_gen.load(), g_glow_want.load() ? 1 : 0, ps);
    for (int f = 0; f < 9 && k > 0 && static_cast<size_t>(k) < sizeof(b); ++f)
        if (const uint64_t n = g_glow_func[f].load()) k += std::snprintf(b + k, sizeof(b) - k, " %d:%llu", f, static_cast<unsigned long long>(n));
    if (k > 0 && static_cast<size_t>(k) < sizeof(b))
        k += std::snprintf(b + k, sizeof(b) - k, " | marks on %llu off %llu bad %llu overlap %llu expired %llu | played %llu (last %.0f ms ago), runs unplayed %llu | dropped",
                           static_cast<unsigned long long>(g_on_marks.load()), static_cast<unsigned long long>(g_off_marks.load()),
                           static_cast<unsigned long long>(g_bad_marks.load()), static_cast<unsigned long long>(g_overlap.load()),
                           static_cast<unsigned long long>(g_expired.load()), static_cast<unsigned long long>(g_played.load()), ago, static_cast<unsigned long long>(g_unplayed.load()));
    for (int i = 0; i < kDrops && k > 0 && static_cast<size_t>(k) < sizeof(b); ++i)
        k += std::snprintf(b + k, sizeof(b) - k, " %s %llu%s", kDropName[i], static_cast<unsigned long long>(g_drop[i].load()), i + 1 < kDrops ? "," : "");
    if (k > 0 && static_cast<size_t>(k) < sizeof(b)) k += std::snprintf(b + k, sizeof(b) - k, " | played by place");
    for (int i = 0; i < 32 && k > 0 && static_cast<size_t>(k) < sizeof(b); ++i)
        if (const uint64_t n = g_played_by_place[i].load()) k += std::snprintf(b + k, sizeof(b) - k, " %d:%llu", i, static_cast<unsigned long long>(n));
    return b;
}
}  // namespace glow

}  // namespace rdrvr::pso
