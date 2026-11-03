#include "core/inst_prev_mirror.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/prepass_arrays.h"
#include "core/pso.h"
#include "core/state.h"
#include "core/taa.h"

namespace rdrvr::inst_prev {
namespace {
using anchors::Id;

// The per-draw instance buffer object (FUN_140159fc0: 0x150 bytes and its arrays from the frame arena DAT_142ac1e98;
// rmcInstDrawModel FUN_14087d950 publishes it at 0x14087dd7a..ddb3, Ghidra 2026-10-21) [V]: +0x00 the count of entries,
// +0x10 the staging entries, +0x28 the layout (+0x14 the declared size, n x 64), +0x88 the CB object, +0x140 its storage
// pointer (n x 64). The CB object (FUN_140ec8840): +0x00 storage, +0x08 layout, +0x18 mapped (0), +0x24 the upload stamp
// (-1 at the publish: the draw's commit FUN_140ecc5e0 uploads layout +0x14 bytes from +0x00 there and then).
constexpr uintptr_t kObjCb = 0x88, kObjLayout = 0x28, kObjStorage = 0x140;
constexpr uintptr_t kCbStorage = 0x00, kCbLayout = 0x08, kCbMapped = 0x18, kCbStamp = 0x24, kLayoutSize = 0x14;
// PassBind's two instanced callers, their return addresses (the call +5): rmcInstDrawModelUnskinned 0x14087de0e and
// ...Skinned 0x14087e3e8 [V]; every other bind passes straight through
constexpr uintptr_t kRetUnskinned = 0x87de13, kRetSkinned = 0x87e3ed;
constexpr uint32_t kMaxBytes = 65536;   // a CBV's limit (4096 rows)
constexpr uint32_t kMaxN = 999;         // the game's own cap (CMP R15D,0x3e8; JGE at 0x14087dc5c)
constexpr uint64_t kFrameCap = 2u << 20;  // the bytes mirrored a walk (the game's upload ring has no capacity check)
constexpr int kRing = 8;                // blocks in flight: a block is read by its draw's commit, right after the bind

using PassBind_t = void (*)(void*, void*, void*, void*, void*);
using PrevWvp_t = float* (*)(const void* vp, float* out, const float* w);
PassBind_t o_bind = nullptr;
PrevWvp_t g_prev_wvp = nullptr;
uintptr_t g_c38 = 0, g_c48 = 0, g_a18 = 0, g_a20 = 0;  // the globals' addresses
uintptr_t g_ret1 = 0, g_ret2 = 0;

alignas(16) unsigned char g_ring[kRing][kMaxBytes];  // (static: never allocated)
alignas(16) float g_wbuf[kMaxN * 16];                // the draw's rage_instancemtx entries, copied (W_i)
int g_ring_next = 0;

std::atomic<bool> g_installed{false}, g_mirror{false}, g_observe{false}, g_compare{false}, g_notaa{false};
std::atomic<bool> g_live_pending{false};  // "prepass instprev on": the twins handed out from the next pass 1's walk
std::atomic<int> g_walk{0};       // 1 pass 1's walk open, 2 pass 2's (the compare, the markers), 0 none
std::atomic<DWORD> g_tid{0};      // the open walk's thread
uint32_t g_k = 512;

// the render thread only (the walk's state)
const void* g_vp2 = nullptr;       // the eye-2 viewport of this walk (null: none)
bool g_e2_ok = false, g_rows_ok = false;
bool g_rec = false;                // pass 1's walk records for the compare
uint64_t g_rec_frame = ~0ull;      // the frame pass 1 recorded the compare's table in
bool g_marks = false;              // this walk marks its draws for the playback (the mirror on: its twins live)
uint64_t g_mark_frame = ~0ull;     // the frame pass 1 marked (pass 2 then marks by pass 1's mirrored draws)
bool g_cmp2 = false;               // this pass 2 walk compares
bool g_last_ok = false;            // the last draw's mark (a second bind of the same draw repeats it)
uint32_t g_gen = 0, g_dgen = 0;
uint64_t g_walk_bytes = 0;
uint32_t g_walk_draws = 0, g_walk_inst = 0;
std::atomic<uint64_t> g_taa_frame{~0ull};  // the frame of the last set_eye2 that came from taa.cpp's eye-2 update

// the playback thread: the last marker's verdict for the draws after it (prepass's before_draw reads it)
std::atomic<bool> g_pb_ok{false};
void mark_ok_cb(void*) { g_pb_ok.store(true, std::memory_order_relaxed); }
void mark_no_cb(void*) { g_pb_ok.store(false, std::memory_order_relaxed); }

// single writer (the render thread): load + store; the status reads from another thread
using Count = std::atomic<uint64_t>;
inline void bump(Count& c, uint64_t by = 1) { c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed); }
inline void maxs(Count& c, uint64_t v) {
    if (v > c.load(std::memory_order_relaxed)) c.store(v, std::memory_order_relaxed);
}
Count g_walks{0}, g_ready{0}, g_no_vp2{0}, g_no_taa{0}, g_no_rows{0};
Count g_seen{0}, g_mirrored{0}, g_mirrored_inst{0}, g_as_eye1{0}, g_bad{0}, g_again{0}, g_cap{0}, g_over_k{0}, g_fault{0};
Count g_bytes{0}, g_bytes_max{0}, g_frame_bytes_max{0}, g_n_max{0};
Count g_marks_ok{0}, g_marks_no{0}, g_marks_failed{0}, g_p2_marked_ok{0}, g_p2_marked_no{0}, g_keys_full{0};
Count g_ctx_bad{0}, g_nocb{0}, g_cut_skips{0};
Count g_nocb_empty{0}, g_nocb_nonempty{0};  // (run 20, nocb.md 4B) of the nocb binds: both counts and c48 0 (an empty draw), or not
// "not the game's object" by the check that failed, per pass (1, 2) and caller (unskinned 0x87de13, skinned 0x87e3ed)
enum Why { kWhyStorage, kWhyCount, kWhyLayout, kWhySize, kWhyMapped, kWhyStamp, kWhyA18, kWhyC48, kWhyN, kWhyW, kWhys };
const char* const kWhyName[kWhys] = {"storage", "count", "layout", "size", "mapped", "stamp", "a18", "a20c48", "n", "w"};
Count g_why[2][2][kWhys];
Count g_alloc_over{0};
// (GOAL-single-pass-2 item 1) pass 1's draws left untouched (no copy, no CB swap) because the walk was not ready; walks with no eye-2
// TAA update while notaa was off (the stand-down's reason); the largest allocation seen and the largest refused over K; the eye-2
// TAA updates at the last reset (the status shows those since)
Count g_skip_unready{0}, g_no_taa_off{0}, g_alloc_max{0}, g_alloc_max_over_k{0};
Count g_m_skip_unready{0}, g_m_alloc_max_over_k{0};  // (the main walk's: left untouched when not ready; the largest alloc refused over K)
std::atomic<uint64_t> g_e2u_base{0};  // draws whose block was allocated for more entries than were filled (size > n x 64): mirrored over the allocation
int g_caller = 0;       // the bind being looked at: 0 unskinned, 1 skinned (the render thread)  // (SF-1) another recorder's thread context; (SF-4) a bind without c38; (SF-3) cuts skipped
uint64_t g_refused_frame = ~0ull;  // (SF-3) the frame pass 1 left an instanced draw of the walk unmirrored (its twin not drawn)
Count g_other{0}, g_any{0}, g_any_n_max{0};  // (fetch_add: other threads too)
constexpr int kBuckets = 7;
const uint32_t kBucketTop[kBuckets] = {16, 64, 128, 256, 512, 768, kMaxN};
Count g_hist[kBuckets];
Count g_walk_draws_max{0}, g_walk_inst_max{0};
// the compare
Count g_cmp_walks{0}, g_cmp_draws{0}, g_cmp_inst{0}, g_matched{0}, g_exact{0}, g_unmatched{0}, g_over{0}, g_full{0}, g_cmp_bad{0};
std::atomic<float> g_worst{0.0f}, g_worst_rel{0.0f};
constexpr unsigned kSlots = 8192, kProbe = 32;
struct Slot {
    uint32_t gen;
    float w[16], e2[16];
};
Slot g_tab[kSlots];  // (static: 1 MB of zeroes, never allocated)
// pass 1's mirrored draws, by a hash of their W block and n (pass 2 drops its own copy of such a draw only)
constexpr unsigned kKeys = 2048;
struct Key {
    uint32_t gen;
    uint64_t key;
};
Key g_keys[kKeys];

// ---- [Stereo] PrepassInstPrevMain (GOAL-single-pass item 8d, research\run25\item8d.md 2; test only, off by default): the mirror
// over the main walk's DVL call 1 (inst_prev_mirror.h main_open). Its own state below; the ring of blocks and the W scratch are
// shared with the pre-pass walk (the same render thread; the two walks never overlap)
constexpr uint64_t kMainCap = 1u << 20;  // the bytes mirrored a main walk (its own cap: kFrameCap is the pre-pass's)
std::atomic<bool> g_main_inst{false};    // [Stereo] PrepassInstPrevMain=1 at start (with PrepassInstPrevMirror=1)
std::atomic<bool> g_main_on{false}, g_main_cmp{false};  // "prepass instprev main on|off|compare on|off" (off at start)
std::atomic<bool> g_main_broken{false};  // a "no" marker of the main walk not recorded: the main mirror broken for the session
std::atomic<bool> g_main{false};         // the open walk (g_walk) is the main walk's
// the render thread only (the main walk's state)
const void* g_mvp2 = nullptr;
bool g_me2_ok = false, g_mrows_ok = false, g_mlast_ok = false, g_mrec = false;
int g_mlast_why = 0;                // (a second bind of the same draw repeats the first one's marker)
uint64_t g_main_bytes = 0;
uint64_t g_mrec_frame = ~0ull;      // the frame the main walk's pass 1 recorded the main compare's table in
uint64_t g_walk1_frame = ~0ull;     // the frame of the pre-pass walk's pass 1 open (its eye-2 state reused by main_open)
uint32_t g_mgen = 0;
Slot g_mtab[kSlots];  // the main compare's table (static, never allocated; its own generation: the pre-pass's is pass 2's to compare)
// the playback thread: the main walk's open marker (live, or why not) and the last main "no" marker's reason (draw_no_why)
std::atomic<bool> g_pb_main_live{false};
std::atomic<int> g_pb_main_why{5}, g_pb_why{0};
// single writer (the render thread) except g_m_other (other threads): kv main_*
Count g_m_opens1{0}, g_m_opens2{0}, g_m_live{0}, g_m_off{0}, g_m_no_vp2{0}, g_m_no_taa{0}, g_m_no_rows{0}, g_m_not_opened{0}, g_m_reused{0},
    g_m_close_e1{0}, g_m_close_fb{0}, g_m_seen{0}, g_m_mirrored{0}, g_m_mirrored_inst{0}, g_m_as_eye1{0}, g_m_over_k{0}, g_m_cap{0},
    g_m_bad{0}, g_m_again{0}, g_m_fault{0}, g_m_nocb_empty{0}, g_m_nocb_nonempty{0}, g_m_marks_ok{0}, g_m_marks_no{0}, g_m_marks_failed{0},
    g_m_n_max{0}, g_m_alloc_over{0}, g_m_bytes{0}, g_m_walk_bytes_max{0}, g_m_ctx_bad{0}, g_m_cmp_walks{0}, g_m_cmp_draws{0}, g_m_cmp_inst{0},
    g_m_matched{0}, g_m_exact{0}, g_m_unmatched{0}, g_m_over{0}, g_m_cmp_bad{0}, g_m_full{0};
Count g_m_other{0};      // (fetch_add: the other threads' binds during a main walk)
Count g_m_why[2][kWhys];  // pass 1's "not the game's object" by caller (unskinned, skinned) and the check that failed
std::atomic<float> g_m_worst{0.0f}, g_m_worst_rel{0.0f};
// (item 8e, test only) the ip control: the command's switch; the render thread's request (main_inst, a control frame's DVL call 1) and
// the main walk's latch (pass 1's main_open .. main_close); eye 2's mirrored rows nudged by kMainNudge on every element
constexpr float kMainNudge = 0.01f;
std::atomic<bool> g_main_nudge_want{false};
bool g_mnudge_req = false, g_mnudge = false;
Count g_m_nudge_walks{0}, g_m_nudged{0};
void main_ok_cb(void*) {
    g_pb_ok.store(true, std::memory_order_relaxed);
    g_pb_why.store(kNoNone, std::memory_order_relaxed);
}
template <int W>
void main_no_cb(void*) {
    g_pb_ok.store(false, std::memory_order_relaxed);
    g_pb_why.store(W, std::memory_order_relaxed);
}
// the main walk's open: the verdict "not" until a draw's own marker plays (SF-4's reset), and the walk's live latch (W 0) or why not
template <int W>
void main_open_cb(void*) {
    g_pb_ok.store(false, std::memory_order_relaxed);
    g_pb_why.store(kNoMark, std::memory_order_relaxed);
    g_pb_main_why.store(W, std::memory_order_relaxed);
    g_pb_main_live.store(W == 0, std::memory_order_relaxed);
}
using MarkFn = void (*)(void*);
constexpr MarkFn kMainNo[kNoWhys] = {&main_no_cb<kNoMark>, &main_no_cb<kNoMark>, &main_no_cb<kNoBad>, &main_no_cb<kNoOverK>, &main_no_cb<kNoCap>,
                                     &main_no_cb<kNoFault>, &main_no_cb<kNoAsEye1>, &main_no_cb<kNoNocb>, &main_no_cb<kNoCtx>};
constexpr MarkFn kMainOpen[6] = {&main_open_cb<0>, &main_open_cb<1>, &main_open_cb<2>, &main_open_cb<3>, &main_open_cb<4>, &main_open_cb<5>};

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
// the game's own previous WVP (FUN_140132640, a .pdata function that saves xmm6-15): vp's +0x371 clear leaves out as is
bool prev_wvp(const void* vp, float* out, const float* w) {
    __try {
        g_prev_wvp(vp, out, w);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool is_ring(uintptr_t p) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(&g_ring[0][0]);
    return p >= a && p < a + sizeof(g_ring);
}

inline uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x9E3779B97F4A7C15ull; }
inline uint32_t hash_w(const float* w) {
    uint64_t q[8], h = 0;
    std::memcpy(q, w, sizeof(q));
    for (uint64_t v : q) h = mix(h, v);
    return static_cast<uint32_t>(h ^ (h >> 32));
}
uint64_t draw_key(const float* w, uint32_t n, uint64_t salt) {
    uint64_t h = mix(mix(0x51ed270b27e1d3a5ull, n), salt);  // (salt: PassBind's pass, VS and PS program entries and its 5th argument)
    const uint64_t* q = reinterpret_cast<const uint64_t*>(w);
    for (uint32_t i = 0; i < n * 8; ++i) h = mix(h, q[i]);
    return h ? h : 1;
}
void key_put(uint64_t k) {
    uint32_t i = static_cast<uint32_t>(k ^ (k >> 32));
    for (unsigned p = 0; p < kProbe; ++p, ++i) {
        Key& s = g_keys[i & (kKeys - 1)];
        if (s.gen == g_dgen && s.key != k) continue;
        s.gen = g_dgen;
        s.key = k;
        return;
    }
    bump(g_keys_full);
}
bool key_has(uint64_t k) {
    uint32_t i = static_cast<uint32_t>(k ^ (k >> 32));
    for (unsigned p = 0; p < kProbe; ++p, ++i) {
        const Key& s = g_keys[i & (kKeys - 1)];
        if (s.gen != g_dgen) return false;
        if (s.key == k) return true;
    }
    return false;
}
// (item 8d) a compare table and its generation: the pre-pass's (g_tab, g_gen) or the main walk's (g_mtab, g_mgen)
void record_in(Slot* tab, uint32_t gen, const float* w, const float* e2, Count& full) {
    uint32_t i = hash_w(w);
    for (unsigned p = 0; p < kProbe; ++p, ++i) {
        Slot& s = tab[i & (kSlots - 1)];
        if (s.gen == gen && std::memcmp(s.w, w, 64) != 0) continue;
        s.gen = gen;
        std::memcpy(s.w, w, 64);
        std::memcpy(s.e2, e2, 64);
        return;
    }
    bump(full);
}
const Slot* find_in(const Slot* tab, uint32_t gen, const float* w) {
    uint32_t i = hash_w(w);
    for (unsigned p = 0; p < kProbe; ++p, ++i) {
        const Slot& s = tab[i & (kSlots - 1)];
        if (s.gen != gen) return nullptr;
        if (!std::memcmp(s.w, w, 64)) return &s;
    }
    return nullptr;
}
void record(const float* w, const float* e2) { record_in(g_tab, g_gen, w, e2, g_full); }
const Slot* find(const float* w) { return find_in(g_tab, g_gen, w); }
void fmax_store(std::atomic<float>& a, float v) {
    if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}
// the playback's verdict for the draw being bound: its twin drawn (pass 1) / its pass-2 copy dropped only when ok
void mark(bool ok) {
    if (!prepass::stream_marker(ok ? &mark_ok_cb : &mark_no_cb)) {
        bump(g_marks_failed);
        return;
    }
    bump(ok ? g_marks_ok : g_marks_no);
}

// One instanced draw's objects, checked against what rmcInstDrawModel publishes [V]; false: not that (refused, untouched)
struct Draw {
    uintptr_t cb = 0, obj = 0, layout = 0, storage = 0, w = 0;
    uint32_t n = 0;      // the entries filled (the object's count; the draw's instances index these)
    uint32_t alloc = 0;  // the entries allocated (layout +0x14 / 64, uploaded by the game: n <= alloc <= 999)
    int why = -1;        // kNotOurs: the check that failed
};
enum Look { kOk, kNotOurs, kAgain };
Look look(uintptr_t cb, Draw* d) {
    d->cb = cb;
    d->obj = cb - kObjCb;
    uintptr_t storage = 0, objstor = 0, layout = 0, mapped = 1, c48 = 0, p = 0, w = 0;
    uint32_t n = 0, size = 0, n20 = 0;
    int stamp = 0;
    const auto no = [d](int why) {
        d->why = why;
        return kNotOurs;
    };
    if (!rd(cb + kCbStorage, &storage) || !storage) return no(kWhyStorage);
    if (is_ring(storage)) return kAgain;  // (this draw's block already swapped: a second bind before its commit)
    if (!rd(d->obj, &n) || n == 0 || n > kMaxN) return no(kWhyCount);
    if (!rd(cb + kCbLayout, &layout) || layout != d->obj + kObjLayout) return no(kWhyLayout);
    // the declared size is the allocation's (FUN_140159fc0: n_alloc x 64), the count the entries filled (the publish copies
    // count x 64 into the storage): a fill that skips instances leaves size > n x 64, which the game uploads whole
    if (!rd(layout + kLayoutSize, &size) || size < n * 64 || size % 64 != 0 || size > kMaxN * 64) return no(kWhySize);
    if (!rd(d->obj + kObjStorage, &objstor) || objstor != storage) return no(kWhyStorage);
    if (!rd(cb + kCbMapped, &mapped) || mapped != 0) return no(kWhyMapped);
    if (!rd(cb + kCbStamp, &stamp) || stamp != -1) return no(kWhyStamp);
    if (!rd(g_a18, &p) || p != d->obj) return no(kWhyA18);  // DAT_142ac0a18: this draw's previous object
    if (!rd(g_c48, &c48) || !c48 || !rd(g_a20, &p) || p != c48 - kObjCb) return no(kWhyC48);
    if (!rd(c48 - kObjCb, &n20) || n20 != n) return no(kWhyN);
    if (!rd(c48 + kCbStorage, &w) || !w) return no(kWhyW);
    d->layout = layout;
    d->storage = storage;
    d->w = w;
    d->n = n;
    d->alloc = size / 64;
    return kOk;
}

void observe_n(uint32_t n) {
    for (int b = 0; b < kBuckets; ++b)
        if (n <= kBucketTop[b]) {
            bump(g_hist[b]);
            break;
        }
    maxs(g_n_max, n);
    ++g_walk_draws;
    g_walk_inst += n;
    maxs(g_walk_draws_max, g_walk_draws);
    maxs(g_walk_inst_max, g_walk_inst);
}

// pass 1's walk: the mirror (and the compare's record); true: this draw's eye-2 rows all written (its mark)
bool pass1(uintptr_t cb, uint64_t salt) {
    bump(g_seen);
    Draw d;
    const Look l = look(cb, &d);
    if (l == kAgain) {
        bump(g_again);
        return g_last_ok;  // (the same draw: the block in place is the one its first bind made)
    }
    if (l != kOk) {
        bump(g_bad);
        if (d.why >= 0) bump(g_why[0][g_caller][d.why]);
        return false;
    }
    if (g_observe.load(std::memory_order_relaxed)) observe_n(d.n);
    const bool mirror = g_mirror.load(std::memory_order_relaxed);
    if (!mirror && !g_rec) return false;  // (observe only: nothing written)
    // (GOAL-single-pass-2 item 1) the walk not ready: the draw left as the game made it (no copy, no CB swap: eye 1 from the game's own
    // instance CB), its twin not drawn; as eye 1's (g_as_eye1) is only a mirrored block's fault now
    if (mirror && !(g_e2_ok && g_rows_ok)) {
        bump(g_skip_unready);
        return false;
    }
    const uint32_t n = d.n, K = g_k;
    if (!raw(d.w, g_wbuf, static_cast<size_t>(n) * 64)) {
        bump(g_fault);
        return false;
    }
    if (!mirror) {  // the compare alone: eye 2's entries made the mirror's way, kept, nothing written to the game
        alignas(16) float e1[16], e2[16];
        for (uint32_t i = 0; i < n; ++i) {
            if (!raw(d.storage + static_cast<uintptr_t>(i) * 64, e1, 64)) {
                bump(g_fault);
                return false;
            }
            std::memcpy(e2, e1, 64);
            if (!prev_wvp(g_vp2, e2, g_wbuf + i * 16)) {
                bump(g_fault);
                return false;
            }
            record(g_wbuf + i * 16, e2);
        }
        return false;
    }
    // eye 2's rows [K, K + alloc) must not overlap eye 1's [0, alloc) and must fit 64 KB: else refused, left as the game's
    // (single). alloc: the game uploads alloc x 64 bytes, its rows [n, alloc) as the storage holds them; eye 2's copy the same
    const uint32_t alloc = d.alloc;
    maxs(g_alloc_max, alloc);
    if (alloc > K || static_cast<uint64_t>(K) + alloc > kMaxBytes / 64) {  // (before any copy or write: the game's CB untouched)
        bump(g_over_k);
        maxs(g_alloc_max_over_k, alloc);
        return false;
    }
    if (alloc > n) bump(g_alloc_over);
    const uint32_t size = (K + alloc) * 64;
    if (g_walk_bytes + size > kFrameCap) {
        bump(g_cap);  // (left as the game's: its twin not drawn)
        return false;
    }
    unsigned char* blk = g_ring[g_ring_next];
    if (!raw(d.storage, blk, static_cast<size_t>(alloc) * 64)) {  // rows [0, alloc): eye 1's, as the game uploads them
        bump(g_fault);
        return false;
    }
    const bool e2 = g_e2_ok && g_rows_ok;
    bool fault = false;
    for (uint32_t i = 0; i < alloc; ++i) {
        float* dst = reinterpret_cast<float*>(blk + static_cast<size_t>(K + i) * 64);
        std::memcpy(dst, blk + static_cast<size_t>(i) * 64, 64);  // eye 1's first (vp2 +0x371 clear: kept; [n, alloc): as eye 1's)
        if (i < n && e2 && !fault) {
            if (!prev_wvp(g_vp2, dst, g_wbuf + i * 16)) {
                fault = true;
                std::memcpy(dst, blk + static_cast<size_t>(i) * 64, 64);
            } else if (g_rec) {
                record(g_wbuf + i * 16, dst);
            }
        }
    }
    if (!wr<uintptr_t>(d.cb + kCbStorage, reinterpret_cast<uintptr_t>(blk))) {
        bump(g_fault);
        return false;
    }
    if (!wr<uint32_t>(d.layout + kLayoutSize, size)) {
        wr<uintptr_t>(d.cb + kCbStorage, d.storage);  // (back as it was)
        bump(g_fault);
        return false;
    }
    g_ring_next = (g_ring_next + 1) % kRing;
    g_walk_bytes += size;
    bump(g_bytes, size);
    maxs(g_bytes_max, size);
    maxs(g_frame_bytes_max, g_walk_bytes);
    if (fault) bump(g_fault);
    if (!e2 || fault) {
        bump(g_as_eye1);  // (eye 2's rows eye 1's: finite, but its twin not drawn)
        return false;
    }
    bump(g_mirrored);
    bump(g_mirrored_inst, n);
    if (g_marks) key_put(draw_key(g_wbuf, n, salt));
    return true;
}

// pass 2's walk: the compare (the game's own eye-2 entries against pass 1's, by W_i) and the marks (dropped only when
// pass 1 mirrored the same draw: its slice 1 drawn by the twin)
bool pass2(uintptr_t cb, uint64_t salt) {
    Draw d;
    if (look(cb, &d) != kOk) {
        bump(g_cmp_bad);
        if (d.why >= 0) bump(g_why[1][g_caller][d.why]);
        return false;
    }
    if (!raw(d.w, g_wbuf, static_cast<size_t>(d.n) * 64)) {
        bump(g_cmp_bad);
        return false;
    }
    const bool ok = g_marks && key_has(draw_key(g_wbuf, d.n, salt));  // (no key: pass 2's copy drawn)
    if (g_marks) bump(ok ? g_p2_marked_ok : g_p2_marked_no);
    if (!g_cmp2) return ok;
    bump(g_cmp_draws);
    alignas(16) float game[16];
    for (uint32_t i = 0; i < d.n; ++i) {
        if (!raw(d.storage + static_cast<uintptr_t>(i) * 64, game, 64)) {
            bump(g_cmp_bad);
            return ok;
        }
        bump(g_cmp_inst);
        const Slot* s = find(g_wbuf + i * 16);
        if (!s) {
            bump(g_unmatched);
            continue;
        }
        bump(g_matched);
        if (!std::memcmp(s->e2, game, 64)) {
            bump(g_exact);
            continue;
        }
        float dd = 0.0f, m = 1.0f;
        for (int k = 0; k < 16; ++k) {
            float x = std::fabs(s->e2[k] - game[k]);
            if (x != x) x = 1e30f;  // (a NaN on either side: the worst; fmax would drop it)
            dd = std::fmax(dd, x);
            m = std::fmax(m, std::fabs(game[k]));
        }
        fmax_store(g_worst, dd);
        fmax_store(g_worst_rel, dd / m);
        if (dd / m > 1e-4f) bump(g_over);
    }
    return ok;
}

// ---- (item 8d) the main walk: pass 1 mirrors and marks every draw with its reason, pass 2 compares; never g_refused_frame, never a key
void main_break(const char* why) {
    if (!g_main_broken.exchange(true)) log::error("[instprev] the main walk's mirror broken for the session: %s (every later main_live() false)", why);
}
void main_mark(bool ok, int why) {  // (before the original bind: plays before the draw's pipeline and its draw)
    if (!prepass::stream_marker(ok ? &main_ok_cb : kMainNo[why > 0 && why < kNoWhys ? why : kNoMark])) {
        bump(g_m_marks_failed);
        if (!ok) main_break("a \"no\" marker not recorded (the playback could keep an \"ok\" over this draw)");  // (a failed "ok": counted only)
        return;
    }
    bump(ok ? g_m_marks_ok : g_m_marks_no);
}
// pass 1 of the main walk (pass1()'s mirror with the main walk's cap and counters); true: this draw's eye-2 rows all written. *why:
// the reason of a false (kNo*)
bool main_pass1(uintptr_t cb, int* why) {
    bump(g_m_seen);
    Draw d;
    const Look l = look(cb, &d);
    if (l == kAgain) {
        bump(g_m_again);
        *why = g_mlast_why;
        return g_mlast_ok;  // (the same draw: the block in place is the one its first bind made)
    }
    if (l != kOk) {
        bump(g_m_bad);
        if (d.why >= 0) bump(g_m_why[g_caller][d.why]);
        *why = kNoBad;
        return false;
    }
    maxs(g_m_n_max, d.n);
    // (GOAL-single-pass-2 item 1) the main walk not ready: the draw left as the game made it (no copy, no CB swap), as eye 1's (its why)
    if (!(g_me2_ok && g_mrows_ok)) {
        bump(g_m_skip_unready);
        *why = kNoAsEye1;
        return false;
    }
    const uint32_t n = d.n, K = g_k;
    if (!raw(d.w, g_wbuf, static_cast<size_t>(n) * 64)) {
        bump(g_m_fault);
        *why = kNoFault;
        return false;
    }
    const uint32_t alloc = d.alloc;
    if (alloc > K || static_cast<uint64_t>(K) + alloc > kMaxBytes / 64) {  // (left as the game's: single, "instprev over K")
        bump(g_m_over_k);
        maxs(g_m_alloc_max_over_k, alloc);
        *why = kNoOverK;
        return false;
    }
    if (alloc > n) bump(g_m_alloc_over);
    const uint32_t size = (K + alloc) * 64;
    if (g_main_bytes + size > kMainCap) {
        bump(g_m_cap);
        *why = kNoCap;
        return false;
    }
    unsigned char* blk = g_ring[g_ring_next];
    if (!raw(d.storage, blk, static_cast<size_t>(alloc) * 64)) {  // rows [0, alloc): eye 1's, as the game uploads them
        bump(g_m_fault);
        *why = kNoFault;
        return false;
    }
    const bool e2 = g_me2_ok && g_mrows_ok;
    bool fault = false;
    for (uint32_t i = 0; i < alloc; ++i) {
        float* dst = reinterpret_cast<float*>(blk + static_cast<size_t>(K + i) * 64);
        std::memcpy(dst, blk + static_cast<size_t>(i) * 64, 64);  // eye 1's first ([n, alloc): as eye 1's)
        if (i < n && e2 && !fault) {
            if (!prev_wvp(g_mvp2, dst, g_wbuf + i * 16)) {
                fault = true;
                std::memcpy(dst, blk + static_cast<size_t>(i) * 64, 64);
            } else {
                if (g_mrec) record_in(g_mtab, g_mgen, g_wbuf + i * 16, dst, g_m_full);
                if (g_mnudge) {  // (item 8e's ip control: after the record, so the main compare keeps the mirror's own value)
                    for (int k = 0; k < 16; ++k) dst[k] += kMainNudge;
                    bump(g_m_nudged);
                }
            }
        }
    }
    if (!wr<uintptr_t>(d.cb + kCbStorage, reinterpret_cast<uintptr_t>(blk))) {
        bump(g_m_fault);
        *why = kNoFault;
        return false;
    }
    if (!wr<uint32_t>(d.layout + kLayoutSize, size)) {
        wr<uintptr_t>(d.cb + kCbStorage, d.storage);  // (back as it was)
        bump(g_m_fault);
        *why = kNoFault;
        return false;
    }
    g_ring_next = (g_ring_next + 1) % kRing;
    g_main_bytes += size;
    bump(g_m_bytes, size);
    maxs(g_m_walk_bytes_max, g_main_bytes);
    if (fault) bump(g_m_fault);
    if (!e2 || fault) {
        bump(g_m_as_eye1);  // (eye 2's rows eye 1's: finite, but not mirrored)
        *why = fault ? kNoFault : kNoAsEye1;
        return false;
    }
    bump(g_m_mirrored);
    bump(g_m_mirrored_inst, n);
    return true;
}
// pass 2 of the main walk: the main compare only (the game's own eye-2 entries against pass 1's, by W_i); nothing written, no marker
void main_pass2(uintptr_t cb) {
    Draw d;
    if (look(cb, &d) != kOk || !raw(d.w, g_wbuf, static_cast<size_t>(d.n) * 64)) {
        bump(g_m_cmp_bad);
        return;
    }
    bump(g_m_cmp_draws);
    alignas(16) float game[16];
    for (uint32_t i = 0; i < d.n; ++i) {
        if (!raw(d.storage + static_cast<uintptr_t>(i) * 64, game, 64)) {
            bump(g_m_cmp_bad);
            return;
        }
        bump(g_m_cmp_inst);
        const Slot* s = find_in(g_mtab, g_mgen, g_wbuf + i * 16);
        if (!s) {
            bump(g_m_unmatched);
            continue;
        }
        bump(g_m_matched);
        if (!std::memcmp(s->e2, game, 64)) {
            bump(g_m_exact);
            continue;
        }
        float dd = 0.0f, m = 1.0f;
        for (int k = 0; k < 16; ++k) {
            float x = std::fabs(s->e2[k] - game[k]);
            if (x != x) x = 1e30f;  // (a NaN on either side: the worst)
            dd = std::fmax(dd, x);
            m = std::fmax(m, std::fabs(game[k]));
        }
        fmax_store(g_m_worst, dd);
        fmax_store(g_m_worst_rel, dd / m);
        if (dd / m > 1e-4f) bump(g_m_over);
    }
}
void main_bind(int walk, uintptr_t cb) {  // (the main walk's thread)
    if (!prepass::thread_stream_ok()) {  // (SF-1: another recorder's context: no marker can reach the playback for this draw)
        bump(g_m_ctx_bad);
        if (walk == 1) main_break("a main-walk bind on another recorder's thread context (no marker for its draw)");
        return;
    }
    if (walk == 2) return main_pass2(cb);
    int why = kNoNone;
    const bool ok = main_pass1(cb, &why);
    g_mlast_ok = ok;
    g_mlast_why = ok ? kNoNone : why;
    main_mark(ok, why);
}

void on_bind(uintptr_t cb, uint64_t salt) {
    const int walk = g_walk.load(std::memory_order_relaxed);
    const bool mine = walk && GetCurrentThreadId() == g_tid.load(std::memory_order_relaxed);
    if (g_observe.load(std::memory_order_relaxed)) {  // anywhere (any walk, any thread): the count and the largest n
        uint32_t n = 0;
        g_any.fetch_add(1, std::memory_order_relaxed);
        if (rd(cb - kObjCb, &n) && n <= kMaxN) {
            uint64_t cur = g_any_n_max.load(std::memory_order_relaxed);
            while (n > cur && !g_any_n_max.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {
            }
        }
    }
    const bool main = walk && g_main.load(std::memory_order_relaxed);  // (item 8d: the main walk's own state, counters and markers)
    if (!mine) {
        // (an instanced draw on another thread during the walk: unmarked; item 8d: a main walk's frame is then not judged)
        if (walk) (main ? g_m_other : g_other).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (main) return main_bind(walk, cb);
    if (!prepass::thread_stream_ok()) {  // (SF-1) this thread's recorder is not the one the markers go to: nothing swapped, nothing marked
        bump(g_ctx_bad);
        if (walk == 1 && g_marks) g_refused_frame = state::presents.load();
        return;
    }
    const bool ok = walk == 1 ? pass1(cb, salt) : pass2(cb, salt);
    g_last_ok = ok;
    if (walk == 1 && g_marks && !ok) g_refused_frame = state::presents.load();  // (SF-3: no unforced cut this frame)
    if (g_marks) mark(ok);  // (before the original bind: the marker plays before the draw's pipeline and its draw)
}
// (SF-4) an instanced caller's bind with no previous block published: its draw's verdict "no" (no stale "ok" carried over)
void on_nocb() {
    const int walk = g_walk.load(std::memory_order_relaxed);
    if (!walk || GetCurrentThreadId() != g_tid.load(std::memory_order_relaxed)) return;
    const bool main = g_main.load(std::memory_order_relaxed);  // (item 8d: the main walk marks every draw, its own counters)
    if (main && walk != 1) return;  // (the main walk's pass 2 compares only)
    if (!main && !g_marks) return;
    if (!main) bump(g_nocb);
    // (nocb.md 4B, the cross-check) an empty draw: DAT_142ac0a18's and DAT_142ac0a20's objects with no entry, c48 not
    // published (the fill accepted no record: InstanceCount 0). Not empty: a draw with instances and no previous block (the
    // case whose twin would read a stale slot): "no" stays the only safe verdict either way
    uintptr_t p18 = 0, p20 = 0, c48 = 1;
    uint32_t n18 = 1, n20 = 1;
    const bool empty = rd(g_a18, &p18) && rd(p18, &n18) && rd(g_a20, &p20) && rd(p20, &n20) && rd(g_c48, &c48) && n18 == 0 && n20 == 0 && c48 == 0;
    if (main) {
        bump(empty ? g_m_nocb_empty : g_m_nocb_nonempty);
        g_mlast_ok = false;
        g_mlast_why = kNoNocb;
        if (prepass::thread_stream_ok()) {
            main_mark(false, kNoNocb);
        } else {
            bump(g_m_ctx_bad);
            main_break("a main-walk bind on another recorder's thread context (no marker for its draw)");
        }
        return;
    }
    bump(empty ? g_nocb_empty : g_nocb_nonempty);
    g_last_ok = false;
    if (prepass::thread_stream_ok()) mark(false);
    else bump(g_ctx_bad);
}

// PassBind FUN_1401514d0 (a .pdata function; its callers keep the ABI). Only its two instanced callers are looked at; the
// previous block is published (DAT_142ac0c38 non-zero) between their publish and their unbind
void hk_bind(void* a, void* b, void* c, void* d, void* e) {
    const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    // (item 8d: an open main walk sees every instanced bind, so a switch turned off inside it never leaves a draw unmarked)
    if ((ra == g_ret1 || ra == g_ret2) && (g_mirror.load(std::memory_order_relaxed) || g_observe.load(std::memory_order_relaxed) ||
                                           g_compare.load(std::memory_order_relaxed) || g_main.load(std::memory_order_relaxed))) {
        uintptr_t cb = 0;
        g_caller = ra == g_ret2 ? 1 : 0;  // (read by the render thread's walk only)
        if (rd(g_c38, &cb) && cb) {
            const uint64_t salt = mix(mix(mix(mix(0, reinterpret_cast<uintptr_t>(a)), reinterpret_cast<uintptr_t>(b)), reinterpret_cast<uintptr_t>(c)),
                                      reinterpret_cast<uintptr_t>(e));
            on_bind(cb, salt);
        } else {
            on_nocb();
        }
    }
    o_bind(a, b, c, d, e);
}

void reset() {  // (a test: the counts from now; the render thread racing it loses a count at most)
    for (Count* c : {&g_walks, &g_ready, &g_no_vp2, &g_no_taa, &g_no_rows, &g_seen, &g_mirrored, &g_mirrored_inst, &g_as_eye1, &g_bad, &g_again,
                     &g_cap, &g_over_k, &g_fault, &g_bytes, &g_bytes_max, &g_frame_bytes_max, &g_n_max, &g_marks_ok, &g_marks_no, &g_marks_failed,
                     &g_p2_marked_ok, &g_p2_marked_no, &g_keys_full, &g_ctx_bad, &g_nocb, &g_nocb_empty, &g_nocb_nonempty, &g_cut_skips, &g_other, &g_any, &g_any_n_max, &g_walk_draws_max, &g_walk_inst_max,
                     &g_cmp_walks, &g_cmp_draws, &g_cmp_inst, &g_matched, &g_exact, &g_unmatched, &g_over, &g_full, &g_cmp_bad})
        c->store(0);
    for (Count& c : g_hist) c.store(0);
    for (auto& a : g_why)
        for (auto& b : a)
            for (Count& c : b) c.store(0);
    g_alloc_over.store(0);
    for (Count* c : {&g_skip_unready, &g_no_taa_off, &g_alloc_max, &g_alloc_max_over_k, &g_m_skip_unready, &g_m_alloc_max_over_k}) c->store(0);
    g_e2u_base = taa::eye2_updates();
    g_worst = 0.0f;
    g_worst_rel = 0.0f;
    for (Count* c : {&g_m_opens1, &g_m_opens2, &g_m_live, &g_m_off, &g_m_no_vp2, &g_m_no_taa, &g_m_no_rows, &g_m_not_opened, &g_m_reused, &g_m_close_e1,
                     &g_m_close_fb, &g_m_seen, &g_m_mirrored, &g_m_mirrored_inst, &g_m_as_eye1, &g_m_over_k, &g_m_cap, &g_m_bad, &g_m_again, &g_m_fault,
                     &g_m_nocb_empty, &g_m_nocb_nonempty, &g_m_marks_ok, &g_m_marks_no, &g_m_marks_failed, &g_m_n_max, &g_m_alloc_over, &g_m_bytes,
                     &g_m_walk_bytes_max, &g_m_ctx_bad, &g_m_cmp_walks, &g_m_cmp_draws, &g_m_cmp_inst, &g_m_matched, &g_m_exact, &g_m_unmatched,
                     &g_m_over, &g_m_cmp_bad, &g_m_full, &g_m_other})  // (item 8d's main walk; g_main_broken kept: the session's)
        c->store(0);
    for (auto& a : g_m_why)
        for (Count& c : a) c.store(0);
    g_m_worst = 0.0f;
    g_m_worst_rel = 0.0f;
    g_m_nudge_walks.store(0);  // (item 8e)
    g_m_nudged.store(0);
}
// (item 8d) the main walk's counters, name=value
std::string main_kv_text() {
    const auto u = [](const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
    char b[1800];
    int n = std::snprintf(b, sizeof(b),
                          " main_inst=%d main_on=%d main_compare=%d main_broken=%d main_opens1=%llu main_opens2=%llu main_live=%llu main_off=%llu "
                          "main_no_vp2=%llu main_no_taa=%llu main_no_rows=%llu main_not_opened=%llu main_reused=%llu main_close_e1=%llu "
                          "main_close_fallback=%llu main_seen=%llu main_mirrored=%llu main_mirrored_inst=%llu main_as_eye1=%llu main_over_k=%llu "
                          "main_cap=%llu main_bad=%llu main_again=%llu main_fault=%llu main_nocb_empty=%llu main_nocb_nonempty=%llu main_other=%llu "
                          "main_marks_ok=%llu main_marks_no=%llu main_marks_failed=%llu main_n_max=%llu main_alloc_over=%llu main_bytes=%llu "
                          "main_walk_bytes_max=%llu main_ctx_bad=%llu main_cmp_walks=%llu main_cmp_draws=%llu main_cmp_inst=%llu main_matched=%llu "
                          "main_exact=%llu main_unmatched=%llu main_cmp_over=%llu main_cmp_bad=%llu main_full=%llu main_worst=%.6g main_worst_rel=%.6g",
                          g_main_inst.load() ? 1 : 0, g_main_on.load() ? 1 : 0, g_main_cmp.load() ? 1 : 0, g_main_broken.load() ? 1 : 0, u(g_m_opens1),
                          u(g_m_opens2), u(g_m_live), u(g_m_off), u(g_m_no_vp2), u(g_m_no_taa), u(g_m_no_rows), u(g_m_not_opened), u(g_m_reused),
                          u(g_m_close_e1), u(g_m_close_fb), u(g_m_seen), u(g_m_mirrored), u(g_m_mirrored_inst), u(g_m_as_eye1), u(g_m_over_k),
                          u(g_m_cap), u(g_m_bad), u(g_m_again), u(g_m_fault), u(g_m_nocb_empty), u(g_m_nocb_nonempty), u(g_m_other), u(g_m_marks_ok),
                          u(g_m_marks_no), u(g_m_marks_failed), u(g_m_n_max), u(g_m_alloc_over), u(g_m_bytes), u(g_m_walk_bytes_max), u(g_m_ctx_bad),
                          u(g_m_cmp_walks), u(g_m_cmp_draws), u(g_m_cmp_inst), u(g_m_matched), u(g_m_exact), u(g_m_unmatched), u(g_m_over),
                          u(g_m_cmp_bad), u(g_m_full), static_cast<double>(g_m_worst.load()), static_cast<double>(g_m_worst_rel.load()));
    if (n < 0 || n >= static_cast<int>(sizeof(b))) n = static_cast<int>(sizeof(b)) - 1;
    std::string out(b, static_cast<size_t>(n));
    out += " main_skip_unready=" + std::to_string(u(g_m_skip_unready)) + " main_alloc_max_over_k=" + std::to_string(u(g_m_alloc_max_over_k));
    uint64_t bad[kWhys] = {};  // (both callers together: the main walk's bad by the check that failed)
    for (int c = 0; c < 2; ++c)
        for (int w = 0; w < kWhys; ++w) bad[w] += g_m_why[c][w].load(std::memory_order_relaxed);
    for (int w = 0; w < kWhys; ++w) {
        char x[48];
        std::snprintf(x, sizeof(x), " main_bad_%s=%llu", kWhyName[w], static_cast<unsigned long long>(bad[w]));
        out += x;
    }
    char x[128];  // (item 8e: the ip control; at most 3 + 20 + 20 digits in 45 bytes of names)
    std::snprintf(x, sizeof(x), " main_nudge=%d main_nudge_walks=%llu main_nudged=%llu", g_main_nudge_want.load() ? 1 : 0, u(g_m_nudge_walks), u(g_m_nudged));
    out += x;
    return out;
}
// the "not the game's object" breakdown: kv (every field) or the status (the non-zero ones)
std::string why_text(bool kv) {
    std::string out = kv ? " alloc_over=" + std::to_string(g_alloc_over.load()) : "";
    bool any = false;
    for (int ps = 0; ps < 2; ++ps)
        for (int c = 0; c < 2; ++c)
            for (int w = 0; w < kWhys; ++w) {
                const unsigned long long v = g_why[ps][c][w].load(std::memory_order_relaxed);
                char b[96];
                if (kv) {
                    std::snprintf(b, sizeof(b), " bad_p%d_%s_%s=%llu", ps + 1, c ? "skin" : "unskin", kWhyName[w], v);
                } else {
                    if (!v) continue;
                    std::snprintf(b, sizeof(b), "%s pass %d %s %s %llu", any ? "," : "", ps + 1, c ? "skinned (0x87e3ed)" : "unskinned (0x87de13)",
                                  kWhyName[w], v);
                    any = true;
                }
                out += b;
            }
    if (!kv) {
        char b[160];
        std::snprintf(b, sizeof(b), " | not the game's object by the check failed:%s; allocated over the filled count (mirrored over the allocation) %llu",
                      any ? "" : " none", static_cast<unsigned long long>(g_alloc_over.load()));
        out = b + out;
    }
    return out;
}
}  // namespace

void install(bool prereq) {
    const int want = config::get_int("Stereo", "PrepassInstPrevMirror", 0);
    if (want <= 0 && config::get_int("Stereo", "PrepassInstPrevMain", 0) > 0)
        log::warn("[instprev] PrepassInstPrevMain needs [Stereo] PrepassInstPrevMirror=1: off");
    if (want <= 0) return;
    if (!prereq) {
        log::warn("[instprev] PrepassInstPrevMirror needs PrepassViews=2 and the walk's start hook (PrepassUnionCull or PrepassInstanced): off");
        return;
    }
    if (!anchors::exe_matches() || anchors::relocated() || !anchors::present(Id::PassBind) || !anchors::present(Id::InstPrevWvp) ||
        !anchors::present(Id::InstBufCur) || !anchors::present(Id::InstBufPrev) || !anchors::present(Id::ExtraCbInstMtx) ||
        !anchors::present(Id::ExtraCbInstMtxPrev)) {
        log::warn("[instprev] PrepassInstPrevMirror runs on the analysed RDR.exe build only (the instance buffer object's offsets; its anchors): off");
        return;
    }
    const int k = config::get_int("Stereo", "PrepassInstPrevK", 512);
    if (k < 1 || k > 768) {
        log::warn("[instprev] PrepassInstPrevK=%d not in 1..768 (the twins' K, (K + 256) x 64 within 64 KB): off", k);
        return;
    }
    const int main_want = config::get_int("Stereo", "PrepassInstPrevMain", 0);  // (item 8d, test only)
    if (main_want > 0 && (main_want != 1 || want != 1))
        log::warn("[instprev] PrepassInstPrevMain=%d: off (it needs PrepassInstPrevMain=1 with PrepassInstPrevMirror=1)", main_want);
    g_k = static_cast<uint32_t>(k);
    g_c38 = anchors::addr(Id::ExtraCbInstMtxPrev);
    g_c48 = anchors::addr(Id::ExtraCbInstMtx);
    g_a18 = anchors::addr(Id::InstBufPrev);
    g_a20 = anchors::addr(Id::InstBufCur);
    g_ret1 = anchors::base() + kRetUnskinned;  // (the analysed build only: checked above)
    g_ret2 = anchors::base() + kRetSkinned;
    g_prev_wvp = reinterpret_cast<PrevWvp_t>(anchors::addr(Id::InstPrevWvp));
    if (!hooks::install("RDR pass bind (FUN_1401514d0, PrepassInstPrevMirror)", reinterpret_cast<void*>(anchors::addr(Id::PassBind)), &hk_bind, &o_bind))
        return;
    g_installed = true;
    g_mirror = want == 1;
    g_observe = want >= 2;
    g_compare = want == 3;
    // (GOAL-single-pass-default.md, r310b Q1) [Stereo] PrepassInstPrevNoTaa: "notaa on" from the ini. Without TAA (FXAA or
    // none) the eye-2 TAA update does not run and the mirror stood down, so the foliage pair 5d2d5cd6../de866858.. drew
    // single every frame and held the walk cut forever (FXAA, the shipped default: no single pass at all). On, both passes
    // have the same copy of vp2 +0x200, the mirror equals two-pass (run 21 L2/O2 FXAA: 0 and 1 texels against two-pass)
    g_notaa = config::get_bool("Stereo", "PrepassInstPrevNoTaa", false);
    pso::set_instprev_live(want == 1);  // (twins.txt's instprev lines are listed only with =1 and this K: pso.cpp)
    if (main_want == 1 && want == 1) {
        g_main_inst = true;
        log::info("[instprev] [Stereo] PrepassInstPrevMain=1 (test only, item 8d): the mirror over the main walk's DVL call 1 installed, off until "
                  "\"prepass instprev main on\" (its own counters, a %llu-byte cap a walk, never the walk cut's refusal)",
                  static_cast<unsigned long long>(kMainCap));
    }
    log::info("[instprev] [Stereo] PrepassInstPrevMirror=%d on (test only, K %u): %s", want, g_k,
              want == 1 ? "eye 2's previous instance matrices written at rows K + i of each instanced draw in pass 1's pre-pass walk"
              : want == 2 ? "observe: n per instanced draw counted, nothing written"
                          : "observe and compare: n counted and pass 2's own entries compared with the mirror's, nothing written");
}

bool installed() { return g_installed.load(std::memory_order_relaxed); }

bool draw_ok() { return g_pb_ok.load(std::memory_order_relaxed); }
void consume_draw_ok() {  // (item 8e: the playback thread, after main_inst's instprev twin; the next marker sets it again)
    g_pb_ok.store(false, std::memory_order_relaxed);
    g_pb_why.store(kNoMark, std::memory_order_relaxed);
}
// (item 8d) the playback thread: the main walk's open marker's latch, and the session's main mirror not broken
bool main_live() { return g_pb_main_live.load(std::memory_order_relaxed) && !g_main_broken.load(std::memory_order_relaxed); }
int main_why() { return g_main_broken.load(std::memory_order_relaxed) ? 4 : g_pb_main_why.load(std::memory_order_relaxed); }
int draw_no_why() { return g_pb_why.load(std::memory_order_relaxed); }
uint64_t main_other() { return g_m_other.load(std::memory_order_relaxed); }
std::string main_kv() { return main_kv_text(); }
bool main_nudge_wanted() { return g_main_nudge_want.load(std::memory_order_relaxed); }
bool set_main_nudge(bool on) {  // (the render thread: main_inst's DVL call 1 open of a control frame, and its close)
    if (!g_main_inst.load(std::memory_order_relaxed)) return false;
    g_mnudge_req = on;
    return true;
}

// (item 8d) the render thread, main_inst's DVL call 1 open: pass 1 opens the main walk; pass 2 compares when pass 1 recorded
void main_open(int pass) {
    if (!g_main_inst.load(std::memory_order_relaxed)) return;
    const uint64_t f = state::presents.load();
    if (pass == 1) {
        g_main.store(false, std::memory_order_relaxed);
        g_mrec_frame = ~0ull;
        const bool on = g_installed.load(std::memory_order_relaxed) && g_mirror.load(std::memory_order_relaxed) && g_main_on.load(std::memory_order_relaxed);
        int why = 5;  // (5: off; the open's marker still tells the playback, so no earlier frame's live latch carries over)
        if (on) {
            bump(g_m_opens1);
            if (g_walk1_frame == f) {  // this frame's eye-2 state from the pre-pass walk's open
                g_mvp2 = g_vp2;
                g_me2_ok = g_e2_ok;
                g_mrows_ok = g_rows_ok;
                bump(g_m_reused);
            } else {  // (no pre-pass walk this frame: derived as walk_open does)
                g_mvp2 = camera_lever::eye2_viewport();
                const bool taa = g_mvp2 && g_taa_frame.load(std::memory_order_relaxed) == f;
                g_me2_ok = g_mvp2 && (taa || g_notaa.load(std::memory_order_relaxed));
                g_mrows_ok = prepass::eye2_rows_ready();
            }
            why = !g_mvp2 ? 1 : !g_me2_ok ? 2 : !g_mrows_ok ? 3 : g_main_broken.load(std::memory_order_relaxed) ? 4 : 0;
            g_mnudge = false;  // (item 8e: the ip control, latched below for a live walk only)
            Count* const no[5] = {&g_m_live, &g_m_no_vp2, &g_m_no_taa, &g_m_no_rows, &g_m_not_opened};
            bump(*no[why]);
        } else {
            bump(g_m_off);
        }
        // the open's marker: the playback's verdict "not" until a draw's own marker, and the walk's live latch (main_live). (The crash
        // review's SF4) "main off": the marker still goes (so no earlier frame's live latch carries over: one marker a frame), but a
        // failure to record it while off only counts, it never breaks the main mirror for the session
        if (!prepass::stream_marker(kMainOpen[why])) {
            bump(g_m_marks_failed);
            if (on) main_break("the main walk's open marker not recorded");
            return;
        }
        if (why != 0) return;  // (not live: no walk opened, nothing swapped; its instanced draws read "not live")
        g_main_bytes = 0;
        g_mlast_ok = false;
        g_mlast_why = kNoMark;
        g_mnudge = g_mnudge_req;  // (item 8e's ip control: main_inst's request for this control frame, for this walk)
        if (g_mnudge) bump(g_m_nudge_walks);
        g_mrec = g_main_cmp.load(std::memory_order_relaxed);
        if (g_mrec) {
            if (++g_mgen == 0) g_mgen = 1;  // (0: the table's empty slots)
            g_mrec_frame = f;
        }
        g_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_main.store(true, std::memory_order_relaxed);
        g_walk.store(1, std::memory_order_relaxed);
        return;
    }
    if (pass != 2) return;
    const bool cmp = g_installed.load(std::memory_order_relaxed) && g_main_cmp.load(std::memory_order_relaxed) && g_mrec_frame == f;
    g_mrec_frame = ~0ull;
    if (!cmp) return;
    bump(g_m_opens2);
    bump(g_m_cmp_walks);
    g_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_main.store(true, std::memory_order_relaxed);
    g_walk.store(2, std::memory_order_relaxed);
}
// (item 8d) e1's end (main_region's e1 marker) and DVL call 1's close (fallback): no main walk past DVL call 1
void main_close(bool fallback) {
    g_mnudge = false;  // (item 8e's ip control: put back with the walk; the render thread's)
    if (!g_main.load(std::memory_order_relaxed)) return;
    g_main.store(false, std::memory_order_relaxed);
    g_walk.store(0, std::memory_order_relaxed);
    bump(fallback ? g_m_close_fb : g_m_close_e1);
}

bool frame_refused() {
    if (!g_installed.load(std::memory_order_relaxed) || g_refused_frame != state::presents.load()) return false;
    bump(g_cut_skips);
    return true;
}

void walk_open(int pass, bool rows_ready, bool cut) {
    if (!g_installed.load(std::memory_order_relaxed)) return;
    g_walk.store(0, std::memory_order_relaxed);
    g_main.store(false, std::memory_order_relaxed);  // (item 8d: a pre-pass walk is never the main walk's)
    // (SF-4) the playback's verdict reset at every walk start while installed: no draw before a mark of its own
    if (prepass::thread_stream_ok()) mark(false);
    else bump(g_ctx_bad);
    const uint64_t f = state::presents.load();
    if (pass == 1 && g_live_pending.exchange(false, std::memory_order_relaxed) && g_mirror.load(std::memory_order_relaxed))
        pso::set_instprev_live(true);  // (after a mid-session "on": from this walk, its draws marked)
    const bool mirror = g_mirror.load(std::memory_order_relaxed), compare = g_compare.load(std::memory_order_relaxed);
    if (!mirror && !compare && !g_observe.load(std::memory_order_relaxed)) return;
    if (pass == 1) {
        g_marks = mirror;
        g_walk_bytes = 0;
        g_walk_draws = 0;
        g_walk_inst = 0;
        g_last_ok = false;
        g_vp2 = camera_lever::eye2_viewport();
        const bool taa = g_vp2 && g_taa_frame.load(std::memory_order_relaxed) == f;
        // open question 5: without the eye-2 TAA update (TAA off, no jitter) vp2 +0x200 is the copied one: stand down
        // (eye 2's rows as eye 1's, the twins not drawn), unless "notaa on" (a test: both passes then have the same copy)
        g_e2_ok = g_vp2 && (taa || g_notaa.load(std::memory_order_relaxed));
        g_rows_ok = rows_ready;
        g_walk1_frame = f;  // (item 8d: main_open reuses this frame's eye-2 state)
        bump(g_walks);
        if (!g_vp2) bump(g_no_vp2);
        else if (!taa) bump(g_no_taa);
        if (g_vp2 && !taa && !g_notaa.load(std::memory_order_relaxed)) bump(g_no_taa_off);
        if (!rows_ready) bump(g_no_rows);
        if (g_e2_ok && g_rows_ok) bump(g_ready);
        g_rec = compare && !cut && g_e2_ok;  // (the compare needs pass 2's walk: cut = the walk cut on)
        if (g_rec) {
            if (++g_gen == 0) g_gen = 1;  // (0: the table's empty slots)
            g_rec_frame = f;
        } else {
            g_rec_frame = ~0ull;
        }
        if (g_marks) {
            if (++g_dgen == 0) g_dgen = 1;
            g_mark_frame = f;
        } else {
            g_mark_frame = ~0ull;
        }
        g_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_walk.store(1, std::memory_order_relaxed);
        return;
    }
    if (pass != 2) return;
    if (cut) {  // (SF-5) this frame's pass 2 walk cut (the decision): no compare, no marks; pass 1's table dropped
        g_rec_frame = ~0ull;
        return;
    }
    g_cmp2 = compare && g_rec_frame == f;
    g_marks = mirror && g_mark_frame == f;  // (a pass 1 not marked this frame: every pass-2 copy drawn, none dropped)
    g_rec_frame = ~0ull;
    if (!g_cmp2 && !g_marks) return;
    if (g_cmp2) bump(g_cmp_walks);
    g_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_walk.store(2, std::memory_order_relaxed);
}

void walk_close() {
    g_walk.store(0, std::memory_order_relaxed);
    g_main.store(false, std::memory_order_relaxed);  // (item 8d)
}

void note_eye2(const void* vp2, bool taa) {
    (void)vp2;
    if (!g_installed.load(std::memory_order_relaxed)) return;
    // the last call before the walk wins: camera_lever's (no TAA) clears, taa.cpp's eye-2 update (after it) sets
    g_taa_frame.store(taa ? state::presents.load() : ~0ull, std::memory_order_relaxed);
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load()) return "instprev: not installed (start with [Stereo] PrepassInstPrevMirror=1|2|3, PrepassViews=2 and the walk's start hook)";
    const auto u = [](const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
    if (t.size() >= 3) {
        const std::string& w = t[2];
        const bool on = t.size() >= 4 && t[3] == "on", off = t.size() >= 4 && t[3] == "off";
        if (w == "main") {  // (item 8d, test only) prepass instprev main [on|off|compare on|off]: the mirror over the main walk's DVL call 1
            if (!g_main_inst.load()) return "instprev main: not installed (start with [Stereo] PrepassInstPrevMain=1 and PrepassInstPrevMirror=1)";
            if (on || off) {
                g_main_on = on;
            } else if (t.size() >= 5 && t[3] == "compare" && (t[4] == "on" || t[4] == "off")) {
                g_main_cmp = t[4] == "on";
            } else if (t.size() >= 5 && t[3] == "control" && (t[4] == "nudge" || t[4] == "off")) {  // (item 8e's ip control, test only)
                g_main_nudge_want = t[4] == "nudge";
                log::info("[instprev] main control %s (MainFamily=region's control kind 5: eye 2's mirrored rows nudged on its control frames)",
                          t[4].c_str());
            } else if (t.size() >= 4) {
                return "ERROR prepass instprev main on|off|compare on|off|control nudge|off";
            }
            if (t.size() >= 4)
                log::info("[instprev] main %s, main compare %s (from the next pass 1's DVL call 1)", g_main_on.load() ? "on" : "off",
                          g_main_cmp.load() ? "on" : "off");
            return "instprev main" + main_kv_text() + " kv_end=1";
        }
        if (w == "kv") {  // (the tests: the counters as name=value)
            char b[2048];
            std::snprintf(b, sizeof(b),
                          "mirror=%d observe=%d compare=%d notaa=%d k=%u walks=%llu ready=%llu no_vp2=%llu no_taa=%llu no_rows=%llu seen=%llu "
                          "mirrored=%llu mirrored_inst=%llu as_eye1=%llu bad=%llu again=%llu cap=%llu over_k=%llu fault=%llu other=%llu bytes=%llu "
                          "bytes_max=%llu walk_bytes_max=%llu n_max=%llu any=%llu any_n_max=%llu walk_draws_max=%llu walk_inst_max=%llu marks_ok=%llu "
                          "marks_no=%llu marks_failed=%llu p2_ok=%llu p2_no=%llu keys_full=%llu ctx_bad=%llu nocb=%llu nocb_empty=%llu "
                          "nocb_nonempty=%llu cut_skips=%llu cmp_walks=%llu "
                          "cmp_draws=%llu cmp_inst=%llu "
                          "matched=%llu exact=%llu unmatched=%llu over=%llu cmp_bad=%llu full=%llu worst=%.6g worst_rel=%.6g",
                          g_mirror.load() ? 1 : 0, g_observe.load() ? 1 : 0, g_compare.load() ? 1 : 0, g_notaa.load() ? 1 : 0, g_k, u(g_walks),
                          u(g_ready), u(g_no_vp2), u(g_no_taa), u(g_no_rows), u(g_seen), u(g_mirrored), u(g_mirrored_inst), u(g_as_eye1), u(g_bad),
                          u(g_again), u(g_cap), u(g_over_k), u(g_fault), u(g_other), u(g_bytes), u(g_bytes_max), u(g_frame_bytes_max), u(g_n_max),
                          u(g_any), u(g_any_n_max), u(g_walk_draws_max), u(g_walk_inst_max), u(g_marks_ok), u(g_marks_no), u(g_marks_failed),
                          u(g_p2_marked_ok), u(g_p2_marked_no), u(g_keys_full), u(g_ctx_bad), u(g_nocb), u(g_nocb_empty), u(g_nocb_nonempty),
                          u(g_cut_skips), u(g_cmp_walks), u(g_cmp_draws),
                          u(g_cmp_inst), u(g_matched),
                          u(g_exact), u(g_unmatched), u(g_over), u(g_cmp_bad), u(g_full), static_cast<double>(g_worst.load()),
                          static_cast<double>(g_worst_rel.load()));
            char r[400];  // (GOAL-single-pass-2 item 1: the readiness since the reset)
            std::snprintf(r, sizeof(r), " no_taa_off=%llu eye2_taa_updates=%llu skip_unready=%llu alloc_max=%llu alloc_max_over_k=%llu",
                          u(g_no_taa_off), static_cast<unsigned long long>(taa::eye2_updates() - g_e2u_base.load()), u(g_skip_unready), u(g_alloc_max),
                          u(g_alloc_max_over_k));
            return std::string(b) + r + why_text(true) + (g_main_inst.load() ? main_kv_text() : std::string());  // (item 8d: main_*)
        }
        if (w == "on") {
            g_mirror = true;
            g_live_pending = true;  // (the twins from the next pass 1's walk, whose draws are marked)
        } else if (w == "off") {
            g_mirror = false;
            g_observe = false;
            g_compare = false;
            g_live_pending = false;
            pso::set_instprev_live(false);  // (at once: no instprev twin handed out from the next pipeline set)
        } else if (w == "observe") {  // read only: the mirror off (its twins not drawn), n counted
            g_mirror = false;
            g_live_pending = false;
            pso::set_instprev_live(false);
            g_observe = true;
        } else if (w == "compare" && (on || off)) g_compare = on;
        else if (w == "compare") g_compare = true;
        else if (w == "notaa" && (on || off)) g_notaa = on;
        else if (w == "reset") reset();
        if (w != "reset") log::info("[instprev] mirror %s, observe %s, compare %s, notaa %s (the session; from the next walk)", g_mirror.load() ? "on" : "off",
                                    g_observe.load() ? "on" : "off", g_compare.load() ? "on" : "off", g_notaa.load() ? "on" : "off");
    }
    char b[2400];
    std::snprintf(b, sizeof(b),
                  "instprev mirror %s (K %u)%s%s%s: pass 1 walks %llu (ready %llu; no eye-2 viewport %llu, no eye-2 TAA update %llu, eye 2's rows not "
                  "ready %llu); draws seen %llu, mirrored %llu (instances %llu), eye 2's rows as eye 1's (walk not ready) %llu; refused: not the game's "
                  "object %llu, already swapped %llu, frame cap %llu, n over K or K + n over 1024 %llu, faults %llu; on other threads in the walk %llu; "
                  "bytes %llu (a draw at most %llu, a walk at most %llu); marks: twin drawn %llu, single %llu, not recorded %llu; pass 2 copies "
                  "dropped (pass 1 mirrored) %llu, drawn %llu, key table full %llu; binds on another recorder's thread context %llu, instanced binds "
                  "without c38 %llu (empty draws %llu, not empty %llu), unforced cuts skipped (a draw not mirrored) %llu",
                  g_mirror.load() ? "on" : "off", g_k, g_observe.load() ? ", observe" : "", g_compare.load() ? ", compare" : "",
                  g_notaa.load() ? ", notaa" : "", u(g_walks), u(g_ready), u(g_no_vp2), u(g_no_taa), u(g_no_rows), u(g_seen), u(g_mirrored),
                  u(g_mirrored_inst), u(g_as_eye1), u(g_bad), u(g_again), u(g_cap), u(g_over_k), u(g_fault), u(g_other), u(g_bytes), u(g_bytes_max),
                  u(g_frame_bytes_max), u(g_marks_ok), u(g_marks_no), u(g_marks_failed), u(g_p2_marked_ok), u(g_p2_marked_no), u(g_keys_full),
                  u(g_ctx_bad), u(g_nocb), u(g_nocb_empty), u(g_nocb_nonempty), u(g_cut_skips));
    std::string out = b;
    std::snprintf(b, sizeof(b),
                  " | readiness (since the reset): walks %llu, ready %llu; not ready: no eye-2 viewport %llu, no eye-2 TAA update with notaa off %llu, "
                  "eye 2's rows not ready %llu; eye-2 TAA updates %llu; notaa now %s; draws left untouched (walk not ready) %llu; refused over K %llu "
                  "(largest refused alloc %llu, largest alloc %llu, K %u)",
                  u(g_walks), u(g_ready), u(g_no_vp2), u(g_no_taa_off), u(g_no_rows),
                  static_cast<unsigned long long>(taa::eye2_updates() - g_e2u_base.load()), g_notaa.load() ? "on" : "off", u(g_skip_unready),
                  u(g_over_k), u(g_alloc_max_over_k), u(g_alloc_max), g_k);
    out += b;
    std::snprintf(b, sizeof(b),
                  " | observe: n max %llu (anywhere %llu, instanced binds anywhere %llu); n <=16 %llu, <=64 %llu, <=128 %llu, <=256 %llu, <=512 %llu, "
                  "<=768 %llu, <=999 %llu; a walk at most %llu draws, %llu instances",
                  u(g_n_max), u(g_any_n_max), u(g_any), u(g_hist[0]), u(g_hist[1]), u(g_hist[2]), u(g_hist[3]), u(g_hist[4]), u(g_hist[5]), u(g_hist[6]),
                  u(g_walk_draws_max), u(g_walk_inst_max));
    out += b;
    std::snprintf(b, sizeof(b),
                  " | compare (the cut off): pass 2 walks %llu, draws %llu, instances %llu, matched by W %llu (bit-exact %llu), unmatched %llu, worst "
                  "diff %.6g (relative %.3g), over 1e-4 relative %llu; pass 2 draws not understood %llu, table full %llu",
                  u(g_cmp_walks), u(g_cmp_draws), u(g_cmp_inst), u(g_matched), u(g_exact), u(g_unmatched), static_cast<double>(g_worst.load()),
                  static_cast<double>(g_worst_rel.load()), u(g_over), u(g_cmp_bad), u(g_full));
    out += b;
    out += why_text(false);
    return out;
}

}  // namespace rdrvr::inst_prev
