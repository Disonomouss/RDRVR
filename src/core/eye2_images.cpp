#include "core/eye2_images.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/eye2_collector.h"
#include "core/eye2_light.h"
#include "core/log.h"
#include "core/prepass_arrays.h"
#include "core/ring_probe.h"
#include "core/state.h"

namespace rdrvr::eye2_images {
std::atomic<bool> g_on_flag{false};

namespace {
using anchors::Id;

// DescAlloc FUN_140f732b0(dev, rec, heap type, partition, name, count) [V Ghidra 2026-10-22]: under EnterCriticalSection(dev
// +0x430); the record filled whole (the free list dev+0x3a0+0x10p first, matched by type and size; else the partition's
// next slot); the name is never read; it returns rec. A partition's overflow (used +0x218 > capacity +0x208, both 64-bit,
// after the count is added) writes 0x3e to address 3
using DescAlloc_t = void* (*)(uintptr_t dev, void* rec, int type, int partition, const char* name, int count);
using Rec = DescRec;  // (eye2_images.h: the record, shared with [Stereo] MainSlice since run 25 item 5)
static_assert(sizeof(Rec) == 0x28, "the descriptor record is 0x28 bytes");
// the device (*D3dDeviceSingleton): partition p at +0x1f0 + 0x40p (+0x1f0 base index, +0x1f8 CPU base, +0x200 GPU base, +0x208
// capacity, +0x210 bytes used, +0x218 count used)
constexpr uintptr_t kP1Cpu = 0x1f8 + 0x40, kP1Gpu = 0x200 + 0x40, kP1Cap = 0x208 + 0x40, kP1Used = 0x218 + 0x40;
constexpr uintptr_t kP6Cpu = 0x1f8 + 0x180, kP6Gpu = 0x200 + 0x180;
constexpr uint64_t kHeadroom = 64;
// the CB object [V FUN_140ec8bd0, FUN_140ecc150]: +0 storage, +8 layout (+0x14 the size), +0x18 mapped, +0x24 the stamp (-1:
// mapped since its last upload; the commit uploads when it differs from RecorderStamp, then sets it), +0x28 the last CBV's
// GPU handle (a partition-6 descriptor, FUN_140f83c50)
constexpr uintptr_t kCbStamp = 0x24, kCbGpu = 0x28;
constexpr uint32_t kDeclared = 0xc0, kGrown = 0x100, kAlloc = 0x100;
// the design's uints were 59 / 61 (slots 11 and 13, + 48); the ones used are the pinned pair below (images.md 9.6)
constexpr int kUFirst = 48, kUEnd = 64;         // the slack's uints (0xc0..0xff)
// images.md 9.5: the uints the shipped effects' entries could write (the sampler states t209..t213 -> uints 59..63), ORed
// into every run-time scan, so the choice never depends on the load timing; and the pair that gives, pinned for the session
// and for the converter (the free uints nearest 59 and 61 under that mask): LB2 uint 58, SC2 uint 57
constexpr uint32_t kStaticClaimed = 0x1fu << (59 - kUFirst);
constexpr int kULb = kUintLb, kUSc = kUintSc;
static_assert(!(kStaticClaimed >> (kULb - kUFirst) & 1) && !(kStaticClaimed >> (kUSc - kUFirst) & 1), "the pinned uints are free of the static mask");
// (run 25) the guard's uint, claimed as the pair is
constexpr int kUGuard = kUintGuard;
static_assert(kUGuard >= kUFirst && kUGuard < kUEnd && !(kStaticClaimed >> (kUGuard - kUFirst) & 1) && kUGuard != kULb && kUGuard != kUSc,
              "the guard's uint is in the slack, free of the static mask and not one of the pair");
constexpr int kPinned[3] = {kULb, kUSc, kUGuard};
const char* const kPinnedName[3] = {"LB2", "SC2L", "the guard"};
constexpr int kUGameLb = 11, kUGameSc = 13;     // the game's indices (LightBufferSampler t161, ShadowCollectorSampler t163)
constexpr int kMaxVars = 239;                   // GlobalTexTable's room before GlobalVarCount (0x2b0ba90 - 0x2b07700) / 0x48
// GlobalTexTable's entry for id (1-based) at table + id * 0x48 [V the loader FUN_14014d1c0 0x14df47..0x14e1b2, the lookups
// FUN_14014ffc0, SetGlobalTexture FUN_140149140; images.md 9]: -0x38 the type byte (6 a texture, 11 a sampler state, others
// constants), -0x37 the count; -0x30 the name (char*, the effect's variable name), -0x28 the second string (the name the game
// looks up, e.g. "LightBufferTex"); -0x20 / -0x14 their hashes; +0 the register (u16, from one byte of the file; written at the
// entry's creation only), +2 the flags byte (OR-merged by every later effect that declares it: not "is a texture"), +3 the
// sampler state's index, +8 / +0xc SetGlobalTexture's cached index / sampler
constexpr intptr_t kEType = -0x38, kECount = -0x37, kEName = -0x30, kESem = -0x28, kEHash = -0x20, kESemHash = -0x14;
constexpr intptr_t kEReg = 0, kEFlags = 2, kESampler = 3;
// FUN_140149140 writes an entry's index at storage + 4r - 0x258 and its sampler at 4r - 0x1f8, into global_textures always (no
// type, CB or bounds check; only its flags byte nonzero): uints r - 150 and r - 126
constexpr int kIdxBias = 150, kSmpBias = 126;
enum Kind : int { kSc = 0, kLb = 1 };
// (GOAL-single-pass item 6b) the collector is eye2_light's SC2L (eye 2's final collector, after its footprints), not
// eye2_collector's SC2 (before them)
const char* const kName[2] = {"RDRVR Shadow Collector 2 (final)", "RDRVR Light Buffer 2"};
const char* const kShort[2] = {"SC2L", "LB2"};

std::atomic<bool> g_installed{false}, g_want{false}, g_stood{false};
using Count = std::atomic<uint64_t>;
inline void bump(Count& c) { c.fetch_add(1, std::memory_order_relaxed); }
inline unsigned long long u(const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); }
char g_why[200] = "";

// the playback thread's
struct Img {
    Rec rec{};
    bool have = false;            // the record allocated (kept for the session)
    bool srv = false;             // an SRV written into it
    ID3D12Resource* res = nullptr;  // the image the SRV views (no reference held: its module's)
    uint64_t gen = ~0ull;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
Img g_im[2];
std::atomic<DWORD> g_pb_tid{0};  // (item 6b: atomic, fresh_this_frame reads it on any thread)
std::atomic<uintptr_t> g_view[2];  // (kv only) the resource image k's SRV views, as a value (0 none)
Count g_not_owner;  // a step on another thread than the image's module's (the image not used)
Count g_steps, g_other_thread, g_made, g_rewrites, g_nulled, g_headroom_refused, g_bad_desc, g_faults;
std::atomic<uint32_t> g_idx[2];  // published once the first SRV is written (release); 0 none
std::atomic<int> g_fmt[2];
std::atomic<uint64_t> g_headroom{0};

// the render thread's
std::atomic<int> g_grow{0};  // 0 not yet, 1 grown, -1 refused
uintptr_t g_cb = 0, g_block = 0;
bool g_written = false;          // the storage holds both indices
uint32_t g_last_a = 0, g_last_b = 0;  // what the LB / SC uints hold (0 before the first write)
std::atomic<int> g_ulb{-1}, g_usc{-1};  // the uints chosen at the grow (free of every runtime entry's writes); -1 none
std::atomic<uint32_t> g_claimed{0};     // bit u - 48: uint u of 48..63 some entry could write (the run time's | the files')
std::atomic<uint32_t> g_rt_claimed{0};  // the run-time scan's alone
std::atomic<uint64_t> g_high{0}, g_past{0};  // entries whose register writes at or past 0xc0 / at or past 0x100
bool g_zeroed = false;
uint64_t g_p1_gpu = 0, g_p1_frame = ~0ull;
Count g_frames, g_grows, g_grow_tries, g_still_failed, g_marks, g_mark_failed, g_writes, g_zero_writes, g_not_rt, g_tables_checked;
Count g_gt_vars;  // shader globals of global_textures below 0xc0 (reported)
std::atomic<uint32_t> g_game_lb{0}, g_game_sc{0};
Count g_rb_ok[2], g_rb_bad[2], g_no_cbv[2], g_pending[2], g_stale[2], g_bad_idx[2], g_bad_data[2];
Count g_same_cbv2;
// (run 25) the guard at uint 56. The render thread's: g_last_g (what uint 56 holds, as the module wrote it), g_seen (an upload
// carried the current indices since they last changed). Any thread's: g_guard_want ("prepass e2img guard on|off"), g_guard_valid
// (guard_valid(); a "guard off" drops it at the next pass 1's start, before that frame's draws, so both passes of a frame agree)
uint32_t g_last_g = 0;
std::atomic<bool> g_seen{false};  // (written by the render thread only; atomic for kv)
std::atomic<bool> g_guard_want{true}, g_guard_valid{false};
std::atomic<bool> g_srv_live[2];  // the playback thread's: image k's SRV views its image (false: never written or nulled)
std::atomic<uint32_t> g_guard_word{0}, g_rb_guard[2];  // uint 56 as written; as the last checked upload of pass k held it
Count g_guard_writes, g_guard_ok[2], g_bad_guard[2], g_guard_drops, g_guard_confirms, g_guard_zero_first, g_guard_zero_failed;
// (GOAL-single-pass item 6b) the frame's freshness. uint 56 is written 0 at every pass 1's start (write_indices) and the magic
// only at the rise: eye2_light's (L) in that pass 1 (the render thread, after eye 1's call, before the MRT bind 0x5c3e8d and
// so before the main walk's commits of global_textures) reports eye 2's lights recorded between IN-L and OUT-L; then this
// frame's start conditions (write_indices' ok, the start marker appended) are checked again, eye2_light's check marker is
// appended and the magic written (the stamp set: the walk's commits upload it). On the playback thread the check's step runs
// after this pass 1's OUT-L and reports whether OUT-L copied LB2 with SC2L final; pb_check then also needs both SRVs on those
// images of that generation. The render thread's: g_fseq (pass 1s begun), g_base_seq (write_indices' ok held at that frame's
// start), g_marked_seq (its start marker appended), g_rise_seq (it rose), g_counted_seq (counted at pass 1's scene end)
std::atomic<uint64_t> g_fseq{0}, g_rise_seq{~0ull};
uint64_t g_base_seq = ~0ull, g_marked_seq = ~0ull, g_counted_seq = ~0ull;
std::atomic<uint32_t> g_walk_guard{0};  // uint 56 as the storage held it at pass 1's scene end (after the walk's commits)
// (the review's should-fix 1) the frame's identity on the playback thread, carried by the markers: frame seq uses slot seq %
// kFrameSlots (g_slot_seq[slot] = seq, written by the render thread before it appends that frame's start marker; the markers
// carry the slot in their callbacks, cb_desc_t<S> and eye2_light's check<S>). The playback runs at most a frame behind the
// render thread, so a slot is not reused before its frame has played. g_pb_frame_seq: the frame whose start marker played
// last; g_pb_fresh_seq: the frame whose check found its images fresh (0 none). g_fail_seq: the last frame whose start marker
// was not appended (written before any of its draws are recorded): a frame after the played one without its marker, so the
// playback thread may be in it, reads not fresh
constexpr int kFrameSlots = eye2_light::kCheckTags;
std::atomic<uint64_t> g_slot_seq[kFrameSlots];
std::atomic<uint64_t> g_pb_frame_seq{0}, g_pb_fresh_seq{0}, g_fail_seq{0};
std::atomic<bool> g_rise_want{true};  // ("prepass e2img rise on|off", a test: the control with the modules up and no rise)
// (GOAL-single-pass item 6c, a test: "prepass e2img guard flip on|off", off by default) on: guard_valid() dropped at each pass 1's
// scene end (after its readback), so it falls mid-frame; pass 2's readback confirms it again as before. pso's latch must keep
// a frame's passes agreeing through it
std::atomic<bool> g_flip{false};
Count g_flip_drops;
Count g_fr_fresh, g_fr_stale, g_rises, g_rise_no_lights, g_rise_no_base, g_rise_no_marker, g_rise_word, g_pb_fresh_n, g_pb_stale_n, g_pb_no_lights,
    g_pb_regen, g_pb_wrong_thread, g_pb_other_frame, g_rise_off;
bool pb_fresh_now() {  // the playback thread's answer for the frame it is playing
    const uint64_t f = g_pb_frame_seq.load(std::memory_order_acquire);
    return f != 0 && g_pb_fresh_seq.load(std::memory_order_acquire) == f && g_fail_seq.load(std::memory_order_acquire) < f;
}
// guard_valid() false at once (any thread), counted once per fall
void drop_guard() {
    if (g_guard_valid.exchange(false)) bump(g_guard_drops);
}

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
bool raw_read(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// a game string (a char* at a) into out (n > 1), printable bytes only: "" if unreadable, "?" for a byte that is not
void read_str(uintptr_t a, char* out, size_t n) {
    out[0] = 0;
    uintptr_t p = 0;
    if (!rd(a, &p) || !p) return;
    size_t i = 0;
    for (; i + 1 < n; ++i) {
        char c = 0;
        if (!rd(p + i, &c) || !c) break;
        out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    out[i] = 0;
}

void stand_down(const char* why) {
    drop_guard();  // (run 25) the eye2img twins released first (pso.cpp's twin_of), then the stand-down as before
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    ring_probe::arm_cbv_map(false);
    if (!g_stood.exchange(true)) log::error("[e2img] PrepassEye2Images stood down: %s (its guard, then its two uints zeroed at the next pass 1)", why);
    drop_guard();  // (again, now that g_stood is set: a readback's confirmation in between is undone; any later one sees g_stood)
}
void refuse(Count& c, const char* why) {
    bump(c);
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    log::limited("e2img.refused", 10, "[e2img] refused: %s", why);
}

// ---- the playback thread: the records and the SRVs
// the game's DescAlloc, under SEH (no C++ objects in this frame)
// (review) a fault inside it would leave the device's critical section dev+0x430 held by this thread, and the game's commit
// ring (FUN_140f83c50) calls it from other threads: they would hang. So the handler leaves the section down to the recursion
// it had before the call, when this thread owns it. *left: how many times it was left
bool call_alloc(uintptr_t dev, Rec* rec, const char* name, int* left) {
    CRITICAL_SECTION* cs = reinterpret_cast<CRITICAL_SECTION*>(dev + 0x430);
    const DWORD me = GetCurrentThreadId();
    LONG before = 0;
    __try {
        if (static_cast<DWORD>(reinterpret_cast<uintptr_t>(cs->OwningThread)) == me) before = cs->RecursionCount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;  // (the section not readable: nothing called)
    }
    __try {
        reinterpret_cast<DescAlloc_t>(anchors::addr(Id::DescAlloc))(dev, rec, 0, 1, name, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        __try {
            for (int i = 0; i < 64 && static_cast<DWORD>(reinterpret_cast<uintptr_t>(cs->OwningThread)) == me && cs->RecursionCount > before; ++i) {
                LeaveCriticalSection(cs);
                ++*left;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return false;
    }
}
bool create_srv(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* s, uint64_t cpu) {
    __try {
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        h.ptr = static_cast<SIZE_T>(cpu);
        d->CreateShaderResourceView(r, s, h);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// the record for image k (once): false, refused or stood down
// (run 25 item 5) the route itself is alloc_p1 (shared with [Stereo] MainSlice; the same checks and reasons as before)
Alloc alloc_route(const char* name, const char* label, Rec* out, char* why, size_t why_len, bool* read, uint64_t* used_out, uint64_t* cap_out) {
    uintptr_t dev = 0, cpu1 = 0, gpu1 = 0;
    uint64_t cap = 0, used = 0;
    *read = false;
    if (!anchors::present(Id::DescAlloc) || !anchors::present(Id::D3dDeviceSingleton) || !rd(anchors::addr(Id::D3dDeviceSingleton), &dev) || !dev ||
        !rd(dev + kP1Cap, &cap) || !rd(dev + kP1Used, &used) || !rd(dev + kP1Cpu, &cpu1) || !rd(dev + kP1Gpu, &gpu1)) {
        std::snprintf(why, why_len, "the device's partition 1 not read");
        return Alloc::kRefused;
    }
    *read = true;
    *used_out = used;
    *cap_out = cap;
    if (used > cap || cap - used < kHeadroom) {  // (the allocator's overflow is a crash inside the device CS: never called then)
        std::snprintf(why, why_len, "partition 1's headroom %llu < %llu (capacity %llu, used %llu)",
                      static_cast<unsigned long long>(used <= cap ? cap - used : 0), static_cast<unsigned long long>(kHeadroom),
                      static_cast<unsigned long long>(cap), static_cast<unsigned long long>(used));
        return Alloc::kRefused;
    }
    Rec r{};
    int left = 0;
    if (!call_alloc(dev, &r, name, &left)) {
        std::snprintf(why, why_len, "a fault in DescAlloc (FUN_140f732b0); the device's critical section left %d times", left);
        return Alloc::kFault;
    }
    ID3D12Device* d = state::device.load();
    const uint32_t inc = d ? d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 0;
    if (r.type != 0 || r.partition != 1 || r.inc <= 0 || static_cast<uint32_t>(r.inc) != inc || r.size != r.inc || r.index <= 0 ||
        static_cast<uint64_t>(r.index) >= cap || !r.cpu || !r.gpu || r.cpu != cpu1 + static_cast<uint64_t>(r.index) * static_cast<uint64_t>(r.inc) ||
        r.gpu != gpu1 + static_cast<uint64_t>(r.index) * static_cast<uint64_t>(r.inc)) {  // (the review; [V] FUN_140f732b0: gpu = base +0x200 + index x inc)
        std::snprintf(why, why_len, "%s's record not as analysed (type %d, partition %d, increment %d of %u, size %d, index %d of %llu, CPU %#llx, GPU %#llx)",
                      label, r.type, r.partition, r.inc, inc, r.size, r.index, static_cast<unsigned long long>(cap),
                      static_cast<unsigned long long>(r.cpu), static_cast<unsigned long long>(r.gpu));
        return Alloc::kBadRecord;  // (the record is the game's: kept, never written)
    }
    *out = r;
    return Alloc::kOk;
}
bool alloc(int k) {
    Img& m = g_im[k];
    uint64_t cap = 0, used = 0;
    bool read = false;
    char why[200];
    Rec r{};
    const Alloc a = alloc_route(kName[k], kShort[k], &r, why, sizeof(why), &read, &used, &cap);
    if (read) g_headroom.store(used <= cap ? cap - used : 0, std::memory_order_relaxed);
    if (a == Alloc::kRefused) {
        refuse(g_headroom_refused, why);
        return false;
    }
    if (a == Alloc::kFault) bump(g_faults);
    if (a != Alloc::kOk) {
        stand_down(why);
        return false;
    }
    m.rec = r;
    m.have = true;
    log::info("[e2img] %s's descriptor: partition 1 index %d (heap index %d, CPU %#llx, GPU %#llx); partition 1 used %llu of %llu", kShort[k], r.index,
              r.heap_index, static_cast<unsigned long long>(r.cpu), static_cast<unsigned long long>(r.gpu), static_cast<unsigned long long>(used + 1),
              static_cast<unsigned long long>(cap));
    return true;
}
bool typeless(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_UNKNOWN:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC7_TYPELESS:
            return true;
        default:
            return false;
    }
}
// the SRV of r (null: a null SRV of the last format) into image k's record
bool write_srv(int k, ID3D12Resource* r) {
    Img& m = g_im[k];
    ID3D12Device* d = state::device.load();
    if (!d) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC s{};
    s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;  // 0x1688
    s.Texture2D.MostDetailedMip = 0;
    s.Texture2D.MipLevels = 1;
    if (r) {
        const D3D12_RESOURCE_DESC rd_ = r->GetDesc();
        if (rd_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd_.DepthOrArraySize != 1 || rd_.SampleDesc.Count != 1 || rd_.MipLevels < 1) {
            refuse(g_bad_desc, "an image is not a single-slice 2D texture");
            return false;
        }
        DXGI_FORMAT f = rd_.Format;
        if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
            f = k == kLb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;  // (the game's views)
        } else if (typeless(f)) {  // (no view format known for it)
            refuse(g_bad_desc, "an image's format is typeless (not B8G8R8A8) or unknown");
            return false;
        }
        m.fmt = f;
    } else if (m.fmt == DXGI_FORMAT_UNKNOWN) {
        m.fmt = k == kLb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    s.Format = m.fmt;
    if (!create_srv(d, r, &s, m.rec.cpu)) {
        bump(g_faults);
        stand_down("a fault in CreateShaderResourceView");
        return false;
    }
    g_fmt[k].store(static_cast<int>(m.fmt), std::memory_order_relaxed);
    return true;
}
void playback_step(int slot) {
    // (item 6b) a frame's start on the playback thread: this frame's seq from its slot, not fresh until its check
    g_pb_fresh_seq.store(0, std::memory_order_release);
    g_pb_frame_seq.store(g_slot_seq[slot].load(std::memory_order_acquire), std::memory_order_release);
    if (!g_installed.load(std::memory_order_relaxed) || g_stood.load()) return;
    const DWORD tid = GetCurrentThreadId();
    DWORD none = 0;
    g_pb_tid.compare_exchange_strong(none, tid);
    if (tid != g_pb_tid.load()) {  // (the images live on the thread that plays the markers: another one is not read)
        bump(g_other_thread);
        return;
    }
    bump(g_steps);
    for (int k = 0; k < 2; ++k) {
        Img& m = g_im[k];
        uint64_t gen = 0;
        unsigned long owner = 0;
        // (item 6b) uint 57: SC2L, never eye2_collector's SC2 (it lacks the footprints; no fallback: the guard needs SC2L)
        ID3D12Resource* r = k == kSc ? eye2_light::sc2l_image(&gen, &owner) : eye2_light::lb2_image(&gen, &owner);
        if (r == m.res && gen == m.gen) continue;
        if (owner == 0) continue;  // (the delta review) the module has run no step yet: nothing made, nothing to count
        if (owner != tid) {  // (review) the image is made and retired on its module's step thread: not this one, so not read
            bump(g_not_owner);
            continue;
        }
        if (!r) {  // (retired, not made again yet: the descriptor views nothing)
            g_srv_live[k].store(false);  // (run 25) the guard falls first: no eye2img twin handed out from now; 0 at pass 1's start
            drop_guard();
            if (m.srv && m.res && write_srv(k, nullptr)) bump(g_nulled);
            m.res = nullptr;
            m.gen = gen;
            g_view[k].store(0, std::memory_order_relaxed);
            continue;
        }
        if (!m.have && !alloc(k)) return;
        if (!write_srv(k, r)) {  // (run 25) refused or a fault: the descriptor is not known to view a live image, so the guard falls
            g_srv_live[k].store(false);
            drop_guard();
            return;
        }
        const bool first = !m.srv;
        m.srv = true;
        m.res = r;
        m.gen = gen;
        g_view[k].store(reinterpret_cast<uintptr_t>(r), std::memory_order_relaxed);
        g_srv_live[k].store(true);
        if (first) {
            bump(g_made);
            g_idx[k].store(static_cast<uint32_t>(m.rec.index), std::memory_order_release);
            log::info("[e2img] %s's SRV written (format %d): its bindless index %d", kShort[k], static_cast<int>(m.fmt), m.rec.index);
        } else {
            bump(g_rewrites);
        }
    }
}
template <int S>
void cb_desc_t(void*) {
    static_assert(S >= 0 && S < kFrameSlots, "a frame slot");
    playback_step(S);
}
void (*const kCbDesc[kFrameSlots])(void*) = {&cb_desc_t<0>, &cb_desc_t<1>, &cb_desc_t<2>, &cb_desc_t<3>};
static_assert(kFrameSlots == 4, "kCbDesc lists four slots");
// (item 6b) eye2_light's check step (the playback thread, after this pass 1's OUT-L, before the walk's draws play): fresh on
// the playback thread only if OUT-L copied LB2 with SC2L final, on this module's thread, and both SRVs view those very images
// of that generation (a remake inside the frame, after this frame's start step wrote the SRVs, leaves them on the retired
// ones: stale, written again at the next frame's start step, never mid-frame)
void pb_check(bool lights_ok, uint64_t gen, int tag) {
    if (!g_installed.load(std::memory_order_relaxed) || g_stood.load()) return;
    // (should-fix 1) the check's frame (its marker's slot) must be the frame whose start marker played last
    const uint64_t seq = tag >= 0 && tag < kFrameSlots ? g_slot_seq[tag].load(std::memory_order_acquire) : 0;
    bool ok = lights_ok;
    if (!ok) bump(g_pb_no_lights);
    if (ok && (seq == 0 || seq != g_pb_frame_seq.load(std::memory_order_acquire))) {
        bump(g_pb_other_frame);
        ok = false;
    }
    if (ok && GetCurrentThreadId() != g_pb_tid.load()) {  // (g_im is that thread's)
        bump(g_pb_wrong_thread);
        ok = false;
    }
    if (ok) {
        uint64_t gs = 0, gl = 0;
        unsigned long ts = 0, tl = 0;
        ID3D12Resource* sc = eye2_light::sc2l_image(&gs, &ts);
        ID3D12Resource* lb = eye2_light::lb2_image(&gl, &tl);
        ok = sc && lb && gs == gen && gl == gen && g_im[kSc].srv && g_im[kLb].srv && g_im[kSc].res == sc && g_im[kLb].res == lb && g_im[kSc].gen == gen &&
             g_im[kLb].gen == gen && g_srv_live[kSc].load() && g_srv_live[kLb].load();
        if (!ok) bump(g_pb_regen);
    }
    if (ok) g_pb_fresh_seq.store(seq, std::memory_order_release);
    bump(ok ? g_pb_fresh_n : g_pb_stale_n);
}

// ---- the render thread: global_textures
bool render_thread() {
    DWORD id = 0;
    return rd(anchors::addr(Id::RenderThreadId), &id) && id == GetCurrentThreadId();
}
// images.md 9: which uints of 48..63 the game could write. Every entry of the runtime table counts, whatever its type and
// flags (FUN_140149140 checks neither the type nor any bound, and a later effect can OR its flags byte nonzero without the
// count moving; the register is written only when the entry is made, but the loader raises the count before it fills the entry, so the whole table is rescanned every pass 1)
struct Scan {
    int n = 0;
    uint32_t claimed = 0;  // bit u - 48
    int by[kUEnd - kUFirst] = {};  // the first id claiming uint 48 + i (0 none)
    unsigned reg_of[kUEnd - kUFirst] = {};
    uint64_t gt_vars = 0, high = 0, past = 0;
    uint64_t types[3] = {};  // textures (6), sampler states (11), the others
};
// the uints entry reg r could write: the index r - 150, the sampler r - 126
inline void claim(Scan* s, int id, unsigned r) {
    const int us[2] = {static_cast<int>(r) - kIdxBias, static_cast<int>(r) - kSmpBias};
    bool high = false, past = false;
    for (int u : us) {
        if (u >= kUFirst) high = true;
        if (u >= kUEnd) past = true;
        if (u >= kUFirst && u < kUEnd) {
            if (!(s->claimed >> (u - kUFirst) & 1)) {
                s->by[u - kUFirst] = id;
                s->reg_of[u - kUFirst] = r;
            }
            s->claimed |= 1u << (u - kUFirst);
        }
    }
    if (high) ++s->high;
    if (past) ++s->past;
}
// nullptr, or why the table is not as analysed (cb: global_textures, 0 if not made yet)
const char* scan_why(uintptr_t cb, Scan* s) {
    static thread_local char why[160];  // (the render thread's gate and the command thread's dump)
    int n = -1;
    if (!rd(anchors::addr(Id::GlobalVarCount), &n) || n < 0 || n > kMaxVars) {
        std::snprintf(why, sizeof(why), "the global variables' count %d is not as analysed (0..%d)", n, kMaxVars);
        return why;
    }
    s->n = n;
    const uintptr_t tex = anchors::addr(Id::GlobalTexTable), sg = anchors::addr(Id::ShaderGlobalTable);
    for (int id = 1; id <= n; ++id) {
        const uintptr_t e = tex + static_cast<uintptr_t>(id) * 0x48;
        uint16_t reg = 0;
        uint8_t type = 0;
        if (!rd(e + kEReg, &reg) || !rd(e + kEType, &type)) return "the global variable table not read";
        ++s->types[type == 6 ? 0 : type == 11 ? 1 : 2];
        claim(s, id, reg);
        uintptr_t c = 0;
        int off = 0;
        if (!rd(sg + static_cast<uintptr_t>(id) * 16, &c) || (cb && c == cb && !rd(sg + static_cast<uintptr_t>(id) * 16 + 8, &off)))
            return "the shader global table not read";
        if (cb && c == cb) {
            if (off < 0 || off >= static_cast<int>(kDeclared)) {  // (SetShaderGlobal 0x1490a0 would write there)
                std::snprintf(why, sizeof(why), "the shader global %d is in global_textures at +%#x", id, off);
                return why;
            }
            ++s->gt_vars;
        }
    }
    return nullptr;
}
void publish(const Scan& s) {
    g_rt_claimed.store(s.claimed, std::memory_order_relaxed);
    g_claimed.store(s.claimed | kStaticClaimed, std::memory_order_relaxed);  // (as avoided: the run time's and the files')
    g_high.store(s.high, std::memory_order_relaxed);
    g_past.store(s.past, std::memory_order_relaxed);
    g_gt_vars.store(s.gt_vars, std::memory_order_relaxed);
    bump(g_tables_checked);
}
int free_count(uint32_t mask) {
    int n = 0;
    for (int u = kUFirst; u < kUEnd; ++u) n += (mask >> (u - kUFirst) & 1) ? 0 : 1;
    return n;
}
// nullptr, or which run-time entry could write a pinned uint (s.claimed: the scan's, without the static mask)
const char* pinned_why(const Scan& s) {
    static thread_local char why[160];
    for (int k = 0; k < 3; ++k) {  // (run 25: the guard's uint 56 too)
        const int uu = kPinned[k];
        if (s.claimed >> (uu - kUFirst) & 1) {
            std::snprintf(why, sizeof(why), "the global variable %d (register %u) could write uint %d, %s's pinned uint", s.by[uu - kUFirst], s.reg_of[uu - kUFirst],
                          uu, kPinnedName[k]);
            return why;
        }
    }
    return nullptr;
}
std::string uint_list(uint32_t mask, bool set) {
    std::string r;
    for (int u = kUFirst; u < kUEnd; ++u)
        if (((mask >> (u - kUFirst)) & 1) == (set ? 1u : 0u)) r += (r.empty() ? "" : ",") + std::to_string(u);
    return r.empty() ? "none" : r;
}
void refuse_grow(const char* why) {
    g_grow.store(-1);
    stand_down(why);
}
bool try_grow() {
    uintptr_t cb = 0, layout = 0;
    uint32_t size = 0;
    if (!rd(anchors::addr(Id::GlobalTexturesCb), &cb) || !cb || !rd(cb + 0x08, &layout) || !layout || !rd(layout + 0x14, &size)) {
        bump(g_grow_tries);  // (made at the first effect load that declares it: again next frame)
        return false;
    }
    if (size != kDeclared) {
        char why[120];
        std::snprintf(why, sizeof(why), "global_textures' declared size is %#x, not 0xc0", size);
        refuse_grow(why);
        return false;
    }
    Scan s;
    if (const char* why = scan_why(cb, &s)) {
        refuse_grow(why);
        return false;
    }
    publish(s);
    // (images.md 9.6) the pinned pair, never moved: a run-time entry on either is a refusal (no other choice; a grow past
    // 0x100 changes the CBV's rounded size, which prepass::grow_cb does not do)
    const int ulb = kULb, usc = kUSc;
    if (const char* why = pinned_why(s)) {
        refuse_grow(why);
        return false;
    }
    uintptr_t block = 0;
    if (!prepass::grow_cb(cb, kDeclared, kGrown, kAlloc, &block)) {
        refuse_grow("global_textures not grown (prepass::grow_cb refused: a read failed or the size changed)");
        return false;
    }
    g_cb = cb;
    g_block = block;
    g_ulb.store(ulb);
    g_usc.store(usc);
    g_grow.store(1);
    bump(g_grows);
    log::info("[e2img] global_textures %#llx grown 0xc0 -> 0x100 (storage %#llx; the CBV stays 0x100; %d global variables scanned, %llu of its own "
              "below 0xc0; uints the table could write: %s): uint %d for LB2, uint %d for SC2",
              static_cast<unsigned long long>(cb), static_cast<unsigned long long>(block), s.n, static_cast<unsigned long long>(s.gt_vars),
              uint_list(s.claimed, true).c_str(), ulb, usc);
    return true;
}
// 0 into the two uints once (the NullResource), each only while it still holds the module's value (a uint another writer
// took is left), the stamp set if the CB is still the grown one
void zero_out() {
    drop_guard();  // (run 25: already down by stand_down; again, whoever calls)
    if (g_zeroed) return;
    g_zeroed = true;
    g_written = false;
    g_seen = false;
    g_on_flag.store(false, std::memory_order_relaxed);  // (nothing more to do in this session)
    const int ua = g_ulb.load(), ub = g_usc.load();
    if (g_grow.load() <= 0 || !g_block || ua < 0 || ub < 0) return;
    if (g_last_g || g_last_a || g_last_b) {
        bool wrote = false;
        uint32_t v = 0;
        // (run 25) the guard first: 0 at uint 56 where it still holds the module's magic, before the indices are zeroed. If uint 56
        // is not read, or holds the magic and is not written, the indices are left as they are (still the live images'
        // descriptors, kept for the session): zeroing them under a live magic would point eye 2 at heap index 0
        if (g_last_g) {
            const uintptr_t gat = g_block + static_cast<uintptr_t>(kUGuard) * 4;
            const bool read = rd(gat, &v);
            if (read && v == g_last_g && wr<uint32_t>(gat, 0)) {
                wrote = true;
                bump(g_guard_writes);
                bump(g_guard_zero_first);
            } else if (!read || v == g_last_g) {
                bump(g_guard_zero_failed);
                bump(g_faults);  // (zero_out runs only once stood down: g_why keeps the stand-down's own reason)
                log::error("[e2img] uint %d (the guard) not zeroed at the stand-down: uints %d/%d left as they are", kUGuard, ua, ub);
                return;
            }
        }
        g_last_g = 0;
        g_guard_word.store(0, std::memory_order_relaxed);
        if (g_last_a && rd(g_block + static_cast<uintptr_t>(ua) * 4, &v) && v == g_last_a) wrote |= wr<uint32_t>(g_block + static_cast<uintptr_t>(ua) * 4, 0);
        if (g_last_b && rd(g_block + static_cast<uintptr_t>(ub) * 4, &v) && v == g_last_b) wrote |= wr<uint32_t>(g_block + static_cast<uintptr_t>(ub) * 4, 0);
        g_last_a = g_last_b = 0;
        if (wrote && prepass::cb_still_grown(g_cb, g_block, kGrown)) wr<int>(g_cb + kCbStamp, -1);
        if (wrote) bump(g_zero_writes);
    }
    log::info("[e2img] stood down: uint %d (the guard) zeroed first, then uints %d/%d, where they still held the module's values (the NullResource); the "
              "descriptors kept", kUGuard, ua, ub);
}
void write_indices() {
    const bool want = g_want.load(std::memory_order_relaxed);
    const uint32_t lb = g_idx[kLb].load(std::memory_order_acquire), sc = g_idx[kSc].load(std::memory_order_acquire);
    const int ua = g_ulb.load(), ub = g_usc.load();
    uint32_t s[64];
    if (ua < kUFirst || ua >= kUEnd || ub < kUFirst || ub >= kUEnd || ua == ub) {
        stand_down("the chosen uints are not two of 48..63");
        return;
    }
    if (!raw_read(g_block, s, sizeof(s))) {
        bump(g_faults);
        stand_down("global_textures' storage not read");
        return;
    }
    g_game_lb.store(s[kUGameLb], std::memory_order_relaxed);
    g_game_sc.store(s[kUGameSc], std::memory_order_relaxed);
    if (s[ua] != g_last_a || s[ub] != g_last_b || s[kUGuard] != g_last_g) {  // (images.md 9: no runtime entry writes them; any write there is a stand-down)
        char why[200];
        std::snprintf(why, sizeof(why), "uints %d/%d/%d hold %u/%u/%#x, not the module's %u/%u/%#x (another writer)", ua, ub, kUGuard, s[ua], s[ub], s[kUGuard],
                      g_last_a, g_last_b, g_last_g);
        if (s[ua] != g_last_a) g_last_a = 0;  // (review: that uint is the other writer's: zero_out leaves it, and zeroes the other)
        if (s[ub] != g_last_b) g_last_b = 0;
        if (s[kUGuard] != g_last_g) g_last_g = 0;
        stand_down(why);
        return;
    }
    uint32_t a = 0, b = 0;
    if (want && lb && sc) {
        if (lb == sc || lb == s[kUGameLb] || lb == s[kUGameSc] || sc == s[kUGameLb] || sc == s[kUGameSc]) {
            char why[160];
            std::snprintf(why, sizeof(why), "an index is the game's or both are one (LB2 %u, SC2 %u; the game's LB %u, SC %u)", lb, sc, s[kUGameLb],
                          s[kUGameSc]);
            stand_down(why);
            return;
        }
        a = lb;
        b = sc;
    }
    // (run 25) the guard: the magic only while the grow is live (frame_begin checked cb_still_grown just before), both SRVs view
    // their images, the indices are unchanged by this call and an upload has carried them since they changed (g_seen), the
    // module is up and "guard" is on; else 0 (and the guard dropped). The 0 is written before the indices change, the magic (item
    // 6b: at the rise) after
    const bool change = s[ua] != a || s[ub] != b;
    if (change) g_seen = false;
    // (item 6b) and SC2L's and LB2's modules usable (no fallback to SC2). The magic itself is written only at this pass 1's rise
    // (rise(), after eye 2's lights): uint 56 is 0 from every pass 1's start, so a frame that does not prove fresh uploads 0
    const bool ok = g_guard_want.load() && a && b && !change && g_seen && g_srv_live[kLb].load() && g_srv_live[kSc].load() && !g_stood.load() &&
                    g_grow.load() == 1 && eye2_light::usable() && eye2_collector::usable();
    if (!ok) drop_guard();  // (at once: the eye2img twins not handed out from this pass 1 on)
    bool dirty = false;
    const uintptr_t gat = g_block + static_cast<uintptr_t>(kUGuard) * 4;
    if (s[kUGuard] != 0) {
        if (!wr<uint32_t>(gat, 0)) {
            bump(g_faults);
            stand_down("global_textures' guard not written");
            return;
        }
        g_last_g = 0;
        g_guard_word.store(0, std::memory_order_relaxed);
        bump(g_guard_writes);
        dirty = true;
    }
    if (change) {
        if (!wr<uint32_t>(g_block + static_cast<uintptr_t>(ua) * 4, a) || !wr<uint32_t>(g_block + static_cast<uintptr_t>(ub) * 4, b)) {
            bump(g_faults);
            stand_down("global_textures not written");
            return;
        }
        if (!g_last_a && a)
            log::info("[e2img] global_textures: uint %d = %u (LB2), uint %d = %u (SC2); the game's LB %u, SC %u", ua, a, ub, b, s[kUGameLb], s[kUGameSc]);
        g_last_a = a;
        g_last_b = b;
        bump(g_writes);
        dirty = true;
    }
    if (dirty && !wr<int>(g_cb + kCbStamp, -1)) {
        bump(g_faults);
        stand_down("global_textures' stamp not written");
        return;
    }
    g_written = a != 0;
    if (ok) g_base_seq = g_fseq.load(std::memory_order_relaxed);  // (item 6b: this frame may rise)
}
// (item 6b) the rise: eye2_light's (L) in pass 1, the render thread, after eye 1's call and before the MRT bind and the main
// walk. eye2_ran: eye 2's lights recorded between IN-L and OUT-L this pass 1 (so e2sc's step ran, and (F)'s when the game drew
// footprints). The magic is written once per frame, only on this frame's start conditions checked again; else uint 56 stays
// 0 for this frame's uploads
void rise(bool eye2_ran) {
    if (!g_installed.load(std::memory_order_relaxed) || g_stood.load() || !g_want.load(std::memory_order_relaxed)) return;
    if (!render_thread()) {
        bump(g_not_rt);
        return;
    }
    const uint64_t seq = g_fseq.load(std::memory_order_relaxed);
    if (g_rise_seq.load(std::memory_order_relaxed) == seq) return;  // (once a pass 1: 0x6e0450's one caller)
    if (!eye2_ran) {
        bump(g_rise_no_lights);
        return;
    }
    if (!g_rise_want.load()) {  // (the test's control: SC2L's and LB2's modules up, no rise: uint 56 stays 0, the frame stale)
        bump(g_rise_off);
        return;
    }
    uintptr_t now = 0;
    if (g_base_seq != seq || g_marked_seq != seq || !g_written || !g_guard_want.load() || !g_seen || !g_srv_live[kLb].load() || !g_srv_live[kSc].load() ||
        g_grow.load() != 1 || !eye2_light::usable() || !eye2_collector::usable() || !rd(anchors::addr(Id::GlobalTexturesCb), &now) || now != g_cb ||
        !prepass::cb_still_grown(g_cb, g_block, kGrown)) {
        bump(g_rise_no_base);  // (a CB made again: the next pass 1's check stands down)
        return;
    }
    const int ua = g_ulb.load(), ub = g_usc.load();
    const uintptr_t gat = g_block + static_cast<uintptr_t>(kUGuard) * 4;
    uint32_t va = 0, vb = 0, vg = 1;
    if (ua < kUFirst || ua >= kUEnd || ub < kUFirst || ub >= kUEnd || !rd(g_block + static_cast<uintptr_t>(ua) * 4, &va) ||
        !rd(g_block + static_cast<uintptr_t>(ub) * 4, &vb) || !rd(gat, &vg) || va != g_last_a || vb != g_last_b || vg != 0 || g_last_g != 0) {
        bump(g_rise_word);  // (another writer: the next pass 1's check stands down)
        return;
    }
    if (!eye2_light::check_marker(static_cast<int>(seq % kFrameSlots))) {  // (the playback's check, behind this pass 1's OUT-L; this frame's slot)
        bump(g_rise_no_marker);
        return;
    }
    if (!wr<uint32_t>(gat, kGuardMagic)) {
        bump(g_faults);
        stand_down("global_textures' guard not written (the rise)");
        zero_out();
        return;
    }
    g_last_g = kGuardMagic;
    g_guard_word.store(kGuardMagic, std::memory_order_relaxed);
    bump(g_guard_writes);
    if (!wr<int>(g_cb + kCbStamp, -1)) {
        bump(g_faults);
        stand_down("global_textures' stamp not written (the rise)");
        zero_out();
        return;
    }
    g_rise_seq.store(seq, std::memory_order_release);
    bump(g_rises);
    log::limited("e2img.guard", 10, "[e2img] global_textures: uint %d = %#x after eye 2's lights (the frame's rise: SC2L and LB2 recorded this pass 1; the "
                 "indices %u/%u seen in an upload; both SRVs live)", kUGuard, kGuardMagic, g_last_a, g_last_b);
}
// (item 6b) pass 2's end, after its readback: uint 56 back to 0 (the guard_valid() state kept), so what follows the frame (its
// UI, a mono frame) never uploads a frame's magic; the next pass 1's start finds it 0. A word not the module's is left to that
// start's check (a stand-down)
void fall() {
    if (g_stood.load() || g_last_g != kGuardMagic || !g_block || g_grow.load() != 1) return;
    const uintptr_t gat = g_block + static_cast<uintptr_t>(kUGuard) * 4;
    uint32_t v = 0;
    if (!rd(gat, &v) || v != g_last_g) return;
    if (!wr<uint32_t>(gat, 0) || !prepass::cb_still_grown(g_cb, g_block, kGrown) || !wr<int>(g_cb + kCbStamp, -1)) {
        bump(g_faults);
        stand_down("global_textures' guard not written back to 0 (pass 2's end)");
        zero_out();
        return;
    }
    g_last_g = 0;
    g_guard_word.store(0, std::memory_order_relaxed);
    bump(g_guard_writes);
}
// (item 6b) pass 1's scene end, before the readback: the frame counted fresh (it rose) or stale; uint 56 as the walk left it
void count_frame() {
    const uint64_t seq = g_fseq.load(std::memory_order_relaxed);
    if (!g_want.load(std::memory_order_relaxed) || g_stood.load() || g_counted_seq == seq || !g_block) return;
    g_counted_seq = seq;
    uint32_t v = 0;
    g_walk_guard.store(rd(g_block + static_cast<uintptr_t>(kUGuard) * 4, &v) ? v : 0xffffffffu, std::memory_order_relaxed);
    bump(g_rise_seq.load(std::memory_order_relaxed) == seq ? g_fr_fresh : g_fr_stale);
}
void frame_body(uint64_t seq, bool* marked);
// (should-fix 1) a frame whose start marker is not appended (off, refused, stood down, a failed append) is recorded in
// g_fail_seq before any of its draws: the playback thread never reads it fresh
void frame_begin() {
    bump(g_frames);
    const uint64_t seq = g_fseq.fetch_add(1, std::memory_order_relaxed) + 1;  // (item 6b: a new frame, not fresh until its rise)
    bool marked = false;
    frame_body(seq, &marked);
    if (!marked) g_fail_seq.store(seq, std::memory_order_release);
}
void frame_body(uint64_t seq, bool* marked) {
    if (g_stood.load()) {
        zero_out();
        return;
    }
    if (g_grow.load() == 0 && !try_grow()) {
        if (g_stood.load()) zero_out();
        return;
    }
    uintptr_t now = 0;
    if (!rd(anchors::addr(Id::GlobalTexturesCb), &now) || now != g_cb || !prepass::cb_still_grown(g_cb, g_block, kGrown)) {
        bump(g_still_failed);
        stand_down("global_textures was made again by the game (its object, storage or declared size changed)");
        zero_out();
        return;
    }
    {  // (review) every entry rescanned each pass 1, not at a count change: the loader moves the count (0x14df47) before it
       // writes the new entry's register, so a scan in between reads 0 for it (at most 239 entries, a few microseconds)
        Scan s;
        const char* why = scan_why(g_cb, &s);
        if (!why) {
            publish(s);
            why = pinned_why(s);
        }
        if (why) {
            stand_down(why);
            zero_out();
            return;
        }
    }
    if (g_want.load(std::memory_order_relaxed)) {  // (off: no marker, no playback step)
        const int slot = static_cast<int>(seq % kFrameSlots);
        g_slot_seq[slot].store(seq, std::memory_order_release);  // (before the marker: its callback reads it)
        if (prepass::stream_marker(kCbDesc[slot])) {
            bump(g_marks);
            g_marked_seq = seq;  // (item 6b: the playback's frame start, before this frame's check)
            *marked = true;
        } else {
            bump(g_mark_failed);
        }
    }
    write_indices();
    if (g_stood.load()) zero_out();
}
// the CB's last upload against the storage: pass k (0 pass 1's scene end, 1 pass 2's end)
void readback(int k) {
    if (!g_want.load(std::memory_order_relaxed) || !g_written || g_stood.load()) return;
    const uint64_t f = state::presents.load();
    int32_t st = 0, stamp = 0;
    uint64_t gpu = 0, cpu6 = 0, gpu6 = 0;
    uintptr_t dev = 0;
    if (!rd(g_cb + kCbStamp, &st) || !rd(anchors::addr(Id::RecorderStamp), &stamp) || !rd(g_cb + kCbGpu, &gpu) ||
        !rd(anchors::addr(Id::D3dDeviceSingleton), &dev) || !dev || !rd(dev + kP6Cpu, &cpu6) || !rd(dev + kP6Gpu, &gpu6)) {
        bump(g_no_cbv[k]);
        return;
    }
    if (st == -1) {  // (mapped since its last upload: the storage may differ from it)
        bump(g_pending[k]);
        return;
    }
    if (st != stamp) {  // (not uploaded in this frame's recording: its ring bytes may be reused)
        bump(g_stale[k]);
        return;
    }
    uint64_t va = 0;
    uint32_t size = 0;
    const uint8_t* p = nullptr;
    if (!gpu || gpu < gpu6 || !ring_probe::cbv_of(gpu - gpu6 + cpu6, &va, &size) || size < kGrown || !(p = ring_probe::cpu_of(va, kGrown))) {
        bump(g_no_cbv[k]);
        return;
    }
    uint32_t up[64], s[64];
    if (!raw_read(reinterpret_cast<uintptr_t>(p), up, sizeof(up)) || !raw_read(g_block, s, sizeof(s))) {
        bump(g_no_cbv[k]);
        return;
    }
    const uint32_t lb = g_idx[kLb].load(std::memory_order_relaxed), sc = g_idx[kSc].load(std::memory_order_relaxed);
    const int ua = g_ulb.load(), ub = g_usc.load();
    if (ua < kUFirst || ua >= kUEnd || ub < kUFirst || ub >= kUEnd) {
        bump(g_no_cbv[k]);
        return;
    }
    const bool idx_ok = up[ua] == lb && up[ub] == sc;
    const bool data_ok = !std::memcmp(up, s, 48 * 4);
    // (run 25) uint 56 against what the module wrote; the indices as written (g_last_a/b: lb/sc while written)
    const bool guard_ok = up[kUGuard] == g_last_g;
    const bool mine = idx_ok && up[ua] == g_last_a && up[ub] == g_last_b;
    g_rb_guard[k].store(up[kUGuard], std::memory_order_relaxed);
    if (guard_ok) bump(g_guard_ok[k]);
    else bump(g_bad_guard[k]);
    if (mine && guard_ok) {
        g_seen = true;  // (the magic may be written from the next pass 1)
        // the confirmation: valid set first, then every condition read again (seq_cst), so a stand-down or an SRV nulled on
        // another thread in between (each clears its condition before it drops the guard) is never overwritten
        if (g_last_g == kGuardMagic && !g_guard_valid.exchange(true)) {
            if (!g_guard_want.load() || g_stood.load() || !g_srv_live[kLb].load() || !g_srv_live[kSc].load() || !eye2_light::usable() ||
                !eye2_collector::usable())  // (item 6b: SC2L's and LB2's modules too)
                drop_guard();
            else bump(g_guard_confirms);
        }
    } else {
        g_seen = false;  // (an upload not as written: the guard falls, and is written 0 at the next pass 1)
        drop_guard();
    }
    if (idx_ok && data_ok && guard_ok) {
        bump(g_rb_ok[k]);
    } else {
        bump(g_rb_bad[k]);
        if (!idx_ok) bump(g_bad_idx[k]);
        if (!data_ok) bump(g_bad_data[k]);
        log::limited("e2img.bad", 10, "[e2img] pass %d's upload: uint %d %u (want %u), uint %d %u (want %u), uint %d %#x (want %#x), uints 0-47 %s the storage's",
                     k + 1, ua, up[ua], lb, ub, up[ub], sc, kUGuard, up[kUGuard], g_last_g, data_ok ? "=" : "differ from");
    }
    if (k == 0) {
        g_p1_gpu = gpu;
        g_p1_frame = f;
    } else if (g_p1_frame == f && gpu == g_p1_gpu) {
        bump(g_same_cbv2);  // (pass 2 drew with pass 1's upload: no game write in between)
    }
}

std::string kv() {
    char b[2400];
    std::snprintf(b, sizeof(b),
                  "on=%d stood=%d grow=%d made=%llu idx_lb=%u idx_sc=%u game_lb=%u game_sc=%u fmt_lb=%d fmt_sc=%d headroom=%llu "
                  "headroom_refused=%llu rewrites=%llu nulled=%llu bad_desc=%llu steps=%llu other_thread=%llu not_owner=%llu frames=%llu grows=%llu grow_tries=%llu "
                  "still_failed=%llu marks=%llu mark_failed=%llu writes=%llu zero_writes=%llu not_rt=%llu tables_checked=%llu gt_vars=%llu "
                  "readback_ok1=%llu readback_bad1=%llu no_cbv1=%llu pending1=%llu stale1=%llu bad_idx1=%llu bad_data1=%llu readback_ok2=%llu "
                  "readback_bad2=%llu no_cbv2=%llu pending2=%llu stale2=%llu bad_idx2=%llu bad_data2=%llu same_cbv2=%llu faults=%llu cbv_map=%d "
                  "cbvs_mapped=%llu written=%d uint_lb=%d uint_sc=%d claimed=%u runtime_claimed=%u static_claimed=%u pinned=1 free=%d high=%llu past=%llu",
                  g_want.load() ? 1 : 0, g_stood.load() ? 1 : 0, g_grow.load(), u(g_made), g_idx[kLb].load(), g_idx[kSc].load(), g_game_lb.load(),
                  g_game_sc.load(), g_fmt[kLb].load(), g_fmt[kSc].load(), static_cast<unsigned long long>(g_headroom.load()), u(g_headroom_refused),
                  u(g_rewrites), u(g_nulled), u(g_bad_desc), u(g_steps), u(g_other_thread), u(g_not_owner), u(g_frames), u(g_grows), u(g_grow_tries), u(g_still_failed),
                  u(g_marks), u(g_mark_failed), u(g_writes), u(g_zero_writes), u(g_not_rt), u(g_tables_checked), u(g_gt_vars), u(g_rb_ok[0]),
                  u(g_rb_bad[0]), u(g_no_cbv[0]), u(g_pending[0]), u(g_stale[0]), u(g_bad_idx[0]), u(g_bad_data[0]), u(g_rb_ok[1]), u(g_rb_bad[1]),
                  u(g_no_cbv[1]), u(g_pending[1]), u(g_stale[1]), u(g_bad_idx[1]), u(g_bad_data[1]), u(g_same_cbv2), u(g_faults),
                  ring_probe::cbv_map_on() ? 1 : 0, static_cast<unsigned long long>(ring_probe::cbvs_mapped()), g_written ? 1 : 0, g_ulb.load(), g_usc.load(),
                  g_claimed.load(), g_rt_claimed.load(), kStaticClaimed, free_count(g_claimed.load()), static_cast<unsigned long long>(g_high.load()),
                  static_cast<unsigned long long>(g_past.load()));
    // (run 25) the guard at uint 56, in decimal (the magic 0x32455652 is 843404882)
    // guard= the module's record of what it wrote; store_guard= uint 56 read from the storage now (SEH; -1 not read)
    const uintptr_t blk = g_block;
    uint32_t sg = 0;
    const long long store_guard = blk && rd(blk + static_cast<uintptr_t>(kUGuard) * 4, &sg) ? static_cast<long long>(sg) : -1;
    char gb[700];
    std::snprintf(gb, sizeof(gb),
                  " uint_guard=%d guard=%u store_guard=%lld guard_zero_failed=%llu guard_want=%d guard_valid=%d guard_writes=%llu guard_confirms=%llu "
                  "guard_drops=%llu guard_zero_first=%llu seen=%d srv_lb=%d srv_sc=%d rb_guard1=%u rb_guard2=%u guard_ok1=%llu bad_guard1=%llu guard_ok2=%llu "
                  "bad_guard2=%llu",
                  kUGuard, g_guard_word.load(), store_guard, u(g_guard_zero_failed), g_guard_want.load() ? 1 : 0, g_guard_valid.load() ? 1 : 0, u(g_guard_writes), u(g_guard_confirms),
                  u(g_guard_drops), u(g_guard_zero_first), g_seen.load() ? 1 : 0, g_srv_live[kLb].load() ? 1 : 0, g_srv_live[kSc].load() ? 1 : 0,
                  g_rb_guard[0].load(), g_rb_guard[1].load(), u(g_guard_ok[0]), u(g_bad_guard[0]), u(g_guard_ok[1]), u(g_bad_guard[1]));
    // (item 6b) the freshness. fresh/stale: frames counted at pass 1's scene end; walk_guard: uint 56 there; rises and why not;
    // pb_*: the playback thread's checks; sc_view/lb_view: the resources the SRVs view (as values; uint 57's is SC2L, sc_src=2)
    char fb[900];
    std::snprintf(fb, sizeof(fb),
                  " fresh=%llu stale=%llu walk_guard=%u rises=%llu rise_no_lights=%llu rise_no_base=%llu rise_no_marker=%llu rise_word=%llu pb_fresh=%llu "
                  "pb_stale=%llu pb_no_lights=%llu pb_regen=%llu pb_wrong_thread=%llu fresh_rt=%d pb_fresh_now=%d sc_src=2 sc_view=%llu lb_view=%llu "
                  "pb_other_frame=%llu rise_off=%llu rise_want=%d pb_frame=%llu fail_seq=%llu fseq=%llu flip=%d flip_drops=%llu",
                  u(g_fr_fresh), u(g_fr_stale), g_walk_guard.load(), u(g_rises), u(g_rise_no_lights), u(g_rise_no_base), u(g_rise_no_marker), u(g_rise_word),
                  u(g_pb_fresh_n), u(g_pb_stale_n), u(g_pb_no_lights), u(g_pb_regen), u(g_pb_wrong_thread),
                  g_rise_seq.load() == g_fseq.load() ? 1 : 0, pb_fresh_now() ? 1 : 0, static_cast<unsigned long long>(g_view[kSc].load()),
                  static_cast<unsigned long long>(g_view[kLb].load()), u(g_pb_other_frame), u(g_rise_off), g_rise_want.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_pb_frame_seq.load()), static_cast<unsigned long long>(g_fail_seq.load()),
                  static_cast<unsigned long long>(g_fseq.load()), g_flip.load() ? 1 : 0, u(g_flip_drops));
    return std::string(b) + gb + fb;
}
// "prepass e2img table" (images.md 9): the runtime global variable table, one log line per entry, and a summary. Read only,
// every read under SEH; it needs only the ini switch and the anchors (not the install's other gates), so it also answers when
// the module refused or stood down
std::string table_dump() {
    if (config::get_int("Stereo", "PrepassEye2Images", 0) != 1) return "e2img table: [Stereo] PrepassEye2Images is not 1 (nothing read)";
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated()) return "e2img table: not the analysed RDR.exe build (nothing read)";
    for (Id id : {Id::GlobalTexturesCb, Id::GlobalTexTable, Id::GlobalVarCount, Id::ShaderGlobalTable})
        if (!anchors::present(id)) return std::string("e2img table: the anchor ") + anchors::name(id) + " is absent (nothing read)";
    uintptr_t cb = 0, layout = 0;
    uint32_t size = 0;
    if (rd(anchors::addr(Id::GlobalTexturesCb), &cb) && cb && rd(cb + 0x08, &layout) && layout) rd(layout + 0x14, &size);
    Scan s;
    const char* why = scan_why(cb, &s);
    const uintptr_t tex = anchors::addr(Id::GlobalTexTable), sg = anchors::addr(Id::ShaderGlobalTable);
    std::string over;  // the entries whose register writes at or past 0xc0
    int listed = 0;
    for (int id = 1; id <= s.n; ++id) {
        const uintptr_t e = tex + static_cast<uintptr_t>(id) * 0x48;
        uint16_t reg = 0;
        uint8_t type = 0, count = 0, flags = 0, smp = 0;
        uint32_t h = 0, hs = 0;
        int32_t cache_i = 0, cache_s = 0;
        uintptr_t c = 0;
        int off = 0;
        rd(e + kEReg, &reg);
        rd(e + kEType, &type);
        rd(e + kECount, &count);
        rd(e + kEFlags, &flags);
        rd(e + kESampler, &smp);
        rd(e + kEHash, &h);
        rd(e + kESemHash, &hs);
        rd(e + 8, &cache_i);
        rd(e + 0xc, &cache_s);
        rd(sg + static_cast<uintptr_t>(id) * 16, &c);
        rd(sg + static_cast<uintptr_t>(id) * 16 + 8, &off);
        char name[64], sem[64];
        read_str(e + kEName, name, sizeof(name));
        read_str(e + kESem, sem, sizeof(sem));
        const int ui = static_cast<int>(reg) - kIdxBias, us = static_cast<int>(reg) - kSmpBias;
        log::info("[e2img] table %d: type %u count %u register %u flags %#x sampler %u cache %d/%d name \"%s\" (%#x) second \"%s\" (%#x) cb %#llx%s +%#x; "
                  "SetGlobalTexture would write uints %d / %d",
                  id, type, count, reg, flags, smp, cache_i, cache_s, name, h, sem, hs, static_cast<unsigned long long>(c), cb && c == cb ? " (global_textures)" : "",
                  off, ui, us);
        if ((ui >= kUFirst || us >= kUFirst) && listed < 16) {
            char one[112];
            std::snprintf(one, sizeof(one), "%s%d:t%u:type%u:flags%#x:%s", over.empty() ? "" : " ", id, reg, type, flags, name[0] ? name : "?");
            over += one;
            ++listed;
        }
    }
    const char* pin = why ? nullptr : pinned_why(s);
    char b[1800];
    std::snprintf(b, sizeof(b),
                  "e2img table: pinned LB2 uint %d, SC2 uint %d, the guard uint %d (%s); in use LB %d, SC %d%s%s | uints 48..63 the run-time table could write: %s; the files' "
                  "(static): %s; free of both: %s | %d entries (textures %llu, sampler states %llu, others %llu) | global_textures cb %#llx, declared %#x, its "
                  "own shader globals %llu | registers writing at or past 0xc0: %llu (past 0x100: %llu): %s%s | one log line per entry",
                  kULb, kUSc, kUGuard, why ? "not checked" : pin ? pin : "free at run time", g_ulb.load(), g_usc.load(), why ? "; NOT AS ANALYSED: " : "", why ? why : "",
                  uint_list(s.claimed, true).c_str(), uint_list(kStaticClaimed, true).c_str(), uint_list(s.claimed | kStaticClaimed, false).c_str(), s.n, static_cast<unsigned long long>(s.types[0]), static_cast<unsigned long long>(s.types[1]), static_cast<unsigned long long>(s.types[2]),
                  static_cast<unsigned long long>(cb), size, static_cast<unsigned long long>(s.gt_vars),
                  static_cast<unsigned long long>(s.high), static_cast<unsigned long long>(s.past), over.empty() ? "none" : over.c_str(),
                  s.high > static_cast<uint64_t>(listed) ? " ..." : "");
    log::info("[e2img] %s", b);
    return b;
}
void reset() {  // (a test: the counts from now; made, the stand-down's counts and the faults kept)
    for (Count* c : {&g_steps, &g_other_thread, &g_not_owner, &g_frames, &g_marks, &g_mark_failed, &g_writes, &g_not_rt, &g_same_cbv2, &g_rewrites, &g_nulled})
        c->store(0);
    for (int k = 0; k < 2; ++k)
        for (Count* c : {&g_rb_ok[k], &g_rb_bad[k], &g_no_cbv[k], &g_pending[k], &g_stale[k], &g_bad_idx[k], &g_bad_data[k], &g_guard_ok[k], &g_bad_guard[k]})
            c->store(0);
    for (Count* c : {&g_guard_writes, &g_guard_confirms, &g_guard_drops}) c->store(0);  // (run 25; guard_zero_first kept, as the stand-down's)
    for (Count* c : {&g_fr_fresh, &g_fr_stale, &g_rises, &g_rise_no_lights, &g_rise_no_base, &g_rise_no_marker, &g_rise_word, &g_pb_fresh_n, &g_pb_stale_n,
                     &g_pb_no_lights, &g_pb_regen, &g_pb_wrong_thread, &g_pb_other_frame, &g_rise_off})
        c->store(0);  // (item 6b)
}
}  // namespace

// (run 25 item 5) the shared route ([Stereo] MainSlice): the module's own checks, its state untouched
Alloc alloc_p1(const char* name, const char* label, DescRec* out, char* why, size_t why_len, bool* read, uint64_t* used, uint64_t* cap) {
    return alloc_route(name, label, out, why, why_len, read, used, cap);
}
bool create_srv_at(ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* s, uint64_t cpu) {
    ID3D12Device* d = state::device.load();
    return d && cpu && create_srv(d, r, s, cpu);
}

void mark(int point) {
    if (!g_installed.load(std::memory_order_relaxed)) return;
    if (!render_thread()) {
        bump(g_not_rt);
        return;
    }
    if (point == 1) frame_begin();
    else if (point == 2) {
        count_frame();  // (item 6b)
        readback(0);
        if (g_flip.load(std::memory_order_relaxed) && g_guard_valid.load()) {  // (item 6c, the test's flip)
            drop_guard();
            bump(g_flip_drops);
        }
    }
    else if (point == 4) {
        readback(1);
        fall();  // (item 6b)
    }
}

void install() {
    const int want = config::get_int("Stereo", "PrepassEye2Images", 0);
    if (want == 0) return;
    if (want != 1) {
        log::warn("[e2img] PrepassEye2Images=%d is not 1: off", want);
        return;
    }
    if (config::get_int("Stereo", "PrepassEye2Collector", 0) != 1 || !eye2_collector::usable() || config::get_int("Stereo", "PrepassEye2Light", 0) != 1 ||
        !eye2_light::usable()) {
        log::warn("[e2img] PrepassEye2Images needs [Stereo] PrepassEye2Collector=1 and PrepassEye2Light=1 installed (eye2_light's SC2L and LB2): off");
        return;
    }
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated()) {
        log::warn("[e2img] PrepassEye2Images runs on the analysed RDR.exe build only: off");
        return;
    }
    for (Id id : {Id::DescAlloc, Id::GlobalTexturesCb, Id::GlobalTexTable, Id::GlobalVarCount, Id::ShaderGlobalTable, Id::RecorderStamp,
                  Id::D3dDeviceSingleton, Id::RenderThreadId})
        if (!anchors::present(id)) {
            log::warn("[e2img] the anchor %s is absent on this build: off", anchors::name(id));
            return;
        }
    const bool map = ring_probe::arm_cbv_map(true);
    g_installed = true;
    g_want = true;
    g_on_flag = true;
    // (GOAL-single-pass item 6b) the frame's rise after eye 2's lights (the render thread) and the playback's check after OUT-L;
    // only here: with PrepassEye2Images unset eye2_light calls neither
    eye2_light::set_fresh_sink(&pb_check);
    eye2_light::set_lights_done(&rise);
    log::info("[e2img] [Stereo] PrepassEye2Images on (test only): LB2's and SC2L's (eye 2's final collector) bindless indices from the game's descriptor "
              "allocator (partition 1), written at global_textures uints 58 / 57 and the guard %#x at uint 56 (pinned: free of the effect files' and the "
              "run-time table's writes; grown 0xc0 -> 0x100), the guard only on frames whose pass 1 made both before the walk (0 from each pass 1's start); "
              "only [Stereo] PrepassEye2Shaders' twins read them, while the guard is valid; the readback through ring_probe's CBV map (%s)",
              kGuardMagic, map ? "armed" : "off: no readback, so the guard is never written (the ini had not asked for its hook at the device's creation)");
}

std::string command(const std::vector<std::string>& t) {
    if (t.size() >= 3 && t[2] == "table") return table_dump();  // (read only: also when not installed, refused or stood down)
    if (!g_installed.load())
        return "e2img: not installed (start with [Stereo] PrepassEye2Images=1, PrepassEye2Collector=1 and PrepassEye2Light=1 and their switches, on the "
               "analysed RDR.exe)";
    if (t.size() >= 3) {
        const std::string& w = t[2];
        if (w == "kv") return kv();
        if (w == "on" || w == "off") {
            if (w == "on" && g_stood.load()) return "ERROR stood down (" + std::string(g_why) + "): restart the game";
            // off, from the next pass 1: the two uints written 0 once (then nothing written), no marker and no playback step (no
            // SRV written again for a new image), no readback. Kept: the grow (the storage stays the 0x100 block: 64 B more an
            // upload), cb_still_grown and the tables' checks each pass 1, the descriptors, the CBV map's writes
            g_want = w == "on";
        } else if (w == "reset") {
            reset();
        } else if (w == "rise" && t.size() >= 4 && (t[3] == "on" || t[3] == "off")) {
            // (item 6b, a test: the freshness control with every module up) off: from the next pass 1 no rise, so uint 56 stays 0
            // for the walk's uploads and the frames count stale (guard_valid() kept: only fresh_this_frame() is false); on: back
            g_rise_want = t[3] == "on";
        } else if (w == "guard" && t.size() >= 5 && t[3] == "flip" && (t[4] == "on" || t[4] == "off")) {
            // (item 6c, a test) guard_valid() dropped at each pass 1's scene end, confirmed again by pass 2's readback
            g_flip = t[4] == "on";
            log::info("[e2img] guard flip %s (test only: the guard falls at each pass 1's scene end)", t[4].c_str());
        } else if (w == "guard" && t.size() >= 4 && (t[3] == "on" || t[3] == "off")) {
            // (run 25, a test: the guard's positive control) off: from the next pass 1, guard_valid() false (pso.cpp's eye2img twins
            // not handed out) and 0 written at uint 56; on: the magic written again once the readback has seen the indices
            if (t[3] == "on" && g_stood.load()) return "ERROR stood down (" + std::string(g_why) + "): restart the game";
            g_guard_want = t[3] == "on";
        } else {
            return "ERROR prepass e2img [on|off|reset|kv|table|guard on|guard off|guard flip on|off|rise on|rise off]";
        }
    }
    const int gs = g_grow.load();
    char b[1600];
    std::snprintf(b, sizeof(b),
                  "e2img %s: SRVs made %llu (LB2 index %u, SC2 index %u; the game's LB %u, SC %u; formats %d / %d), written again %llu, nulled %llu, "
                  "partition 1 headroom %llu (refused %llu) | global_textures %s, uints %d/%d %s (pinned; writes %llu; avoided, the table's and the files': %s) | readback pass 1: ok %llu, bad %llu, "
                  "no CBV %llu, dirty %llu, not this frame's %llu; pass 2: ok %llu, bad %llu, no CBV %llu, dirty %llu, not this frame's %llu (pass "
                  "1's upload %llu) | markers %llu (failed %llu), playback steps %llu, faults %llu, CBV map %s | last refusal: %s",
                  g_want.load() ? "on" : g_stood.load() ? "stood down" : "off", u(g_made), g_idx[kLb].load(), g_idx[kSc].load(), g_game_lb.load(),
                  g_game_sc.load(), g_fmt[kLb].load(), g_fmt[kSc].load(), u(g_rewrites), u(g_nulled),
                  static_cast<unsigned long long>(g_headroom.load()), u(g_headroom_refused), gs > 0 ? "grown 0xc0 -> 0x100" : gs < 0 ? "refused" : "not grown",
                  g_ulb.load(), g_usc.load(), g_written ? "written" : "0", u(g_writes), uint_list(g_claimed.load(), true).c_str(), u(g_rb_ok[0]), u(g_rb_bad[0]), u(g_no_cbv[0]), u(g_pending[0]), u(g_stale[0]), u(g_rb_ok[1]),
                  u(g_rb_bad[1]), u(g_no_cbv[1]), u(g_pending[1]), u(g_stale[1]), u(g_same_cbv2), u(g_marks), u(g_mark_failed), u(g_steps), u(g_faults),
                  ring_probe::cbv_map_on() ? "on" : "off", g_why[0] ? g_why : "none");
    char gb[400];  // (run 25) the guard
    std::snprintf(gb, sizeof(gb), " | guard uint %d: %#x (%s; %s), written %llu, confirmed %llu, fell %llu; last uploads pass 1 %#x, pass 2 %#x (as written %llu / %llu, not %llu / %llu)",
                  kUGuard, g_guard_word.load(), g_guard_want.load() ? "on" : "off", g_guard_valid.load() ? "valid" : "not valid", u(g_guard_writes),
                  u(g_guard_confirms), u(g_guard_drops), g_rb_guard[0].load(), g_rb_guard[1].load(), u(g_guard_ok[0]), u(g_guard_ok[1]), u(g_bad_guard[0]),
                  u(g_bad_guard[1]));
    char fb[400];  // (item 6b) the freshness
    std::snprintf(fb, sizeof(fb), " | frames fresh %llu, stale %llu (rises %llu; not: no eye-2 lights %llu, the frame's start %llu, the check marker %llu, uint "
                  "56's word %llu); the playback's checks fresh %llu, stale %llu (no LB2 / SC2L %llu, SRVs not on them %llu, thread %llu)",
                  u(g_fr_fresh), u(g_fr_stale), u(g_rises), u(g_rise_no_lights), u(g_rise_no_base), u(g_rise_no_marker), u(g_rise_word), u(g_pb_fresh_n),
                  u(g_pb_stale_n), u(g_pb_no_lights), u(g_pb_regen), u(g_pb_wrong_thread));
    return std::string(b) + gb + fb;
}

bool guard_valid() { return g_guard_valid.load(std::memory_order_acquire); }
namespace {
bool g_gz_held = false;  // (the render thread) guard_zero(true) holds uint 56 at 0
}  // namespace
bool guard_zero(bool zero) {
    if (!g_installed.load(std::memory_order_relaxed) || g_stood.load() || !g_block || g_grow.load() != 1 || !render_thread()) return false;
    const uintptr_t gat = g_block + static_cast<uintptr_t>(kUGuard) * 4;
    uint32_t v = 0;
    if (!rd(gat, &v) || !prepass::cb_still_grown(g_cb, g_block, kGrown)) return false;
    if (zero) {
        if (g_gz_held || v != kGuardMagic || g_last_g != kGuardMagic) return false;  // (no risen guard this frame: not a control frame)
        if (!wr<uint32_t>(gat, 0)) return false;
        if (!wr<int>(g_cb + kCbStamp, -1)) {  // (the review: nothing left done on a failure)
            wr<uint32_t>(gat, kGuardMagic);
            return false;
        }
        g_gz_held = true;
        return true;
    }
    if (!g_gz_held) return true;
    g_gz_held = false;
    if (v != 0) return false;  // (another writer: left to the next pass 1's start check)
    return wr<uint32_t>(gat, kGuardMagic) && wr<int>(g_cb + kCbStamp, -1);
}
// (item 6b) this frame's freshness, per thread (eye2_images.h)
bool fresh_this_frame() {
    if (!g_installed.load(std::memory_order_relaxed) || g_stood.load() || !g_want.load(std::memory_order_relaxed)) return false;
    // the render thread: its own frame (g_fseq) rose
    if (render_thread()) return g_rise_seq.load(std::memory_order_acquire) == g_fseq.load(std::memory_order_relaxed);
    // (should-fix 1) the playback thread: the frame it plays (its start marker's seq), never the render thread's current one
    const DWORD pb = g_pb_tid.load();
    return pb && GetCurrentThreadId() == pb && pb_fresh_now();
}
int health() {
    if (!g_installed.load()) return 0;
    return g_stood.load() || g_grow.load() < 0 ? -1 : 1;
}

}  // namespace rdrvr::eye2_images
