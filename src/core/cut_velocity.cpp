#include "core/cut_velocity.h"

#include <windows.h>
#include <xmmintrin.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/post_target.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/eye_shape.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/prepass_arrays.h"
#include "core/prevworld_mirror.h"
#include "core/rec_state.h"
#include "core/state.h"

// [Stereo] PrepassWalkCut's stub switch (prepass_arrays.cpp defines them): set by walk_cut_decide between the passes and
// cleared at the frame's end, so on the render thread in pass 2 "the cut decided for this frame", before the stub fires
extern "C" volatile unsigned char rdrvr_prepass_cut;
extern "C" volatile unsigned long rdrvr_prepass_cut_tid;

// (run 22 item 3, research\run22\d4band.md; test only) [Stereo] PrepassEye2Collector's "where2" diagnostic (eye2_collector.cpp
// defines them): the H2 step's marker at a cut walk's end, and whether its bind tap holds the kBindTapCutVel slot
namespace rdrvr::eye2_collector {
void cut_walk_end_mark();
bool where2_on();
}  // namespace rdrvr::eye2_collector

namespace rdrvr::cut_velocity {
namespace {
using anchors::Id;

// PostFx fields (taa.cpp, research\native-taa-study.md; FUN_140873920 and FUN_1408728f0 in Ghidra, 2026-10-21)
constexpr size_t kTech = 0x868, kTaaEffect = 0x128, kVarNdcToPrev = 0x3c4, kVarTexel = 0x3c8, kVarDepth = 0x3cc, kNdcToPrev = 0x4b0;
// the effect (FUN_140151020 [V]): +0x00 its values (entry h-1 of 16 bytes, +8 the pointer), +0x10 the owner (+0x20 the
// variable table, entries of 0x48, +0x20 the member index into the CB's layout), +0x38 the constant buffer object
constexpr size_t kEffValues = 0x00, kEffOwner = 0x10, kEffCb = 0x38, kOwnerVars = 0x20, kVarStride = 0x48, kVarMember = 0x20;
// the CB object (FUN_140ec8bd0 / FUN_140ecc150 [V]): +0x00 storage, +0x08 layout (+0x00 the member table, entries of 0x14:
// +0 offset, +8 size; +0x14 the declared size), +0x10 mapped size, +0x18 mapped pointer, +0x20 a byte, +0x24 the upload
// stamp, +0x28 the GPU address of its last upload
constexpr size_t kCbStorage = 0x00, kCbLayout = 0x08, kCbMapSize = 0x10, kCbMapped = 0x18, kCbFlag = 0x20, kCbStamp = 0x24,
                 kCbGpu = 0x28, kLayoutSize = 0x14, kMemberStride = 0x14, kMemberSize = 0x08;

using Velocity_t = void (*)(void* postfx);
using SetVar_t = void (*)(void* owner, void* eff, int handle, const void* data, int size, int count);
Velocity_t o_velocity = nullptr;

std::atomic<int> g_mode{0};  // 0 off, 1 d1, 2 d2, 3 d3, 4 d4, 5 d5, 6 d6
// (run 22 item 3, research\run22\d4band.md; "prepass cutvel quiet N", test only, 0 by default) d1's observing parts silenced
// while the mode stays, to tell which of them goes with eye 2's collector band: 1 the velocity pass's op-0x4a markers,
// 2 the PrevWorld sampling (the detour then only calls the original), 4 the velocity pass's reads (read_sample, vp_sample,
// read_after, publish), 8 lb_sample at the cut walk's end. d2-d6's own actions are not affected.
constexpr int kQuietMarkers = 1, kQuietPrevWorld = 2, kQuietReads = 4, kQuietWalkEnd = 8;
std::atomic<int> g_quiet{0};
std::atomic<bool> g_installed{false};

template <class T>
bool rd(uintptr_t a, T* out) {
    if (!a) return false;
    __try {
        *out = *reinterpret_cast<const T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool raw(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool wr(uintptr_t a, T v) {
    if (!a) return false;
    __try {
        *reinterpret_cast<T*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- D1: one pass's view of the velocity pass's constants (the render thread writes, the status reads)
struct Sample {
    uint64_t frame = 0;
    int tech = 0, handle = 0, member = -1, off = -1, msize = 0, map_size = 0, cb_stamp = 0, stamp = 0;
    uintptr_t eff = 0, cb = 0, storage = 0, mapped = 0;
    uint64_t gpu = 0;
    bool have_local = false, have_cbval = false, have_stamp = false;
    // the depth texture variable (PostFx +0x3cc). Before the call: the effect's RT pointer and that RT's +0x2c byte, and
    // PostFx +0xf8 (the RT the pass gives it): FUN_140873920 sets the variable again (FUN_14014b100) when the pointer
    // differs or the byte is set. After the call: the effect's RT pointer, and the bindless index and sampler
    // FUN_14014b100 left in the CB at its member, and TexelSize (PostFx +0x3c8, set inside the call) as in the CB.
    uintptr_t depth_rt_pre = 0, f8_pre = 0, depth_rt = 0, f8 = 0;
    uint32_t depth_dirty_pre = 0;
    uint32_t depth_cb[2] = {};
    float texel[4] = {};
    bool have_after = false;
    float ndc[16] = {}, local[16] = {}, cbval[16] = {};
};
enum Klass { kPass1, kPass2, kPass2Cut, kKlasses };
const char* const kKlassName[kKlasses] = {"pass 1", "pass 2 not cut", "pass 2 cut"};
struct Slot {
    std::atomic<uint32_t> seq{0};  // odd while being written (single writer: the render thread)
    Sample s;
    std::atomic<uint64_t> n{0}, unread{0}, upload_due{0}, cpu_ok{0}, local_ok{0}, gpu_as_p1{0}, unmapped{0}, set_again{0}, rt_is_f8{0}, after_unread{0};
};
Slot g_slot[kKlasses];
uint64_t g_p1_frame = ~0ull, g_p1_gpu = 0;  // the render thread: this frame's pass-1 CB address at its velocity pass
std::atomic<uint64_t> g_calls{0}, g_d2{0}, g_d2_refused{0}, g_d2_tech{0}, g_d2_unmapped{0}, g_d2_size{0}, g_d3{0}, g_d3_refused{0}, g_d3_absent{0};
// the size guard (velocity.md 4.2): rdr2_taa's CB layout's declared size (+0x14) as first read (the CB is made after
// install, at the effect's first use: its first sample stands for "install-time"); D2 refuses any other size
std::atomic<int> g_cb_size0{0};
inline void bump(std::atomic<uint64_t>& a) { a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

bool read_sample(uintptr_t p, Sample* s) {
    s->frame = state::presents.load(std::memory_order_relaxed);
    if (!rd(p + kTech, &s->tech) || !raw(p + kNdcToPrev, s->ndc, sizeof(s->ndc)) || !rd(p + kVarNdcToPrev, &s->handle) || !rd(p + kTaaEffect, &s->eff) ||
        !s->eff)
        return false;
    s->have_stamp = anchors::present(Id::RecorderStamp) && rd(anchors::addr(Id::RecorderStamp), &s->stamp);
    if (s->handle <= 0 || s->handle > 4096) return true;  // (the variable not resolved: the rest left empty)
    uintptr_t values = 0, local = 0, owner = 0, vars = 0;
    s->have_local = rd(s->eff + kEffValues, &values) && rd(values + static_cast<uintptr_t>(s->handle) * 16 - 8, &local) &&
                    raw(local, s->local, sizeof(s->local));
    if (!rd(s->eff + kEffCb, &s->cb) || !s->cb) return true;
    uintptr_t layout = 0, members = 0;
    int declared = 0;
    if (!g_cb_size0.load(std::memory_order_relaxed) && rd(s->cb + kCbLayout, &layout) && rd(layout + kLayoutSize, &declared) && declared > 0 &&
        declared <= 0x10000)
        g_cb_size0.store(declared, std::memory_order_relaxed);
    rd(s->cb + kCbStorage, &s->storage);
    rd(s->cb + kCbMapSize, &s->map_size);
    rd(s->cb + kCbMapped, &s->mapped);
    rd(s->cb + kCbStamp, &s->cb_stamp);
    rd(s->cb + kCbGpu, &s->gpu);
    if (rd(s->eff + kEffOwner, &owner) && rd(owner + kOwnerVars, &vars) &&
        rd(vars + static_cast<uintptr_t>(s->handle - 1) * kVarStride + kVarMember, &s->member) && s->member >= 0 && s->member < 4096 &&
        rd(s->cb + kCbLayout, &layout) && rd(layout, &members) &&
        rd(members + static_cast<uintptr_t>(s->member) * kMemberStride, &s->off) &&
        rd(members + static_cast<uintptr_t>(s->member) * kMemberStride + kMemberSize, &s->msize) && s->off >= 0 && s->off < 0x10000 &&
        s->msize >= 64 && s->storage)
        s->have_cbval = raw(s->storage + static_cast<uintptr_t>(s->off), s->cbval, sizeof(s->cbval));
    // the depth texture variable before the call (FUN_140873920 decides there whether to set it again)
    int hd = 0;
    rd(p + 0xf8, &s->f8_pre);
    if (rd(p + kVarDepth, &hd) && hd > 0 && hd <= 4096 && rd(values + static_cast<uintptr_t>(hd) * 16 - 8, &s->depth_rt_pre) && s->depth_rt_pre) {
        uint8_t dirty = 0;
        if (rd(s->depth_rt_pre + 0x2c, &dirty)) s->depth_dirty_pre = dirty;
    }
    return true;
}

// After the call (FUN_140873920 sets TexelSize and, when it must, the depth variable inside it [V]): the same reads
bool read_after(uintptr_t p, Sample* s) {
    uintptr_t values = 0, owner = 0, vars = 0, cb = 0, storage = 0, layout = 0, members = 0;
    if (!s->eff || !rd(s->eff + kEffValues, &values) || !rd(s->eff + kEffOwner, &owner) || !rd(owner + kOwnerVars, &vars) || !vars ||
        !rd(s->eff + kEffCb, &cb) || !cb || !rd(cb + kCbStorage, &storage) || !storage || !rd(cb + kCbLayout, &layout) || !rd(layout, &members) ||
        !members)
        return false;
    auto member_off = [&](int h, int* off) {
        int mem = -1;
        return h > 0 && h <= 4096 && rd(vars + static_cast<uintptr_t>(h - 1) * kVarStride + kVarMember, &mem) && mem >= 0 && mem < 4096 &&
               rd(members + static_cast<uintptr_t>(mem) * kMemberStride, off) && *off >= 0 && *off < 0x10000;
    };
    int hd = 0, ht = 0, off = -1;
    rd(p + 0xf8, &s->f8);
    bool ok = rd(p + kVarDepth, &hd) && hd > 0 && hd <= 4096 && rd(values + static_cast<uintptr_t>(hd) * 16 - 8, &s->depth_rt);
    ok = ok && member_off(hd, &off) && raw(storage + static_cast<uintptr_t>(off), s->depth_cb, sizeof(s->depth_cb));
    ok = ok && rd(p + kVarTexel, &ht) && member_off(ht, &off) && raw(storage + static_cast<uintptr_t>(off), s->texel, sizeof(s->texel));
    s->have_after = ok;
    return ok;
}

void publish(const Sample& s, int k) {  // (read_sample before the call, read_after after it)
    Slot& sl = g_slot[k];
    bump(sl.n);
    if (!s.have_after) bump(sl.after_unread);
    if (s.depth_rt_pre != s.f8_pre || s.depth_dirty_pre) bump(sl.set_again);  // (FUN_140873920's condition for FUN_14014b100)
    if (s.have_after && s.depth_rt == s.f8) bump(sl.rt_is_f8);
    if (s.have_stamp && s.cb && s.cb_stamp != s.stamp) bump(sl.upload_due);  // (the draw's commit uploads; else it re-binds +0x28)
    if (s.have_cbval && !std::memcmp(s.cbval, s.ndc, sizeof(s.ndc))) bump(sl.cpu_ok);
    if (s.have_local && !std::memcmp(s.local, s.ndc, sizeof(s.ndc))) bump(sl.local_ok);
    if (s.cb && !s.mapped) bump(sl.unmapped);
    if (k == kPass1) {
        g_p1_frame = s.frame;
        g_p1_gpu = s.gpu;
    } else if (g_p1_frame == s.frame && s.gpu == g_p1_gpu && s.have_stamp && s.cb_stamp == s.stamp) {
        bump(sl.gpu_as_p1);  // pass 2's draw will bind pass 1's upload (no upload due, the same address)
    }
    sl.seq.fetch_add(1, std::memory_order_acq_rel);
    sl.s = s;
    sl.seq.fetch_add(1, std::memory_order_acq_rel);
}

// ---- D2: rdr2_taa's CB re-mapped (FUN_140ec8bd0(cb, -1, 0), as FUN_140151020 calls it) and NDCToPrevNDC set again
void d2(uintptr_t p) {
    int tech = 0, handle = 0;
    uintptr_t eff = 0, owner = 0, cb = 0;
    if (!rd(p + kTech, &tech) || tech < 2 || tech > 5) {  // (the velocity pass draws for 2-5 or motion blur; FXAA: nothing)
        bump(g_d2_tech);
        return;
    }
    if (!rd(p + kVarNdcToPrev, &handle) || handle <= 0 || handle > 4096 || !rd(p + kTaaEffect, &eff) || !eff || !rd(eff + kEffOwner, &owner) ||
        !owner || !rd(eff + kEffCb, &cb)) {
        bump(g_d2_refused);
        return;
    }
    if (cb) {
        uintptr_t storage = 0, mapped = 0, layout = 0;
        int size = 0;
        if (!rd(cb + kCbStorage, &storage) || !storage || !rd(cb + kCbMapped, &mapped) || !rd(cb + kCbLayout, &layout) ||
            !rd(layout + kLayoutSize, &size) || size <= 0 || size > 0x10000) {
            bump(g_d2_refused);
            return;
        }
        int size0 = g_cb_size0.load(std::memory_order_relaxed);
        if (!size0) g_cb_size0.store(size0 = size, std::memory_order_relaxed);
        if (size != size0) {  // (the CB made again with another layout, or not the one first read: not touched)
            bump(g_d2_size);
            log::limited("cutvel.size", 5, "[cutvel] rdr2_taa's CB declares %d bytes, first read %d: not re-mapped", size, size0);
            return;
        }
        if (!mapped) {
            bump(g_d2_unmapped);  // (EffectSetVar maps it itself: the stamp -1 there)
        } else if (mapped != storage) {
            bump(g_d2_refused);  // (mapped elsewhere than its storage: not as the analysed build does it)
            log::limited("cutvel.map", 5, "[cutvel] rdr2_taa's CB mapped at %p, not its storage %p: not re-mapped", reinterpret_cast<void*>(mapped),
                         reinterpret_cast<void*>(storage));
            return;
        } else if (!(wr<uintptr_t>(cb + kCbMapped, storage) && wr<int>(cb + kCbMapSize, size) && wr<int>(cb + kCbStamp, -1) &&
                     wr<uint8_t>(cb + kCbFlag, 0))) {
            bump(g_d2_refused);
            return;
        }
    }
    float m[16];
    if (!raw(p + kNdcToPrev, m, sizeof(m))) {
        bump(g_d2_refused);
        return;
    }
    __try {
        reinterpret_cast<SetVar_t>(anchors::addr(Id::EffectSetVar))(reinterpret_cast<void*>(owner), reinterpret_cast<void*>(eff), handle, m, 0x40, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bump(g_d2_refused);
        return;
    }
    bump(g_d2);
}

// ---- d1 at playback (run 19 item 2, round 2): op-0x4a markers recorded around the velocity pass on the render thread
// (DeferredAppend with no payload: one callback per class and probe bit) tell the playback thread, in stream order,
// where its packets play:
// the draws played between them (d3d::thread_draws on that thread), whether a predication was set on the list, and
// whether a pre-pass region was open, per class. The probe ("prepass cutvel probe <prefix> [cut|nocut|pass1]", a one-shot, test
// only) copies a centre crop of the game's Velocity RT at the first bind after the begin marker (the velocity pass's own
// bind: the content the pass draws over, after the handover) and at the first bind after the end marker (0x869740's MRT
// bind: what the pass left), and slice 1 of the mod's velocity array at the first (what the handover copied from), into
// a readback buffer, written at the second frame end after as RDB1 files <prefix>-pre/-post/-slice1.rdb.
std::atomic<uint64_t> g_pb_runs[kKlasses], g_pb_draws[kKlasses], g_pb_nodraw[kKlasses], g_pb_pred[kKlasses], g_pb_region[kKlasses],
    g_pb_unpaired{0};
uint64_t g_pb_draws0 = 0;  // the playback thread
int g_pb_k = -1;
std::atomic<DWORD> g_pb_tid{0};  // the playback thread the last begin marker played on (the probe's tap checks it)
using Append_t = void (*)(void* recorder, void* fn, const void* data, uint64_t size);
bool append_marker(void (*fn)(void*)) {  // (no payload: prepass_arrays' walk markers' form)
    uintptr_t dev = 0, hrec = 0, rec = 0, vt = 0;
    if (!rd(anchors::addr(Id::D3dDeviceSingleton), &dev) || !rd(dev + 0x58, &hrec) || !rd(hrec + 0x128, &rec) || !rd(rec, &vt) ||
        vt != anchors::addr(Id::RecorderVtbl))
        return false;
    reinterpret_cast<Append_t>(anchors::addr(Id::DeferredAppend))(reinterpret_cast<void*>(rec), reinterpret_cast<void*>(fn), nullptr, 0);
    return true;
}

// the probe (the command thread arms and writes; the render thread marks; the playback thread copies)
// 0 pre (the game's RT before the pass), 1 post (after it), 2 the array's slice 1 before it; then (round 3) 3-5 the RT at
// the 2nd-4th binds that include it after the velocity pass (0x869740's MRT is the 1st: segments e, g, i, k of p3.md 1
// rebind it after each refraction copy), 6 the RT at the post output's first bind (the tonemap, after the upscaler)
constexpr int kProbeSlots = 7;
const char* const kProbeName[kProbeSlots] = {"pre", "post", "slice1", "mrt2", "mrt3", "mrt4", "final"};
// (item 8f0, test only: "prepass cutvel fsc <prefix> [cut|nocut]") the same probe on the game's FullScreenCopy (PostFx +0x10): at
// 0x869740's MRT bind (the first bind after the velocity pass's end marker) FSC is copied (seed), then cleared to the sentinel; dr
// is FSC at the Depth Resolve copy (segment e's end), mrt2-mrt4 FSC at the later binds that include it (the MRT rebound after each
// refraction blit, FSC's first sampled reader, which writes only the Refraction RT), final at the post output's first bind. The
// sentinel is finite, positive and exact in half floats (a one-frame blotch where it survives: nothing persists)
const char* const kFscName[kProbeSlots] = {"dr", "seed", "unused", "mrt2", "mrt3", "mrt4", "final"};
constexpr float kFscSentinel[4] = {1.0009765625f, 2.001953125f, 0.50048828125f, 1.0f};  // (half 0x3c01, 0x4001, 0x3801, 0x3c00)
std::atomic<bool> g_probe_fsc{false};  // the probe armed in the FSC mode (set by the command thread before the probe arms)
bool g_fsc_cleared = false, g_fsc_unbound = false, g_fsc_state_unknown = false;  // (the playback thread, at the MRT bind)
int g_probe_mrt = 0;            // the playback thread: binds including the RT since post (post's own counted when it has it)
bool g_probe_post_vel = false;  // post's bind included the RT
uint64_t g_probe_out_rtv[8] = {};  // the post output's render-target views
int g_probe_out_n = 0;
std::atomic<int> g_probe{0};   // 0 idle, 1 armed (the render thread marks the next matching pass 2), 2 marked
std::atomic<int> g_pp{0};      // the playback side: 0 none, 1 at the pass's begin (copy at the next bind), 11 pre done, 2 at its end, 3 done
std::atomic<int> g_probe_ends{0};  // frame ends since done (the files at the second)
int g_probe_cut = 1;           // 1 a cut frame's pass 2, 0 a pass 2 not cut
int g_probe_pass1 = 0;         // (run 25, test only: "prepass cutvel probe <prefix> pass1") 1: the next pass 1 instead (g_probe_cut unused)
bool g_probe_ok[kProbeSlots] = {}, g_probe_first_vel = false;
bool g_probe_pred[kProbeSlots] = {};  // the slot's copy refused: a predication set on the list (a predicated copy does nothing)
bool g_probe_pre_region = false;      // pre taken while a pre-pass region was open (before the handover)
bool g_probe_arr_gone = false;        // slice1 refused: the array was made again since the probe armed
ID3D12Resource* g_probe_vel = nullptr;  // the game's Velocity RT (watched)
ID3D12Resource* g_probe_arr = nullptr;  // the mod's velocity array (RENDER_TARGET at rest, outside a region)
ID3D12Resource* g_probe_rb = nullptr;   // readback, kProbeSlots footprints
uint64_t g_probe_rb_size = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_probe_fp{};
UINT64 g_probe_row = 0, g_probe_slot_bytes = 0;
D3D12_BOX g_probe_box{};
uint32_t g_probe_w = 0, g_probe_h = 0, g_probe_fmt = 0;
uint64_t g_probe_frame = 0;
ID3D12Fence* g_probe_fence = nullptr;
uint64_t g_probe_fence_value = 0;
HANDLE g_probe_done = nullptr, g_probe_fence_event = nullptr;
std::string g_probe_prefix, g_probe_result;

void probe_copy(ID3D12GraphicsCommandList* cl, int slot, ID3D12Resource* res, UINT sub, D3D12_RESOURCE_STATES state) {
    if (!res || !g_probe_rb) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {res, sub, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    const bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (transition) d3d::raw_barrier(cl, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = g_probe_rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = g_probe_fp;
    dl.PlacedFootprint.Offset = static_cast<UINT64>(slot) * g_probe_slot_bytes;
    sl.pResource = res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = sub;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, &g_probe_box);
    if (transition) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        d3d::raw_barrier(cl, 1, &b);
    }
    g_probe_ok[slot] = true;
}
bool probe_vel_state(ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES* st) {
    ID3D12GraphicsCommandList* wcl = nullptr;
    uint32_t s = 0;
    if (!d3d::watched_state(g_probe_vel, &wcl, &s) || wcl != cl) return false;
    *st = static_cast<D3D12_RESOURCE_STATES>(s);
    return true;
}
void probe_tap_later(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single);
void probe_tap(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single, const D3D12_CPU_DESCRIPTOR_HANDLE*) {
    const int pp = g_pp.load(std::memory_order_acquire);
    if (pp != 1 && pp != 2 && pp != 4) return;
    if (GetCurrentThreadId() != g_pb_tid.load(std::memory_order_relaxed)) return;  // (another thread's list: the mod's own, say)
    if (pp == 4) {
        probe_tap_later(cl, n, rts, single);
        return;
    }
    const bool pred = d3d::thread_predicated();  // (a predicated copy does nothing: those slots NOT MEASURED)
    D3D12_RESOURCE_STATES st{};
    if (pp == 1 && g_probe_fsc.load(std::memory_order_relaxed)) {  // (the FSC mode: nothing at the velocity pass)
        g_pp.store(11, std::memory_order_release);
        return;
    }
    if (pp == 2 && g_probe_fsc.load(std::memory_order_relaxed)) {  // (item 8f0) 0x869740's MRT bind: the seed copied, FSC cleared
        const D3D12_CPU_DESCRIPTOR_HANDLE* fsc_rtv = nullptr;
        for (unsigned i = 0; rts && i < (single ? 1u : n) && !fsc_rtv; ++i)
            if (d3d::view_resource(rts[i].ptr) == g_probe_vel) fsc_rtv = &rts[i];  // (single: one contiguous range, the first view)
        g_probe_post_vel = fsc_rtv != nullptr;
        g_probe_mrt = 1;  // (the FSC mode: the later binds are mrt2.., slot 2 is never used)
        if (!fsc_rtv) {
            g_fsc_unbound = true;  // (not the MRT with FSC: nothing cleared, NOT MEASURED)
        } else if (pred) {
            g_probe_pred[1] = true;  // (a predicated clear or copy does nothing: never under one)
        } else if (!probe_vel_state(cl, &st)) {
            g_fsc_state_unknown = true;  // (no transition of FSC seen in this list: its state not guessed)
        } else {
            probe_copy(cl, 1, g_probe_vel, 0, st);  // the seed (back in st after)
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {g_probe_vel, 0, st, D3D12_RESOURCE_STATE_RENDER_TARGET};
            const bool tr = st != D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (tr) d3d::raw_barrier(cl, 1, &b);
            d3d::raw_clear_rtv(cl, fsc_rtv, kFscSentinel);
            if (tr) {
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                d3d::raw_barrier(cl, 1, &b);
            }
            g_fsc_cleared = true;
        }
        g_pp.store(4, std::memory_order_release);  // (on to the later binds: mrt2.., final)
        return;
    }
    if (pp == 1) {  // the velocity pass's own bind: before its draw
        g_probe_first_vel = n >= 1 && rts && d3d::view_resource(rts[0].ptr) == g_probe_vel;
        g_probe_pre_region = prepass::active();
        if (pred) g_probe_pred[0] = g_probe_pred[2] = true;
        else if (probe_vel_state(cl, &st)) probe_copy(cl, 0, g_probe_vel, 0, st);
        if (g_probe_arr && g_probe_arr != prepass::velocity_array()) g_probe_arr_gone = true;  // (made again: never touched)
        else if (g_probe_arr && !pred && !prepass::active())
            probe_copy(cl, 2, g_probe_arr, 1, D3D12_RESOURCE_STATE_RENDER_TARGET);  // (slice 1, mip 0)
        g_pp.store(11, std::memory_order_release);
        return;
    }
    if (pred) g_probe_pred[1] = true;
    else if (probe_vel_state(cl, &st)) probe_copy(cl, 1, g_probe_vel, 0, st);  // the next bind after the pass: what it left
    g_probe_post_vel = false;  // (post's bind includes the RT: 0x869740's MRT is then the 1st of the binds counted)
    for (unsigned i = 0; rts && i < (single ? 1u : n) && !g_probe_post_vel; ++i) g_probe_post_vel = d3d::view_resource(rts[i].ptr) == g_probe_vel;
    g_probe_mrt = g_probe_post_vel ? 1 : 0;
    g_pp.store(4, std::memory_order_release);  // (round 3: on to the later binds)
}
// round 3: the binds after the pass until the post output's (pp 4), on the same thread
void probe_tap_later(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single) {
    if (!rts || !n) return;
    bool out = false, vel = false;
    for (int i = 0; i < g_probe_out_n; ++i) out = out || rts[0].ptr == g_probe_out_rtv[i];
    for (unsigned i = 0; i < (single ? 1u : n) && !vel; ++i) vel = d3d::view_resource(rts[i].ptr) == g_probe_vel;
    if (!out && !vel) return;
    const int slot = out ? 6 : g_probe_mrt < 4 ? 2 + g_probe_mrt : -1;
    if (vel) ++g_probe_mrt;
    if (slot >= 0 && !g_probe_ok[slot]) {
        D3D12_RESOURCE_STATES st{};
        if (d3d::thread_predicated()) g_probe_pred[slot] = true;
        else if (probe_vel_state(cl, &st)) probe_copy(cl, slot, g_probe_vel, 0, st);
    }
    if (out) g_pp.store(5, std::memory_order_release);  // (the post's first bind: done)
}

void mark_begin(int k, bool probe) {
    if (g_pb_k >= 0) bump(g_pb_unpaired);
    g_pb_k = k;
    g_pb_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_pb_draws0 = d3d::thread_draws();
    bump(g_pb_runs[k]);
    if (d3d::thread_predicated()) bump(g_pb_pred[k]);
    if (prepass::active()) bump(g_pb_region[k]);
    if (probe) g_pp.store(1, std::memory_order_release);
}
void mark_end(int k, bool probe) {
    if (g_pb_k != k || g_pb_tid.load(std::memory_order_relaxed) != GetCurrentThreadId()) {
        bump(g_pb_unpaired);
    } else {
        const uint64_t n = d3d::thread_draws() - g_pb_draws0;
        g_pb_draws[k].store(g_pb_draws[k].load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
        if (!n) bump(g_pb_nodraw[k]);
    }
    g_pb_k = -1;
    if (probe) {
        int expect = 11;
        if (!g_pp.compare_exchange_strong(expect, 2)) {  // (the pass's bind never came: nothing before it copied)
            expect = 1;
            g_pp.compare_exchange_strong(expect, 2);
        }
    }
}

// one callback per class and probe bit (op-0x4a callbacks with no payload)
template <int K, bool P>
void mark_begin_cb(void*) { mark_begin(K, P); }
template <int K, bool P>
void mark_end_cb(void*) { mark_end(K, P); }
using Cb_t = void (*)(void*);
const Cb_t kBegin[kKlasses][2] = {{&mark_begin_cb<0, false>, &mark_begin_cb<0, true>},
                                  {&mark_begin_cb<1, false>, &mark_begin_cb<1, true>},
                                  {&mark_begin_cb<2, false>, &mark_begin_cb<2, true>}};
const Cb_t kEnd[kKlasses][2] = {{&mark_end_cb<0, false>, &mark_end_cb<0, true>},
                                {&mark_end_cb<1, false>, &mark_end_cb<1, true>},
                                {&mark_end_cb<2, false>, &mark_end_cb<2, true>}};

ID3D12Resource* find_vel(char* postfx) {
    void* obj = *reinterpret_cast<void**>(postfx + 0x3f0);
    char where[64] = "?";
    const void* found = obj ? d3d::resource_in_object(obj, 0x100, "Velocity RT", where, sizeof(where)) : nullptr;
    if (!found) found = d3d::unique_resource("Velocity RT");
    return static_cast<ID3D12Resource*>(const_cast<void*>(found));
}

ID3D12Resource* find_fsc(char* postfx) {  // (item 8f0) PostFx +0x10's FullScreenCopy (burst_grab's find_target)
    void* obj = *reinterpret_cast<void**>(postfx + 0x10);
    char where[64] = "?";
    const void* found = obj ? d3d::resource_in_object(obj, 0x100, "FullScreenCopy", where, sizeof(where)) : nullptr;
    if (!found) found = d3d::unique_resource("FullScreenCopy");
    return static_cast<ID3D12Resource*>(const_cast<void*>(found));
}

std::string probe_arm(int crop_w, int crop_h) {
    char* postfx = *reinterpret_cast<char**>(anchors::addr(Id::PostFxSingleton));
    if (!postfx) return "ERROR no PostFx (NOT MEASURED)";
    const bool fsc = g_probe_fsc.load();
    ID3D12Resource* vel = fsc ? find_fsc(postfx) : find_vel(postfx);
    if (!vel) return fsc ? "ERROR no single live \"FullScreenCopy\" (NOT MEASURED)" : "ERROR no single live \"Velocity RT\" (NOT MEASURED)";
    const D3D12_RESOURCE_DESC d = vel->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.MipLevels != 1) return fsc ? "ERROR FullScreenCopy is not one 2D subresource" : "ERROR the Velocity RT is not one 2D subresource";
    if (fsc && d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return "ERROR FullScreenCopy is not R16G16B16A16_FLOAT (the sentinel's format)";
    if (fsc && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) return "ERROR FullScreenCopy allows simultaneous access (its watched state would be stale)";
    ID3D12Resource* arr = fsc ? nullptr : prepass::velocity_array();  // (the FSC mode: no slice 1)
    if (arr) {
        const D3D12_RESOURCE_DESC ad = arr->GetDesc();
        if (ad.Width != d.Width || ad.Height != d.Height || ad.Format != d.Format || ad.DepthOrArraySize != 2) arr = nullptr;  // (not this target's)
    }
    const uint32_t w = static_cast<uint32_t>(crop_w > 0 && static_cast<UINT64>(crop_w) <= d.Width ? crop_w : d.Width);
    const uint32_t h = static_cast<uint32_t>(crop_h > 0 && static_cast<UINT>(crop_h) <= d.Height ? crop_h : d.Height);
    g_probe_box = {static_cast<UINT>((d.Width - w) / 2), static_cast<UINT>((d.Height - h) / 2), 0, 0, 0, 1};
    g_probe_box.right = g_probe_box.left + w;
    g_probe_box.bottom = g_probe_box.top + h;
    D3D12_RESOURCE_DESC cd = d;
    cd.Width = w;
    cd.Height = h;
    ID3D12Device* dev = nullptr;
    if (FAILED(vel->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return "ERROR no device";
    UINT rows = 0;
    UINT64 total = 0;
    dev->GetCopyableFootprints(&cd, 0, 1, 0, &g_probe_fp, &rows, &g_probe_row, &total);
    g_probe_slot_bytes = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    const UINT64 need = g_probe_slot_bytes * kProbeSlots;
    if (!g_probe_rb || g_probe_rb_size < need) {
        // (the review's must-fix, item 8f0: a probe that timed out may still have copies in flight into the old buffer, so a replaced
        // readback is never released: test only, it grows at most once per mode)
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = need;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g_probe_rb = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_probe_rb));
        g_probe_rb_size = g_probe_rb ? need : 0;
    }
    if (!g_probe_fence) dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_probe_fence));
    if (!g_probe_fence_event) g_probe_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev->Release();
    if (!g_probe_rb || !g_probe_fence || !g_probe_fence_event) return "ERROR could not create the readback objects";
    g_probe_w = w;
    g_probe_h = h;
    g_probe_fmt = static_cast<uint32_t>(d.Format);
    g_probe_vel = vel;
    g_probe_arr = arr;
    for (bool& b : g_probe_ok) b = false;
    for (bool& b : g_probe_pred) b = false;
    g_probe_first_vel = g_probe_pre_region = g_probe_arr_gone = false;
    g_fsc_cleared = g_fsc_unbound = g_fsc_state_unknown = false;
    g_probe_mrt = 0;
    g_probe_out_n = 0;
    if (void* out_obj = *reinterpret_cast<void**>(postfx + post_target::field(postfx))) {
        char where[64] = "?";
        const void* out_res = d3d::resource_in_object(out_obj, 0x100, post_target::name(postfx), where, sizeof(where));
        if (!out_res) out_res = d3d::unique_resource(post_target::name(postfx));
        if (out_res) g_probe_out_n = d3d::rtv_handles_of(out_res, g_probe_out_rtv, 8);
    }
    {  // the readback zeroed (a slot never copied must not show an older probe's data)
        void* z = nullptr;
        D3D12_RANGE none{0, 0};
        if (SUCCEEDED(g_probe_rb->Map(0, &none, &z)) && z) {
            std::memset(z, 0, static_cast<size_t>(g_probe_rb_size));
            D3D12_RANGE all{0, static_cast<SIZE_T>(g_probe_rb_size)};
            g_probe_rb->Unmap(0, &all);
        }
    }
    d3d::watch_add(vel);
    return {};
}

std::string probe_write() {
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q || !g_probe_fence) return "ERROR no present queue";
    q->Signal(g_probe_fence, ++g_probe_fence_value);
    g_probe_fence->SetEventOnCompletion(g_probe_fence_value, g_probe_fence_event);
    if (WaitForSingleObject(g_probe_fence_event, 3000) != WAIT_OBJECT_0) return "ERROR the copies timed out (NOT MEASURED)";
    uint8_t* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(g_probe_rb_size)};
    if (FAILED(g_probe_rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) return "ERROR Map failed";
    std::string out;
    for (int s = 0; s < kProbeSlots; ++s) {
        if (!g_probe_ok[s]) {
            if (g_probe_fsc.load() && s == 2) continue;
            out += std::string(" | ") + (g_probe_fsc.load() ? kFscName[s] : kProbeName[s]) + " NOT MEASURED" +
                   (g_probe_pred[s] ? " (predicated)" : s == 2 && g_probe_arr_gone ? " (the array made again)"
                    : s == 6 && g_probe_out_n == 0 ? " (no render-target view of the post output seen)" : "");
            continue;
        }
        const std::string path = g_probe_prefix + "-" + (g_probe_fsc.load() ? kFscName[s] : kProbeName[s]) + ".rdb";
        std::wstring wpath(path.begin(), path.end()), tmp = wpath + L".tmp";
        FILE* f = nullptr;
        bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
        if (ok) {
            const uint32_t hdr[8] = {0x31424452u /* RDB1 */, g_probe_w, g_probe_h, g_probe_fmt, 0xffffffffu, 1u, static_cast<uint32_t>(g_probe_frame),
                                     static_cast<uint32_t>(g_probe_row)};
            fwrite(hdr, 1, sizeof(hdr), f);
            const uint32_t valid = 1;
            fwrite(&valid, 1, 4, f);
            for (uint32_t y = 0; y < g_probe_h; ++y)
                fwrite(p + static_cast<UINT64>(s) * g_probe_slot_bytes + g_probe_fp.Offset + static_cast<UINT64>(y) * g_probe_fp.Footprint.RowPitch, 1,
                       static_cast<size_t>(g_probe_row), f);
            ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        out += std::string(" | ") + (g_probe_fsc.load() ? kFscName[s] : kProbeName[s]) + (ok ? " written" : " NOT WRITTEN");
    }
    D3D12_RANGE none{0, 0};
    g_probe_rb->Unmap(0, &none);
    if (g_probe_fsc.load()) {
        char fh[480];
        std::snprintf(fh, sizeof(fh), "fsc probe %s, frame %llu, %u x %u format %u; at the MRT bind: %s; sentinel %.10g %.10g %.10g %.10g; mrt binds with FSC after it %d",
                      g_probe_cut ? "a cut frame's pass 2" : "a pass 2 not cut", static_cast<unsigned long long>(g_probe_frame), g_probe_w, g_probe_h, g_probe_fmt,
                      g_fsc_cleared ? "seed copied, FSC cleared to the sentinel" : g_fsc_unbound ? "FSC NOT BOUND (nothing cleared)"
                      : g_fsc_state_unknown ? "FSC's state not seen in this list (nothing cleared)" : g_probe_pred[1] ? "predicated (nothing cleared)" : "not reached",
                      kFscSentinel[0], kFscSentinel[1], kFscSentinel[2], kFscSentinel[3], g_probe_mrt);
        return fh + out;
    }
    char head[480];
    std::snprintf(head, sizeof(head), "probe %s, frame %llu, %u x %u format %u; the first bind after the begin marker %s the Velocity RT%s%s%s",
                  g_probe_pass1 ? "a pass 1" : g_probe_cut ? "a cut frame's pass 2" : "a pass 2 not cut", static_cast<unsigned long long>(g_probe_frame), g_probe_w,
                  g_probe_h, g_probe_fmt,
                  g_probe_first_vel ? "was" : "was NOT", g_probe_arr ? "" : " (no velocity array: slice1 not taken)",
                  g_probe_pre_region ? "; pre taken IN A PRE-PASS REGION (before the handover)" : "",
                  g_probe_post_vel ? "; post's bind included the RT (mrt2 is the next bind with it)" : "; post's bind did NOT include the RT (mrt2 is the first bind with it)");
    return head + out;
}

void probe_frame_end(uint64_t frame) {
    const int pp = g_pp.load(std::memory_order_acquire);
    if (g_probe.load(std::memory_order_acquire) != 2 || pp < 3) return;
    if (pp == 4) g_pp.store(5, std::memory_order_release);  // (a frame end: no later bind of this frame is taken)
    if (g_probe_ends.fetch_add(1) + 1 < 2) return;  // (the second frame end after the copies were recorded: submitted)
    d3d::set_bind_tap(d3d::kBindTapCutVel, nullptr);
    g_probe_frame = frame;
    g_probe_result = probe_write();
    d3d::watch_remove(g_probe_vel);
    g_pp = 0;
    g_probe = 0;
    SetEvent(g_probe_done);
}

std::string probe(const std::string& prefix, bool cut, bool pass1) {
    if (g_mode.load() == 0) return "ERROR cutvel is off (no markers in mode 0): \"prepass cutvel d1\" first";
    if (g_probe.load() != 0) return "ERROR a probe is running";
    if (eye2_collector::where2_on()) return "ERROR \"prepass e2sc where2\" holds the bind tap: \"prepass e2sc where2 off\" first";
    if (!g_probe_done) g_probe_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::string err = g_probe_fsc.load() ? probe_arm(0, 0) : probe_arm(960, 540);  // (the FSC mode: the whole image)
    if (!err.empty()) return err;
    g_probe_prefix = prefix;
    g_probe_cut = cut ? 1 : 0;
    g_probe_pass1 = pass1 ? 1 : 0;
    g_probe_ends = 0;
    g_pp = 0;
    ResetEvent(g_probe_done);
    d3d::set_bind_tap(d3d::kBindTapCutVel, &probe_tap);
    g_probe.store(1, std::memory_order_release);
    if (WaitForSingleObject(g_probe_done, 8000) != WAIT_OBJECT_0) {
        // (left armed or half done: stood down; the tap off first, the late copies land in a buffer kept alive)
        d3d::set_bind_tap(d3d::kBindTapCutVel, nullptr);
        g_probe = 0;
        g_pp = 0;
        d3d::watch_remove(g_probe_vel);
        return std::string("ERROR no matching pass within 8 s (NOT MEASURED; wanted ") + (pass1 ? "a pass 1)" : cut ? "a cut frame's pass 2)" : "a pass 2 not cut)");
    }
    log::info("[cutvel] %s", g_probe_result.c_str());
    return g_probe_result;
}

// ---- d1, round 3 (velocity.md 7): the scene viewport's screen size at the velocity pass, against the render size.
// FUN_140130f00 (from ViewportSetCurrent 0x130940 with its 4th argument 0, as 0x706050 calls it at entry 0x140706455 and
// exit 0x1407076db) sets vp +0x330/+0x334 to the size in DAT_1422722d8/d0 on the thread at DAT_1422722e0 (else d4/cc)
// and rebuilds the screen matrix +0x280..+0x2bc when they differ [V]. d8/d0 are set by the render-target lock's first
// lock in a nest (FUN_140157870, latch DAT_142c37202) to the locked target's size and put back at the slot-0 unlock
// (FUN_140157db0); d4/cc are the window's size [V, the review]. Inside the walk the collector is locked: the render
// size. At the velocity pass no lock of the pre-pass is held, so d8/d0 there are the window's: the count compares with
// DLSS's render size (PostFx +0x530 -> +0xec/+0xf0, or the eye shape's, as taa.cpp's update_on does), and d8/d0 are
// also sampled at a cut walk's end (cut_walk_end, the collector still locked). SceneRender writes gViewport (rage_prev_world,
// handle 0x2c44e7c) for the colour walk from ViewportCurrent +0x330/+0x334 x +0x308/+0x30c at 0x1405c3f91..0x1405c404c
// [V]. camera_lever.cpp:425's copy of the master viewport per pass puts +0x330 back to the window's size. Globals read
// by RVA, under the analysed-build gate install() holds (exe_matches, not relocated); reads only.
constexpr uintptr_t kRvaSizeW = 0x22722d4, kRvaSizeH = 0x22722cc, kRvaSizeWt = 0x22722d8, kRvaSizeHt = 0x22722d0, kRvaSizeTid = 0x22722e0;
struct VpSample {
    uintptr_t cur = 0, scene = 0;
    int w = 0, h = 0, tw = 0, th = 0, twt = 0, tht = 0, rw = 0, rh = 0;  // (rw, rh: the render size, DLSS's or the eye shape's)
    DWORD tid = 0;
    float scale[2] = {}, screen[16] = {};  // +0x308/+0x30c, the screen matrix +0x280..+0x2bf
    uint8_t flag371 = 0;
    bool ok = false;
};
struct VpSlot {
    std::atomic<uint32_t> seq{0};
    VpSample s;
    std::atomic<uint64_t> n{0}, not_render{0}, not_scene{0}, no_render{0};
};
VpSlot g_vps[kKlasses];
void vp_sample(int k) {
    VpSample s;
    const uintptr_t base = anchors::base();
    s.scene = reinterpret_cast<uintptr_t>(camera_lever::scene_viewport());
    s.ok = rd(anchors::addr(Id::ViewportCurrent), &s.cur) && s.cur && rd(s.cur + 0x330, &s.w) && rd(s.cur + 0x334, &s.h) &&
           raw(s.cur + 0x308, s.scale, sizeof(s.scale)) && raw(s.cur + 0x280, s.screen, sizeof(s.screen)) && rd(s.cur + 0x371, &s.flag371) &&
           rd(base + kRvaSizeW, &s.tw) && rd(base + kRvaSizeH, &s.th) && rd(base + kRvaSizeWt, &s.twt) && rd(base + kRvaSizeHt, &s.tht) &&
           rd(base + kRvaSizeTid, &s.tid);
    {  // the render size: the eye shape's when it shapes DLSS, else DLSS's own (PostFx +0x530 -> +0xec/+0xf0, taa.cpp)
        uint32_t srw = 0, srh = 0, sew = 0, seh = 0;
        uintptr_t pfx = 0, dl = 0;
        if (eye_shape::dlss_shape(&srw, &srh, &sew, &seh)) {
            s.rw = static_cast<int>(srw);
            s.rh = static_cast<int>(srh);
        } else if (rd(anchors::addr(Id::PostFxSingleton), &pfx) && pfx && rd(pfx + 0x530, &dl) && dl) {
            rd(dl + 0xec, &s.rw);
            rd(dl + 0xf0, &s.rh);
        }
    }
    VpSlot& sl = g_vps[k];
    bump(sl.n);
    if (s.ok) {
        if (s.rw <= 0 || s.rh <= 0) bump(sl.no_render);
        else if (s.w != s.rw || s.h != s.rh) bump(sl.not_render);  // (what the colour walk's gViewport is made from)
        if (s.cur != s.scene) bump(sl.not_scene);
    }
    sl.seq.fetch_add(1, std::memory_order_acq_rel);
    sl.s = s;
    sl.seq.fetch_add(1, std::memory_order_acq_rel);
}

// the size FUN_140130f00 would take at a cut walk's end (cut_walk_end: the collector still locked, d8/d0 its size), and
// the viewport's there
std::atomic<uint64_t> g_lb_n{0}, g_lb_not_render{0};
std::atomic<int> g_lb_w{0}, g_lb_h{0}, g_lb_vw{0}, g_lb_vh{0};
std::atomic<DWORD> g_lb_tid{0};
void lb_sample() {
    const uintptr_t base = anchors::base();
    int w = 0, h = 0, vw = 0, vh = 0;
    DWORD tid = 0;
    uintptr_t cur = 0;
    if (!rd(base + kRvaSizeWt, &w) || !rd(base + kRvaSizeHt, &h) || !rd(base + kRvaSizeTid, &tid) || !rd(anchors::addr(Id::ViewportCurrent), &cur) ||
        !cur || !rd(cur + 0x330, &vw) || !rd(cur + 0x334, &vh))
        return;
    bump(g_lb_n);
    if (tid == GetCurrentThreadId() && (vw != w || vh != h)) bump(g_lb_not_render);
    g_lb_w = w;
    g_lb_h = h;
    g_lb_vw = vw;
    g_lb_vh = vh;
    g_lb_tid = tid;
}

// ---- d4 (velocity.md 7): at a cut walk's end, the walk's own ViewportSetCurrent(ViewportCurrent, 1, 0, 0) replayed
// once on the render thread (0x706050 makes that call at entry and at exit [V]): FUN_140130f00's size and screen
// matrix (from d8/d0, which the collector's lock holds there at its size), PushGlobals' four stored matrices (+0x80,
// +0xc0, +0x100, +0x140, as they are: "post-update" is [I]), FUN_14012dbe0's viewport packet. That packet plays before
// the walk-end handover (it is recorded after the marker but before the region's next list call ends it): harmless,
// it sets no target.
using SetCurrent_t = uintptr_t (*)(uintptr_t vp, char push, uintptr_t unused, char keep);
std::atomic<uint64_t> g_d4{0}, g_d4_refused{0};
bool d4_call() {  // (the replay itself: true when made; the callers count)
    uintptr_t cur = 0;
    if (!rd(anchors::addr(Id::ViewportCurrent), &cur) || !cur) return false;
    __try {
        reinterpret_cast<SetCurrent_t>(anchors::addr(Id::ViewportSetCurrent))(cur, 1, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
void d4() { bump(d4_call() ? g_d4 : g_d4_refused); }
// (run 25, GOAL-single-pass item 6g; test only, off: "prepass cutvel p1d4 on|off") d4's replay also at pass 1's walk end, in
// mode 4: pass 1's gViewport then made as pass 2's is (families.md 12: pass 1's entity velocity offset in this configuration,
// and the twin's two views share pass 1's row 4)
std::atomic<bool> g_p1d4{false};
// (the review) its own counts: made, refused; and before the replay, whether the viewport's size (+0x330/+0x334) already was
// the locked target's (d8/d0, on this thread): "same" means the replay repeats the walk's own exit call
std::atomic<uint64_t> g_p1d4_n{0}, g_p1d4_refused{0}, g_p1d4_same{0}, g_p1d4_differ{0}, g_p1d4_unread{0};

// ---- d5 and d6 (run 21 item 2, research\run21\d4.md): d4's one needed effect without the rest. ViewportSetCurrent with
// its 4th argument 0 does three things [V 0x130940]: FUN_140130f00 (the size and screen matrix), PushGlobals (four
// matrices into the global CB's storage) and FUN_14012dbe0 (the viewport packet). Only the first is what the colour
// walk's gViewport (0x1405c3f91) needs. FUN_140130f00 [V, 0x140130f00..] reads vp +0x300..+0x313 and d8/d0 (the locked
// target's size on DAT_1422722e0's thread), returns early when +0x330/+0x334 already hold that size, and otherwise writes
// exactly +0x280..+0x29f, +0x2b0..+0x2bf and +0x330..+0x337 (nothing else). Called by RVA on the analysed build only
// (install()'s gate), its first 14 bytes compared at install; without them d5 and d6 are refused (counted).
//   d6  at the cut walk's end, FUN_140130f00(ViewportCurrent) alone: d4 without the push and the packet.
//   d5  at the cut walk's end (the collector locked: d8/d0 its size, as d4 takes it) FUN_140130f00 runs on a copy of the
//       viewport; at the velocity pass's entry of the same pass 2 (0x1405c3e74: after 0x5cbfd0 has returned, the collector,
//       its term and the footprints drawn, and the Light Buffer's 0x6e0450), those three ranges are copied into
//       ViewportCurrent, if it is still the same viewport with the same +0x300..+0x313. Nothing of d5 reaches the game
//       before the collector segment is over; gViewport at 0x5c3f91 is made from the same size as with d4.
constexpr uintptr_t kRvaNormalise = 0x130f00;
const uint8_t kNormaliseEntry[14] = {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xe9, 0xe8, 0x82, 0x81, 0xd8, 0x00};
using Normalise_t = void (*)(uintptr_t vp);
uintptr_t g_normalise = 0;  // FUN_140130f00 when its entry bytes are as analysed (install)
constexpr size_t kVpBytes = 0x470;  // (camera_lever's kViewportSize)
alignas(16) char g_d5_vp[kVpBytes];  // the render thread: the normalised copy
uint8_t g_d5_in[0x14] = {};          // its +0x300..+0x313 (FUN_140130f00's inputs besides d8/d0)
uintptr_t g_d5_src = 0;              // the viewport it was copied from
bool g_d5_pending = false;           // the render thread: a copy waits for this pass 2's velocity pass
std::atomic<uint64_t> g_d5_prepared{0}, g_d5{0}, g_d5_refused{0}, g_d5_changed{0}, g_d5_missed{0}, g_d5_none{0}, g_d6{0}, g_d6_refused{0};
std::atomic<int> g_d5_w{0}, g_d5_h{0};
bool call_normalise(uintptr_t vp) {
    __try {
        reinterpret_cast<Normalise_t>(g_normalise)(vp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// the three ranges FUN_140130f00 writes, packed: +0x280..+0x29f, +0x2b0..+0x2bf, +0x330..+0x337 (56 bytes)
constexpr size_t kD5Bytes = 0x20 + 0x10 + 8;
void pack_ranges(const char* vp, uint8_t* out) {
    std::memcpy(out, vp + 0x280, 0x20);
    std::memcpy(out + 0x20, vp + 0x2b0, 0x10);
    std::memcpy(out + 0x30, vp + 0x330, 8);
}
bool read_ranges(uintptr_t vp, uint8_t* out) {
    __try {
        pack_ranges(reinterpret_cast<const char*>(vp), out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint8_t g_d5_before[kD5Bytes] = {};  // the render thread: the real viewport's ranges at the walk's end, before any normalising
// (the review's should-fix 2) Regenerate 0x1401310f0's +0x240..+0x27f, the WVP (+0x100..+0x13f) times the screen matrix
// (+0x280..+0x2bf), in its own order [V the disassembly, 0x1401313ae..0x14013149f]: for each row i, the WVP row's four
// elements broadcast (shufps 0x00/0x55/0xaa/0xff), each times one screen row (mulps), summed
// ((x*S0 + y*S1) + z*S2) + w*S3 (addps), no FMA. Regenerate itself is not called (it pushes when the viewport is current).
// The +0x100 it reads is the one its last run stored, as Regenerate's own registers held it.
void screen_rows(const float* wvp, const float* s, float* out) {
    const __m128 s0 = _mm_loadu_ps(s), s1 = _mm_loadu_ps(s + 4), s2 = _mm_loadu_ps(s + 8), s3 = _mm_loadu_ps(s + 12);
    for (int i = 0; i < 4; ++i) {
        __m128 r = _mm_mul_ps(_mm_set1_ps(wvp[i * 4]), s0);
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(wvp[i * 4 + 1]), s1));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(wvp[i * 4 + 2]), s2));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(wvp[i * 4 + 3]), s3));
        _mm_storeu_ps(out + i * 4, r);
    }
}
bool put_normalised(uintptr_t cur) {  // (the three ranges FUN_140130f00 writes, from the copy; then +0x240..+0x27f as Regenerate makes them)
    __try {
        std::memcpy(reinterpret_cast<void*>(cur + 0x280), g_d5_vp + 0x280, 0x20);
        std::memcpy(reinterpret_cast<void*>(cur + 0x2b0), g_d5_vp + 0x2b0, 0x10);
        std::memcpy(reinterpret_cast<void*>(cur + 0x330), g_d5_vp + 0x330, 8);
        alignas(16) float wvp[16], s[16], rows[16];
        std::memcpy(wvp, reinterpret_cast<const void*>(cur + 0x100), sizeof(wvp));
        std::memcpy(s, reinterpret_cast<const void*>(cur + 0x280), sizeof(s));
        screen_rows(wvp, s, rows);
        std::memcpy(reinterpret_cast<void*>(cur + 0x240), rows, sizeof(rows));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void d6() {
    uintptr_t cur = 0;
    if (!g_normalise || !rd(anchors::addr(Id::ViewportCurrent), &cur) || !cur || !call_normalise(cur)) {
        bump(g_d6_refused);
        return;
    }
    bump(g_d6);
}
void d5_prepare() {  // (the cut walk's end, the render thread)
    if (g_d5_pending) bump(g_d5_missed);  // (the last copy's velocity pass never came)
    g_d5_pending = false;
    uintptr_t cur = 0;
    if (!g_normalise || !rd(anchors::addr(Id::ViewportCurrent), &cur) || !cur || !raw(cur, g_d5_vp, kVpBytes)) {
        bump(g_d5_refused);
        return;
    }
    pack_ranges(g_d5_vp, g_d5_before);  // (the review's should-fix 1: the real ranges as they were, before normalising the copy)
    if (!call_normalise(reinterpret_cast<uintptr_t>(g_d5_vp))) {
        bump(g_d5_refused);
        return;
    }
    std::memcpy(g_d5_in, g_d5_vp + 0x300, sizeof(g_d5_in));
    int w = 0, h = 0;
    std::memcpy(&w, g_d5_vp + 0x330, 4);
    std::memcpy(&h, g_d5_vp + 0x334, 4);
    g_d5_w = w;
    g_d5_h = h;
    g_d5_src = cur;
    g_d5_pending = true;
    bump(g_d5_prepared);
}
void d5_apply(bool cut_pass2) {  // (the velocity pass's entry, the render thread)
    if (!g_d5_pending) {
        if (cut_pass2) bump(g_d5_none);
        return;
    }
    g_d5_pending = false;
    if (!cut_pass2) {  // (not the cut pass 2 its walk end belonged to)
        bump(g_d5_missed);
        return;
    }
    uintptr_t cur = 0;
    uint8_t in[sizeof(g_d5_in)] = {}, now[kD5Bytes] = {};
    // (not applied: another viewport current, its inputs changed since the walk's end, or (the review's should-fix 1) a
    // later writer changed the three ranges since then: its values are kept)
    if (!rd(anchors::addr(Id::ViewportCurrent), &cur) || cur != g_d5_src || !raw(cur + 0x300, in, sizeof(in)) || std::memcmp(in, g_d5_in, sizeof(in)) != 0 ||
        !read_ranges(cur, now) || std::memcmp(now, g_d5_before, kD5Bytes) != 0) {
        bump(g_d5_changed);
        return;
    }
    if (!put_normalised(cur)) {
        bump(g_d5_refused);
        return;
    }
    bump(g_d5);
}

// ---- d1, round 3: the PrevWorld writer FUN_1401322b0(ViewportCurrent, prev world) [V: .pdata 0x1322b0..; called per
// entity draw, e.g. 0x14087f52c, with [0x142ac0a10]], hooked only on the analysed build (RVA and entry bytes checked: no
// anchor, nothing on another build). Per class: its calls, those with vp +0x371 clear (it writes nothing then), those not
// on the scene viewport; one sample in 256: vp, +0x371, vp +0x200 (the previous VP), the rows it left in
// gPrevWorldViewProjSingle's CB (rage_prev_world) and that CB's gViewport (handle 0x2c44e7c), as the draw will upload them.
constexpr uintptr_t kRvaPrevWorld = 0x1322b0;
const uint8_t kPrevWorldEntry[16] = {0x4c, 0x8b, 0xdc, 0x48, 0x81, 0xec, 0x38, 0x01, 0x00, 0x00, 0x80, 0xb9, 0x71, 0x03, 0x00, 0x00};
using PrevWorld_t = void (*)(void* vp, const float* prev_world);
PrevWorld_t o_prevworld = nullptr;
struct PwSample {
    uint64_t frame = 0;
    uintptr_t vp = 0, scene = 0;
    uint8_t flag = 0;
    float prev_vp[16] = {}, rows[16] = {}, vport[4] = {};
    bool have_rows = false, have_vport = false;
};
struct PwSlot {
    std::atomic<uint32_t> seq{0};
    PwSample s;
    std::atomic<uint64_t> calls{0}, noflag{0}, not_scene{0}, zero_prev{0};
};
PwSlot g_pw[kKlasses];
bool global_mapped(Id handle, void* out, size_t n) {  // a shader global's bytes as its CB holds them now (mapped, else storage)
    int h = 0, off = -1;
    uintptr_t cb = 0, mem = 0;
    const uintptr_t table = anchors::addr(Id::ShaderGlobalTable);
    if (!anchors::present(handle) || !rd(anchors::addr(handle), &h) || h <= 0 || h > 4096 || !rd(table + static_cast<uintptr_t>(h) * 16, &cb) || !cb ||
        !rd(table + static_cast<uintptr_t>(h) * 16 + 8, &off) || off < 0 || off > 0x1000)
        return false;
    if (!rd(cb + kCbMapped, &mem) || !mem) rd(cb + kCbStorage, &mem);
    return mem && raw(mem + static_cast<uintptr_t>(off), out, n);
}
// (call: the original, or [Stereo] PrepassPrevWorldMirror's call of it and its mirror when its detour is the one: set_d1)
void d1_prevworld(void* vp, const float* prev_world, PrevWorld_t call) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (g_quiet.load(std::memory_order_relaxed) & kQuietPrevWorld) {  // (run 22: quiet 2, the original only)
        call(vp, prev_world);
        return;
    }
    const int pass = mode ? dual_pass::pass() : 0;
    if (pass != 1 && pass != 2) {
        call(vp, prev_world);
        return;
    }
    // (pass 2 of a frame whose cut was decided: from the pass's start, so its early calls count as cut too)
    const bool cut = pass == 2 && rdrvr_prepass_cut && rdrvr_prepass_cut_tid == GetCurrentThreadId();
    const int k = pass == 1 ? kPass1 : cut ? kPass2Cut : kPass2;
    PwSlot& sl = g_pw[k];
    const uintptr_t v = reinterpret_cast<uintptr_t>(vp);
    uint8_t flag = 0;
    rd(v + 0x371, &flag);
    const uint64_t n = sl.calls.load(std::memory_order_relaxed) + 1;
    sl.calls.store(n, std::memory_order_relaxed);
    if (!flag) bump(sl.noflag);
    const uintptr_t scene = reinterpret_cast<uintptr_t>(camera_lever::scene_viewport());
    if (v != scene) bump(sl.not_scene);
    call(vp, prev_world);
    if ((n & 255) != 1) return;
    PwSample s;
    s.frame = state::presents.load(std::memory_order_relaxed);
    s.vp = v;
    s.scene = scene;
    s.flag = flag;
    raw(v + 0x200, s.prev_vp, sizeof(s.prev_vp));
    bool zero = true;
    for (float f : s.prev_vp) zero = zero && f == 0.0f;
    if (zero) bump(sl.zero_prev);
    s.have_rows = global_mapped(Id::HandlePrevWorldVP, s.rows, sizeof(s.rows));
    s.have_vport = global_mapped(Id::HandleViewport, s.vport, sizeof(s.vport));
    sl.seq.fetch_add(1, std::memory_order_acq_rel);
    sl.s = s;
    sl.seq.fetch_add(1, std::memory_order_acq_rel);
}
void hk_prevworld(void* vp, const float* prev_world) { d1_prevworld(vp, prev_world, o_prevworld); }
void install_prevworld() {
    // ([Stereo] PrepassPrevWorldMirror's detour is on 0x1322b0 already, its entry bytes checked before it: d1 runs inside it,
    // never a second detour; the entry is a jump now, so this comes before the bytes' check)
    if (prevworld::installed()) {
        if (!anchors::present(Id::HandlePrevWorldVP) || !anchors::present(Id::HandleViewport) || !anchors::present(Id::ShaderGlobalTable)) {
            log::warn("[cutvel] HandlePrevWorldVP/HandleViewport/ShaderGlobalTable missing on this build: the PrevWorld sampling off");
            return;
        }
        prevworld::set_d1(&d1_prevworld);
        log::info("[cutvel] the PrevWorld writer's d1 sampling runs inside PrepassPrevWorldMirror's detour");
        return;
    }
    const uintptr_t at = anchors::base() + kRvaPrevWorld;
    uint8_t now[sizeof(kPrevWorldEntry)] = {};
    if (!raw(at, now, sizeof(now)) || std::memcmp(now, kPrevWorldEntry, sizeof(now)) != 0) {
        log::warn("[cutvel] the PrevWorld writer's entry is not as analysed: its d1 sampling off");
        return;
    }
    if (!anchors::present(Id::HandlePrevWorldVP) || !anchors::present(Id::HandleViewport) || !anchors::present(Id::ShaderGlobalTable)) {
        log::warn("[cutvel] HandlePrevWorldVP/HandleViewport/ShaderGlobalTable missing on this build: the PrevWorld sampling off");
        return;
    }
    hooks::install("RDR PrevWorld writer (FUN_1401322b0, PrepassCutVelocity d1)", reinterpret_cast<void*>(at), &hk_prevworld, &o_prevworld);
}

void velocity_d1(void* postfx) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!mode || !postfx) {
        o_velocity(postfx);
        return;
    }
    bump(g_calls);
    const uintptr_t p = reinterpret_cast<uintptr_t>(postfx);
    const int pass = dual_pass::pass();
    const bool cut = pass == 2 && prepass::walk_was_cut();
    const int k = pass == 1 ? kPass1 : cut ? kPass2Cut : kPass2;
    if (pass != 1 && pass != 2) {
        if (cut && mode == 2) d2(p);
        o_velocity(postfx);
        return;
    }
    if (mode == 5) d5_apply(cut);  // (run 21: d5's size, before d1's viewport sample sees it)
    const int quiet = g_quiet.load(std::memory_order_relaxed);  // (run 22: d1's parts silenced, test only)
    Sample s;
    const bool read = !(quiet & kQuietReads) && read_sample(p, &s);  // (NDCToPrevNDC and the depth variable's state before the call)
    if (!(quiet & kQuietReads)) vp_sample(k);  // (round 3: the scene viewport's size against the bound target's)
    if (cut && mode == 2) d2(p);
    bool probe = false;
    // (the review: armed seen first with acquire, so the command thread's g_probe_pass1 and g_probe_cut are this probe's)
    if (!(quiet & kQuietMarkers) && g_probe.load(std::memory_order_acquire) == 1 &&
        (g_probe_pass1 ? pass == 1 : pass == 2 && cut == (g_probe_cut != 0))) {
        int one = 1;
        probe = g_probe.compare_exchange_strong(one, 2, std::memory_order_acq_rel);  // (1 -> 2: this pass is the probe's)
    }
    const bool marked = !(quiet & kQuietMarkers) && append_marker(kBegin[k][probe ? 1 : 0]);
    if (probe && !marked) g_probe.store(1, std::memory_order_release);  // (no marker: the next matching pass takes it)
    o_velocity(postfx);
    if (marked) append_marker(kEnd[k][probe ? 1 : 0]);
    if (quiet & kQuietReads) return;
    if (!read) {
        bump(g_slot[k].unread);
        return;
    }
    read_after(p, &s);  // (TexelSize and the depth variable as the call left them)
    publish(s, k);
}
// ([Stereo] PrepassEye2Velocity, eye2_velocity.cpp; test only) its callbacks around the game's call (null unless installed);
// d1-d6 inside them, as before
std::atomic<VelHook_t> g_e2v{nullptr};
std::atomic<bool> g_hooked{false};  // the hook installed (for PrepassCutVelocity or PrepassEye2Velocity)
void hk_velocity(void* postfx) {
    const VelHook_t e2v = g_e2v.load(std::memory_order_relaxed);
    if (e2v) e2v(postfx, false);
    velocity_d1(postfx);
    if (e2v) e2v(postfx, true);
}

void row(char* b, size_t n, const float* m) {
    std::snprintf(b, n, "[%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", m[0], m[1], m[2], m[3], m[4], m[5],
                  m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}
}  // namespace

void install(bool cut_hooked) {
    const int want = config::get_int("Stereo", "PrepassCutVelocity", 0);
    // ([Stereo] PrepassEye2Velocity, test only) the one detour on 0x873920 is shared: installed for either switch
    const bool e2v = config::get_int("Stereo", "PrepassEye2Velocity", 0) == 1;
    if (want <= 0 && !e2v) return;
    if (!cut_hooked) {
        log::warn("[cutvel] PrepassCutVelocity (or PrepassEye2Velocity) needs [Stereo] PrepassWalkCut's hooks (PrepassInstanced and its switches, "
                  "[Compat] DrawMaskInit=1): off");
        return;
    }
    if (!anchors::exe_matches() || anchors::relocated() || !anchors::present(Id::VelocityPass)) {
        log::warn("[cutvel] PrepassCutVelocity (or PrepassEye2Velocity) runs on the analysed RDR.exe build only (PostFx, effect and CB offsets; "
                  "VelocityPass): off");
        return;
    }
    if (!hooks::install("RDR velocity pass (FUN_140873920, PrepassCutVelocity / PrepassEye2Velocity)", reinterpret_cast<void*>(anchors::addr(Id::VelocityPass)),
                        &hk_velocity, &o_velocity))
        return;
    g_hooked = true;
    if (want <= 0) {  // (PrepassEye2Velocity only: mode 0, the detour calls the original and the e2vel callbacks only)
        log::info("[cutvel] the velocity pass hooked for [Stereo] PrepassEye2Velocity only (cutvel's modes off, not installed)");
        return;
    }
    g_installed = true;
    g_mode = want > 6 ? 1 : want;
    {  // (run 21: d5 and d6 call FUN_140130f00 by RVA; the analysed build is install()'s gate, its entry bytes checked here)
        uint8_t now[sizeof(kNormaliseEntry)] = {};
        const uintptr_t f = anchors::base() + kRvaNormalise;
        if (raw(f, now, sizeof(now)) && !std::memcmp(now, kNormaliseEntry, sizeof(now))) g_normalise = f;
        else log::warn("[cutvel] FUN_140130f00's entry is not as analysed: d5 and d6 are refused (counted)");
    }
    install_prevworld();
    d3d::add_frame_end_listener(&probe_frame_end);
    if (g_mode.load() == 3 && !rec_state::installed())
        log::warn("[cutvel] d3 needs [Stereo] RecorderReemit=1 at start (not installed): its re-emits are counted as absent");
    log::info("[cutvel] [Stereo] PrepassCutVelocity on (test only), d%d: the velocity pass's constants read every pass%s", g_mode.load(),
              g_mode.load() == 2 ? "; rdr2_taa's CB re-mapped and NDCToPrevNDC set again in the cut frames' pass 2"
              : g_mode.load() == 3 ? "; the record side re-emitted at the cut walks' end"
              : g_mode.load() == 4 ? "; ViewportSetCurrent replayed at the cut walks' end"
              : g_mode.load() == 5 ? "; the screen size made at the cut walks' end, put in at the velocity pass"
              : g_mode.load() == 6 ? "; the screen size alone (FUN_140130f00) at the cut walks' end" : "");
}

void pass1_walk_end() {
    if (!g_p1d4.load(std::memory_order_relaxed) || g_mode.load(std::memory_order_relaxed) != 4) return;
    const uintptr_t base = anchors::base();
    int w = 0, h = 0, vw = 0, vh = 0;
    DWORD tid = 0;
    uintptr_t cur = 0;
    if (rd(base + kRvaSizeWt, &w) && rd(base + kRvaSizeHt, &h) && rd(base + kRvaSizeTid, &tid) && rd(anchors::addr(Id::ViewportCurrent), &cur) &&
        cur && rd(cur + 0x330, &vw) && rd(cur + 0x334, &vh) && tid == GetCurrentThreadId())
        (vw == w && vh == h ? g_p1d4_same : g_p1d4_differ).fetch_add(1, std::memory_order_relaxed);
    else
        g_p1d4_unread.fetch_add(1, std::memory_order_relaxed);
    (d4_call() ? g_p1d4_n : g_p1d4_refused).fetch_add(1, std::memory_order_relaxed);
}
void cut_walk_end() {
    eye2_collector::cut_walk_end_mark();  // (run 22, test only: [Stereo] PrepassEye2Collector's where2 step; returns at once unless armed)
    if (g_mode.load(std::memory_order_relaxed) != 0 && !(g_quiet.load(std::memory_order_relaxed) & kQuietWalkEnd))
        lb_sample();  // (before d4: the size as the cut left it)
    if (g_mode.load(std::memory_order_relaxed) == 4) {
        d4();
        return;
    }
    if (g_mode.load(std::memory_order_relaxed) == 5) {  // (run 21) the size made here, put in at the velocity pass
        d5_prepare();
        return;
    }
    if (g_mode.load(std::memory_order_relaxed) == 6) {  // (run 21) the size alone, here
        d6();
        return;
    }
    if (g_mode.load(std::memory_order_relaxed) != 3) return;
    if (!rec_state::installed()) {
        bump(g_d3_absent);
        return;
    }
    if (rec_state::reemit_now()) bump(g_d3);
    else bump(g_d3_refused);
}

bool velocity_hooked() { return g_hooked.load() && o_velocity; }
void set_e2v(VelHook_t fn) { g_e2v = fn; }
void call_velocity(void* postfx) {
    if (o_velocity) o_velocity(postfx);
}

bool fsc_listening() { return g_probe_fsc.load(std::memory_order_relaxed) && g_pp.load(std::memory_order_relaxed) == 4; }
void fsc_before_copy(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst) {  // (the playback thread, before the game's CopyResource)
    if (!g_probe_fsc.load(std::memory_order_relaxed) || g_pp.load(std::memory_order_acquire) != 4 || g_probe_ok[0] || !g_fsc_cleared) return;
    if (GetCurrentThreadId() != g_pb_tid.load(std::memory_order_relaxed) || !dst || !d3d::resource_named(dst, "Depth Resolve")) return;
    D3D12_RESOURCE_STATES st{};
    if (d3d::thread_predicated()) g_probe_pred[0] = true;
    else if (probe_vel_state(cl, &st)) probe_copy(cl, 0, g_probe_vel, 0, st);  // dr: FSC at segment e's end
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load()) return "cutvel: not installed (start with [Stereo] PrepassCutVelocity=1..6 and PrepassWalkCut's switches)";
    if (t.size() >= 4 && t[2] == "probe") return probe(t[3], !(t.size() >= 5 && t[4] == "nocut"), t.size() >= 5 && t[4] == "pass1");
    if (t.size() >= 4 && t[2] == "fsc") {  // (item 8f0, test only) the FSC sentinel probe: one pass 2
        if (g_probe.load() != 0) return "ERROR a probe is running";
        g_probe_fsc = true;
        std::string r = probe(t[3], !(t.size() >= 5 && t[4] == "nocut"), false);
        g_probe_fsc = false;
        return r;
    }
    if (t.size() >= 3) {
        const std::string& w = t[2];
        if (w == "off") g_mode = 0;
        else if (w == "d1") g_mode = 1;
        else if (w == "on" || w == "d2") g_mode = 2;
        else if (w == "d3") g_mode = 3;
        else if (w == "d4") g_mode = 4;
        else if (w == "d5") g_mode = 5;
        else if (w == "d6") g_mode = 6;
        else if (w == "p1d4") {  // (run 25 item 6g, test only)
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off")) return "ERROR prepass cutvel p1d4 on|off (only in d4)";
            g_p1d4 = t[3] == "on";
            log::info("[cutvel] p1d4 %s (test only: d4's replay also at pass 1's walk end, in mode 4)", t[3].c_str());
        }
        else if (w == "quiet") {  // (run 22, test only) d1's parts silenced: a mask of 1 markers, 2 PrevWorld, 4 the reads, 8 lb_sample
            const int q = t.size() >= 4 ? std::atoi(t[3].c_str()) : -1;
            if (q < 0 || q > 15) return "ERROR prepass cutvel quiet N (0..15: 1 the markers, 2 the PrevWorld sampling, 4 the velocity pass's reads, 8 the walk end's sample)";
            g_quiet = q;
            log::info("[cutvel] quiet %d (the session)", q);
        }
        else if (w == "reset") {  // (a test: the counts from now; the render thread racing it loses a count at most)
            for (auto* a : {&g_calls, &g_d2, &g_d2_refused, &g_d2_tech, &g_d2_unmapped, &g_d2_size, &g_d3, &g_d3_refused, &g_d3_absent, &g_d4,
                            &g_d4_refused, &g_p1d4_n, &g_p1d4_refused, &g_p1d4_same, &g_p1d4_differ, &g_p1d4_unread, &g_d5_prepared, &g_d5, &g_d5_refused, &g_d5_changed, &g_d5_missed, &g_d5_none, &g_d6, &g_d6_refused})
                a->store(0);
            for (VpSlot& s : g_vps)
                for (auto* a : {&s.n, &s.not_render, &s.not_scene, &s.no_render}) a->store(0);
            for (auto* a : {&g_lb_n, &g_lb_not_render}) a->store(0);
            for (PwSlot& s : g_pw)
                for (auto* a : {&s.calls, &s.noflag, &s.not_scene, &s.zero_prev}) a->store(0);
            for (Slot& s : g_slot)
                for (auto* a : {&s.n, &s.unread, &s.upload_due, &s.cpu_ok, &s.local_ok, &s.gpu_as_p1, &s.unmapped, &s.set_again, &s.rt_is_f8,
                                &s.after_unread})
                    a->store(0);
            for (int k = 0; k < kKlasses; ++k)
                for (auto* a : {&g_pb_runs[k], &g_pb_draws[k], &g_pb_nodraw[k], &g_pb_pred[k], &g_pb_region[k]}) a->store(0);
            g_pb_unpaired = 0;
        }
        if (w != "reset" && w != "quiet") log::info("[cutvel] mode %d (the session)", g_mode.load());
    }
    auto v = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
    const int mode = g_mode.load();
    char b[900];
    std::snprintf(b, sizeof(b),
                  "cutvel %s: calls %llu | d2 re-set %llu (CB unmapped, set mapped it %llu), refused %llu, CB size not %d %llu, not techniques 2-5 %llu | "
                  "d3 re-emits %llu, refused %llu, RecorderReemit absent %llu | d4 SetCurrent replays %llu, refused %llu (pass 1's: p1d4 %s, made %llu, refused %llu; the size before it the locked one %llu, not %llu, unread %llu)",
                  mode == 0 ? "off" : mode == 1 ? "d1" : mode == 2 ? "d2" : mode == 3 ? "d3" : mode == 4 ? "d4" : mode == 5 ? "d5" : "d6", v(g_calls),
                  v(g_d2), v(g_d2_unmapped), v(g_d2_refused), g_cb_size0.load(), v(g_d2_size), v(g_d2_tech), v(g_d3), v(g_d3_refused), v(g_d3_absent),
                  v(g_d4), v(g_d4_refused), g_p1d4.load() ? "on" : "off", v(g_p1d4_n), v(g_p1d4_refused), v(g_p1d4_same),
                  v(g_p1d4_differ), v(g_p1d4_unread));
    std::string out = b;
    std::snprintf(b, sizeof(b),
                  " | d5 sizes made at the walk's end %llu, put in at the velocity pass %llu (last %dx%d), refused %llu, viewport, its inputs or the ranges changed since (not applied) %llu, "
                  "no velocity pass of that pass 2 %llu, a cut pass 2 with none made %llu | d6 normalised %llu, refused %llu%s",
                  v(g_d5_prepared), v(g_d5), g_d5_w.load(), g_d5_h.load(), v(g_d5_refused), v(g_d5_changed), v(g_d5_missed), v(g_d5_none), v(g_d6),
                  v(g_d6_refused), g_normalise ? "" : " (FUN_140130f00 not as analysed: d5/d6 refused)");
    out += b;
    std::snprintf(b, sizeof(b), " | quiet %d", g_quiet.load());  // (run 22)
    out += b;
    std::snprintf(b, sizeof(b), " | markers unpaired %llu | at the cut walks' end (before d4): samples %llu, viewport size != d8/d0 (the "
                  "collector's, locked) %llu; last viewport %dx%d, d8/d0 %dx%d (thread %lu)",
                  v(g_pb_unpaired), v(g_lb_n), v(g_lb_not_render), g_lb_vw.load(), g_lb_vh.load(), g_lb_w.load(), g_lb_h.load(),
                  static_cast<unsigned long>(g_lb_tid.load()));
    out += b;
    for (int k = 0; k < kKlasses; ++k) {  // (round 3: the PrevWorld writer)
        PwSlot& sl = g_pw[k];
        PwSample s;
        bool got = false;
        for (int i = 0; i < 8 && !got; ++i) {
            const uint32_t a = sl.seq.load(std::memory_order_acquire);
            if (a & 1) continue;
            s = sl.s;
            got = sl.seq.load(std::memory_order_acquire) == a;
        }
        char r0[200], r1[200];
        row(r0, sizeof(r0), s.prev_vp);
        row(r1, sizeof(r1), s.rows);
        std::snprintf(b, sizeof(b),
                      " | %s PrevWorld: calls %llu, +0x371 clear %llu, not the scene viewport %llu, +0x200 all zero (sampled) %llu; last: frame %llu vp %p "
                      "(scene %p) +0x371 %u, +0x200 %s, rows %s%s, gViewport [%.4f %.4f %.4f %.4f]%s",
                      kKlassName[k], v(sl.calls), v(sl.noflag), v(sl.not_scene), v(sl.zero_prev), static_cast<unsigned long long>(s.frame),
                      reinterpret_cast<void*>(s.vp), reinterpret_cast<void*>(s.scene), s.flag, r0, r1, s.have_rows ? "" : " (unread)", s.vport[0],
                      s.vport[1], s.vport[2], s.vport[3], s.have_vport ? "" : " (unread)");
        if (!got) std::snprintf(b, sizeof(b), " | %s PrevWorld: calls %llu (the sample unread: being written)", kKlassName[k], v(sl.calls));
        out += b;
    }
    for (int k = 0; k < kKlasses; ++k) {  // (round 3: the viewport's size at the velocity pass)
        VpSlot& sl = g_vps[k];
        VpSample s;
        bool got = false;
        for (int i = 0; i < 8 && !got; ++i) {
            const uint32_t a = sl.seq.load(std::memory_order_acquire);
            if (a & 1) continue;
            s = sl.s;
            got = sl.seq.load(std::memory_order_acquire) == a;
        }
        std::snprintf(b, sizeof(b),
                      " | %s viewport at the velocity pass: samples %llu, viewport size != the render size %llu (no render size %llu), not the "
                      "scene viewport %llu; last: %p (scene %p) +0x330 %dx%d, render %dx%d, d4/cc (the window) %dx%d, d8/d0 %dx%d (thread %lu), "
                      "+0x308 %.4f %.4f, +0x371 %u, screen [%.2f %.2f | %.2f %.2f | %.2f %.2f]%s",
                      kKlassName[k], v(sl.n), v(sl.not_render), v(sl.no_render), v(sl.not_scene), reinterpret_cast<void*>(s.cur),
                      reinterpret_cast<void*>(s.scene), s.w, s.h, s.rw, s.rh, s.tw, s.th, s.twt, s.tht, static_cast<unsigned long>(s.tid), s.scale[0],
                      s.scale[1], s.flag371, s.screen[0], s.screen[5], s.screen[12], s.screen[13], s.screen[14], s.screen[15],
                      got && s.ok ? "" : " (unread)");
        out += b;
    }
    for (int k = 0; k < kKlasses; ++k) {
        Slot& sl = g_slot[k];
        Sample s;
        bool got = false;
        for (int i = 0; i < 8 && !got; ++i) {
            const uint32_t a = sl.seq.load(std::memory_order_acquire);
            if (a & 1) continue;
            s = sl.s;
            got = sl.seq.load(std::memory_order_acquire) == a;
        }
        std::snprintf(b, sizeof(b),
                      " | %s: samples %llu (unread %llu), upload due at the draw %llu, the CB's bytes = PostFx +0x4b0 %llu, the effect's value = +0x4b0 %llu, "
                      "CB unmapped %llu, pass 1's upload re-bound %llu",
                      kKlassName[k], v(sl.n), v(sl.unread), v(sl.upload_due), v(sl.cpu_ok), v(sl.local_ok), v(sl.unmapped), v(sl.gpu_as_p1));
        out += b;
        std::snprintf(b, sizeof(b), "; at playback: runs %llu, draws %llu (runs with no draw %llu), predicated %llu, in a pre-pass region %llu; "
                      "the pass set the depth variable again %llu, the effect's RT = PostFx +0xf8 after it %llu (after unread %llu); last depth var: "
                      "before RT %p (+0x2c %u, +0xf8 %p), after RT %p (+0xf8 %p), CB index %#x sampler %#x, texel [%.6g %.6g %.6g %.6g]%s",
                      v(g_pb_runs[k]), v(g_pb_draws[k]), v(g_pb_nodraw[k]), v(g_pb_pred[k]), v(g_pb_region[k]), v(sl.set_again), v(sl.rt_is_f8),
                      v(sl.after_unread), reinterpret_cast<void*>(s.depth_rt_pre), s.depth_dirty_pre, reinterpret_cast<void*>(s.f8_pre),
                      reinterpret_cast<void*>(s.depth_rt), reinterpret_cast<void*>(s.f8), s.depth_cb[0], s.depth_cb[1], s.texel[0], s.texel[1], s.texel[2],
                      s.texel[3], s.have_after ? "" : " (after unread)");
        out += b;
        if (!got || !sl.n.load()) continue;
        char r0[200], r1[200], r2[200];
        row(r0, sizeof(r0), s.ndc);
        row(r1, sizeof(r1), s.local);
        row(r2, sizeof(r2), s.cbval);
        std::snprintf(b, sizeof(b),
                      "; last: frame %llu technique %d, effect %p, handle %d member %d at +%#x (%d B), CB %p storage %p mapped %p (size %d), stamp %d%s "
                      "(recorder %d%s), GPU %#llx; +0x4b0 %s; effect value %s%s; CB bytes %s%s",
                      static_cast<unsigned long long>(s.frame), s.tech, reinterpret_cast<void*>(s.eff), s.handle, s.member, s.off, s.msize,
                      reinterpret_cast<void*>(s.cb), reinterpret_cast<void*>(s.storage), reinterpret_cast<void*>(s.mapped), s.map_size, s.cb_stamp,
                      s.cb_stamp == -1 ? " (mapped: upload due)" : "", s.stamp, s.have_stamp ? "" : " (no anchor)", static_cast<unsigned long long>(s.gpu), r0,
                      r1, s.have_local ? "" : " (unread)", r2, s.have_cbval ? "" : " (unread)");
        out += b;
    }
    return out;
}

}  // namespace rdrvr::cut_velocity
