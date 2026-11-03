#include "core/prevworld_mirror.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/dual_pass.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/prepass_arrays.h"
#include "core/state.h"

namespace rdrvr::prevworld {
namespace {
using anchors::Id;

// FUN_1401322b0(vp, W_prev), the PrevWorld writer (research\run19\prevworld.md section 1, Ghidra 2026-10-22) [V]: a .pdata
// function; its entry bytes as analysed (cut_velocity's d1 checks the same 16).
constexpr uintptr_t kRvaWriter = 0x1322b0;
const uint8_t kWriterEntry[16] = {0x4c, 0x8b, 0xdc, 0x48, 0x81, 0xec, 0x38, 0x01, 0x00, 0x00, 0x80, 0xb9, 0x71, 0x03, 0x00, 0x00};
// DAT_142ac09f0 (8 bytes): non-zero, 0x1322b0, SetShaderGlobal 0x1490a0 and the draw 0x142cb0 write and draw nothing [V]
constexpr uintptr_t kRvaSuppress = 0x2ac09f0;
// the viewport: +0x70 the camera row (16 B, masked by DAT_142c44e50), +0x200 the previous VP (64 B), +0x371 TaaUpdate's flag
constexpr uintptr_t kVpCam = 0x70, kVpPrevVp = 0x200, kVpFlag = 0x371;
// the CB object (FUN_140ec8bd0's map: +0x18 = storage +0x00, stamp +0x24 = -1; FUN_140ec8b90's unmap at the draw's commit)
constexpr uintptr_t kCbStorage = 0x00, kCbMapped = 0x18;
// rage_prev_world: gPrevWorldViewProjSingle +0x00 (rows 0-3), gViewport +0x40 (row 4); eye 2's rows 5-8 at +0x50
constexpr uint32_t kDeclared = 0x50, kGrown = 0x90, kAlloc = 0x100;
constexpr uintptr_t kViewportOff = 0x40, kRows2 = 0x50;

using PrevWvp_t = float* (*)(const void* vp, float* out, const float* w);
Original_t o_pw = nullptr;
PrevWvp_t g_prev_wvp = nullptr;
std::atomic<D1_t> g_d1{nullptr};
uintptr_t g_handle_pw = 0, g_handle_vport = 0, g_table = 0, g_suppress = 0;
double g_qpc_ns = 0.0;  // ns per QPC tick

std::atomic<bool> g_installed{false}, g_write{false}, g_compare{false}, g_grow_wanted{false};
std::atomic<int> g_grow{0};  // 0 not yet, 1 grown, -1 refused (the render thread writes)
uintptr_t g_cb = 0, g_block = 0;
char g_grow_why[160] = "";

// the render thread only
const void* g_vp2 = nullptr;           // this frame's private eye-2 viewport (note_eye2)
uint64_t g_vp2_frame = ~0ull;          // the frame it was built in
uint64_t g_taa_frame = ~0ull;          // the frame taa.cpp's eye-2 update ran on it (the last note_eye2 wins)
uint64_t g_p1_frame = ~0ull, g_p2_frame = ~0ull;  // the frames counted (any call of the pass)
uint64_t g_p1_open = ~0ull, g_p2_open = ~0ull;    // the frames opened (the pass's first write on the scene viewport: after the
                                                  // scene wait, so this frame's vp2 is known)
uint64_t g_rec_frame = ~0ull;
bool g_rec = false, g_cmp2 = false;    // this frame's pass 1 records, its pass 2 compares
bool g_in_pending = false, g_in_ok = false, g_in2_pending = false;
float g_in_cam[4], g_in_pvp[16], g_in_vport[4];  // vp2's inputs and pass 1's gViewport at pass 1's first ready write
bool g_vport_ok = false;
uint32_t g_gen = 0;
uint32_t g_tick = 0;

// single writer (the render thread): load + store; the status reads from another thread
using Count = std::atomic<uint64_t>;
inline void bump(Count& c, uint64_t by = 1) { c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed); }
Count g_calls[2], g_frames[2], g_scene[2], g_other_vp[2], g_noflag[2], g_suppressed[2], g_notours[2];
Count g_outside{0};  // calls outside a double frame's passes (any thread: fetch_add)
Count g_mirrored{0}, g_as_eye1{0}, g_unready{0}, g_wrote_eye1{0}, g_not_grown{0}, g_no_taa{0}, g_fault{0}, g_grow_tries{0};
Count g_t_ticks{0}, g_t_n{0};
Count g_cmp_frames{0}, g_cmp_calls{0}, g_matched{0}, g_exact{0}, g_unmatched{0}, g_over{0}, g_full{0};
Count g_in_checks{0}, g_in_exact{0}, g_in_diff{0}, g_vport_checks{0}, g_vport_diff{0};
Count g_other_wrote{0};  // (run 26) pass 1: the game wrote rows 0-3 on another viewport (rows 5-8 not mirrored: the frame "fell back")
// (run 26 review) the game's writes of rows 0-3 outside pass 1/2 (another thread, a mono frame; any thread: fetch_add), and
// the last frame one happened in: rows_written is false for that frame
std::atomic<uint64_t> g_outside_wrote{0}, g_outside_frame{~0ull};

// (run 26, GOAL-single-pass item 6a) pass 1's frame state for pso.cpp's hold of the prevworld twins, one word so a reader on
// another thread never mixes two frames: (frame << 2) | fell_back << 1 | wrote. "wrote": a write of eye 2's own rows (vp2's,
// not eye 1's); "fell back": a game write of rows 0-3 in pass 1 whose rows 5-8 are not eye 2's (no vp2, vp2's +0x371 clear,
// the mirror off or observing, not grown, a fault, another viewport, a CB not ours). The render thread writes it.
std::atomic<uint64_t> g_fstate{~0ull};
uint64_t g_fs = ~0ull;  // (the render thread's copy)
inline void fs_set(uint64_t v) {
    if (v == g_fs) return;
    g_fs = v;
    g_fstate.store(v, std::memory_order_release);
}
inline void fs_open(uint64_t f) {
    if ((g_fs >> 2) != f || g_fs == ~0ull) fs_set(f << 2);
}
inline void fs_bad() { fs_set(g_fs | 2); }
inline void fs_wrote() { fs_set(g_fs | 1); }

// (GOAL-single-pass item 6c) the per-draw gate (prevworld_mirror.h). Armed by pso; off: call() does one relaxed load more.
// wgen: rage_prev_world's game writes of rows 0-3 (any thread: fetch_add); mgen: the wgen of the last one the mirror
// completed for eye 2 (the render thread, pass 1). The walk: the render thread between walk_open and walk_close (g_walk_tid).
// g_sent: the verdict the walk's stream carries now (the render thread's: -1 none, 0 not, 1 mirrored).
std::atomic<bool> g_gate{false}, g_gate_broken{false}, g_walk_active{false};
// (the review's SF3) the writes are counted from the gate's first arming on, whatever its later state, so wgen / mgen never
// miss an unmirrored write made while it was off (a re-armed gate would otherwise trust an old "mirrored"). Off until then.
std::atomic<bool> g_gate_counting{false};
// (the review's SF5, test only) the render-side check of the verdict: at the walk's mirrored write the CB's rows 0-3 and 5-8
// as written (the storage g_chk_at); before a later write of the walk (every 8th) and at the walk's close, while the stream says
// "mirrored", the storage read again: equal / differ (expected 0) / unread
alignas(16) float g_chk_rows[16 + 16];
uintptr_t g_chk_at = 0;
bool g_chk_ok = false;
uint32_t g_chk_tick = 0;
Count g_chk_equal{0}, g_chk_differ{0}, g_chk_unread{0};
std::atomic<uint64_t> g_wgen{0}, g_mgen{~0ull};
std::atomic<DWORD> g_walk_tid{0};
int g_sent = -1;
std::atomic<uint32_t> g_gate_skip{0};  // ("prepass prevworld gate skip N", a test: the gate's negative control; 0 off)
uint32_t g_gate_skip_n = 0;            // (the render thread)
Count g_gate_skipped{0};
// (run 25, the family compare's "pw nudge" control, families.md 3.4; test only, 0 off) delta added to row 8.x (+0x80) of each
// computed eye-2 write: S1's velocity must move where the GPU reads rows 5-8 ("prepass prevworld nudge D")
std::atomic<float> g_nudge{0.0f};
Count g_nudged{0};
// (item 8e, r114 E2m: the region's pw control, test only, 0 off) delta times the w column added to the x column of each computed eye-2
// write: clip.x + delta * clip.w, so the previous position moves by delta in NDC at every distance (the additive nudge's shift, delta / w,
// falls below the velocity plane's resolution for a far object)
std::atomic<float> g_nudge_ndc{0.0f};
// (run 25, the family compare; test only, off) "prepass prevworld rows1 on|off": each computed eye-2 write puts eye 1's own rows
// (rows 0-3 of the same call) at rows 5-8 instead, the gate's verdict kept: view 1's velocity then shows whether the GPU reads
// this draw's rows 5-8 (eye 1's motion less the disparity) or another draw's
std::atomic<bool> g_rows1{false};
Count g_rows1_n{0};
Count g_gw_m{0}, g_gw_u{0}, g_gm_m{0}, g_gm_u{0}, g_gm_failed{0}, g_gm_retry{0}, g_g_foreign_walk{0}, g_g_walks{0};
// the playback thread's: the verdict last played and its thread
std::atomic<int> g_pb_v{0};
std::atomic<DWORD> g_pb_vtid{0};
Count g_pb_m{0}, g_pb_u{0}, g_pb_resets{0};
void cb_gate_m(void*) {
    g_pb_v.store(1, std::memory_order_relaxed);
    g_pb_vtid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_pb_m.fetch_add(1, std::memory_order_relaxed);
}
void cb_gate_u(void*) {
    g_pb_v.store(0, std::memory_order_relaxed);
    g_pb_vtid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_pb_u.fetch_add(1, std::memory_order_relaxed);
}
// the render thread, inside the walk: the verdict into the stream when it changes
void gate_send(bool v) {
    const int want = v ? 1 : 0;
    if (g_sent == want) return;
    if (prepass::stream_marker(v ? &cb_gate_m : &cb_gate_u)) {
        g_sent = want;
        bump(v ? g_gm_m : g_gm_u);
        return;
    }
    if (v) {  // (the playback keeps "not": safe; sent again at the next write)
        bump(g_gm_retry);
        return;
    }
    bump(g_gm_failed);  // (the playback may hold "mirrored" past an unmirrored write: no prevworld twin from now on)
    if (!g_gate_broken.exchange(true))
        log::error("[prevworld] the per-draw gate's \"not mirrored\" marker not appended: every prevworld twin refused for the session");
}
// a game write of rows 0-3 (v: 1 mirrored for eye 2, 2 not), the writing thread
void gate_write(int v) {
    const uint64_t gen = g_wgen.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (v == 1) g_mgen.store(gen, std::memory_order_release);
    else if (GetCurrentThreadId() == g_walk_tid.load(std::memory_order_relaxed)) g_chk_ok = false;  // (the check's rows no longer the last)
    (v == 1 ? g_gw_m : g_gw_u).fetch_add(1, std::memory_order_relaxed);  // (any thread)
    if (!g_walk_active.load(std::memory_order_acquire)) return;
    if (GetCurrentThreadId() != g_walk_tid.load(std::memory_order_relaxed)) {
        g_g_foreign_walk.fetch_add(1, std::memory_order_relaxed);  // (its draws until the walk's next write are not marked)
        return;
    }
    gate_send(g_mgen.load(std::memory_order_acquire) == g_wgen.load(std::memory_order_acquire));  // (a write in between on another thread: not)
}
std::atomic<float> g_worst{0.0f}, g_worst_rel{0.0f}, g_in_worst{0.0f}, g_vport_worst{0.0f};

// the compare's table: W_prev's 12 read floats (rows 0-2 xyz, row 3 xyz; the w column is not read and may be stack
// garbage in the callers' locals) -> pass 1's eye-2 rows. Static (1.9 MB of zeroes, never allocated); a generation a frame.
constexpr unsigned kSlots = 16384, kProbe = 32;
struct Slot {
    uint32_t gen;
    float key[12], e2[16];
};
Slot g_tab[kSlots];

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
bool write_raw(uintptr_t a, const void* src, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(a), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// the game's own previous WVP (FUN_140132640, a .pdata function): vp's +0x371 clear leaves out as is
bool prev_wvp(const void* vp, float* out, const float* w) {
    __try {
        g_prev_wvp(vp, out, w);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void fmax_store(std::atomic<float>& a, float v) {
    if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}
float max_abs_diff(const float* a, const float* b, int n, float* rel) {
    float d = 0.0f, m = 1.0f;
    for (int i = 0; i < n; ++i) {
        float x = std::fabs(a[i] - b[i]);
        if (x != x) x = 1e30f;  // (a NaN on either side: the worst; fmax would drop it)
        d = std::fmax(d, x);
        m = std::fmax(m, std::fabs(b[i]));
    }
    if (rel) *rel = d / m;
    return d;
}

void key_of(const float* w, float* key) {
    static const int kIdx[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
    for (int i = 0; i < 12; ++i) key[i] = w[kIdx[i]];
}
inline uint32_t hash_key(const float* key) {
    uint64_t q[6], h = 0;
    std::memcpy(q, key, sizeof(q));
    for (uint64_t v : q) h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(h ^ (h >> 32));
}
void record(const float* key, const float* e2) {
    uint32_t i = hash_key(key);
    for (unsigned p = 0; p < kProbe; ++p, ++i) {
        Slot& s = g_tab[i & (kSlots - 1)];
        if (s.gen == g_gen && std::memcmp(s.key, key, sizeof(s.key)) != 0) continue;
        s.gen = g_gen;
        std::memcpy(s.key, key, sizeof(s.key));
        std::memcpy(s.e2, e2, sizeof(s.e2));
        return;
    }
    bump(g_full);
}
const Slot* find(const float* key) {
    uint32_t i = hash_key(key);
    for (unsigned p = 0; p < kProbe; ++p, ++i) {
        const Slot& s = g_tab[i & (kSlots - 1)];
        if (s.gen != g_gen) return nullptr;
        if (!std::memcmp(s.key, key, sizeof(s.key))) return &s;
    }
    return nullptr;
}

// a shader global's CB and byte offset, by the address of its handle int (ShaderGlobalTable: 16 bytes per id)
bool global_cb(uintptr_t handle_addr, uintptr_t* cb, int* off) {
    int h = 0;
    return rd(handle_addr, &h) && h > 0 && h <= 4096 && rd(g_table + static_cast<uintptr_t>(h) * 16, cb) && *cb &&
           rd(g_table + static_cast<uintptr_t>(h) * 16 + 8, off);
}
void refuse_grow(const char* why) {
    g_grow.store(-1, std::memory_order_relaxed);
    std::snprintf(g_grow_why, sizeof(g_grow_why), "%s", why);
    log::warn("[prevworld] eye 2's rows not written: %s", why);
}
// the render thread, at pass 1's scene wait (note_eye2): rage_prev_world grown once 0x50 -> 0x90
void try_grow() {
    uintptr_t cb = 0, cbv = 0;
    int off = -1, offv = -1;
    if (!global_cb(g_handle_pw, &cb, &off) || !global_cb(g_handle_vport, &cbv, &offv)) {
        bump(g_grow_tries);  // (the handles are resolved by name at the first SetCurrent, the CB made at its effect's load: again next frame)
        return;
    }
    if (cb != cbv || off != 0 || offv != static_cast<int>(kViewportOff)) {
        char why[160];
        std::snprintf(why, sizeof(why), "not the expected layout (gPrevWorldViewProjSingle %#llx +%#x, gViewport %#llx +%#x)",
                      static_cast<unsigned long long>(cb), off, static_cast<unsigned long long>(cbv), offv);
        refuse_grow(why);
        return;
    }
    uintptr_t block = 0;
    if (!prepass::grow_cb(cb, kDeclared, kGrown, kAlloc, &block)) {
        refuse_grow("rage_prev_world not grown (its declared size is not 0x50, or a read failed)");
        return;
    }
    g_cb = cb;
    g_block = block;
    g_grow.store(1, std::memory_order_relaxed);
    log::info("[prevworld] rage_prev_world %#llx grown 0x50 -> 0x90 (storage %#llx; the CBV stays 0x100): eye 2's rows 5-8 at +0x50",
              static_cast<unsigned long long>(cb), static_cast<unsigned long long>(block));
}

void frame_open1(uint64_t f) {
    g_p1_open = f;
    // (run 19 review) the grow checked once a frame here too, not only in note_eye2: a frame with no eye-2 viewport still
    // writes eye 1's rows at mapped+0x50, so a CB the game made again must be refused before any such write
    if (g_grow.load(std::memory_order_relaxed) > 0 && !prepass::cb_still_grown(g_cb, g_block, kGrown))
        refuse_grow("rage_prev_world was made again by the game (its storage or declared size changed)");
    const bool vp2 = g_vp2 && g_vp2_frame == f;
    if (vp2 && g_taa_frame != f) bump(g_no_taa);
    g_rec = g_compare.load(std::memory_order_relaxed) && vp2;
    if (g_rec) {
        if (++g_gen == 0) g_gen = 1;  // (0: the table's empty slots)
        g_rec_frame = f;
    } else {
        g_rec_frame = ~0ull;
    }
    g_in_pending = g_rec;
    g_in_ok = false;
    g_vport_ok = false;
}
void frame_open2(uint64_t f) {
    g_p2_open = f;
    g_cmp2 = g_compare.load(std::memory_order_relaxed) && g_rec_frame == f;
    g_in2_pending = g_cmp2;
    if (g_cmp2) bump(g_cmp_frames);
}

// pass 1, after the game's write on the scene viewport: eye 2's rows (rows 0-3 at `mapped`, the CB's mapped storage).
// (item 6c) true: rows 5-8 now hold eye 2's own rows for this write (the gate's "mirrored")
bool pass1(const float* w, uintptr_t mapped, bool grown, uint64_t f) {
    const bool write = g_write.load(std::memory_order_relaxed);
    if (!write) fs_bad();  // (run 26: rows 0-3 written by the game, rows 5-8 not)
    if (!write && !g_rec) return false;  // (observe: counted only)
    alignas(16) float wf[16], e2[16];
    if (!raw(reinterpret_cast<uintptr_t>(w), wf, sizeof(wf)) || !raw(mapped, e2, sizeof(e2))) {  // (e2: eye 1's rows, the prefill)
        bump(g_fault);
        fs_bad();
        return false;
    }
    bool eye1 = false;  // (run 26) rows 5-8 get eye 1's rows this call (no vp2, vp2 +0x371 clear, or a fault)
    const bool ready = g_vp2 && g_vp2_frame == f;
    const bool timed = ready && (++g_tick & 63) == 0;
    LARGE_INTEGER t0 = {}, t1 = {};
    if (timed) QueryPerformanceCounter(&t0);
    bool computed = false;
    if (ready) {
        uint8_t flag2 = 0;
        if (!rd(reinterpret_cast<uintptr_t>(g_vp2) + kVpFlag, &flag2) || !flag2) {
            bump(g_as_eye1);  // (0x132640 a no-op: eye 1's rows kept)
            eye1 = true;
        }
        if (prev_wvp(g_vp2, e2, wf)) {
            computed = true;
        } else {
            bump(g_fault);
            eye1 = true;
            if (!raw(mapped, e2, sizeof(e2))) {
                fs_bad();
                return false;
            }
        }
        if (g_in_pending && computed) {  // vp2's inputs for the compare's once-a-frame check, and pass 1's gViewport
            g_in_pending = false;
            const uintptr_t v2 = reinterpret_cast<uintptr_t>(g_vp2);
            g_in_ok = raw(v2 + kVpCam, g_in_cam, sizeof(g_in_cam)) && raw(v2 + kVpPrevVp, g_in_pvp, sizeof(g_in_pvp));
            g_vport_ok = raw(mapped + kViewportOff, g_in_vport, sizeof(g_in_vport));
        }
    } else {
        bump(g_unready);
        eye1 = true;
    }
    // (item 6c, the gate's negative control "prepass prevworld gate skip N"; 0, the default: never) every Nth write that would be
    // mirrored gets eye 1's rows at rows 5-8 instead (as with no eye-2 viewport): the gate must call it "not mirrored"
    if (computed && !eye1 && g_gate_skip.load(std::memory_order_relaxed) && ++g_gate_skip_n % g_gate_skip.load(std::memory_order_relaxed) == 0) {
        if (!raw(mapped, e2, sizeof(e2))) {
            bump(g_fault);
            fs_bad();
            return false;
        }
        computed = false;
        eye1 = true;
        bump(g_gate_skipped);
    }
    if (computed && !eye1 && g_rows1.load(std::memory_order_relaxed)) {  // ("prepass prevworld rows1 on", test only)
        if (!raw(mapped, e2, sizeof(e2))) {
            bump(g_fault);
            fs_bad();
            return false;
        }
        bump(g_rows1_n);
    }
    if (computed && !eye1) {  // ("prepass prevworld nudge D", test only: the gate's check below records the nudged rows)
        const float nd = g_nudge.load(std::memory_order_relaxed);
        if (nd != 0.0f) {
            e2[12] += nd;
            bump(g_nudged);
        }
        const float nn = g_nudge_ndc.load(std::memory_order_relaxed);
        if (nn != 0.0f) {  // (rows 5-8 as row vectors: clip.x = p . column 0, clip.w = p . column 3)
            for (int r = 0; r < 4; ++r) e2[r * 4] += nn * e2[r * 4 + 3];
            bump(g_nudged);
        }
    }
    bool mine = false;  // (item 6c) eye 2's own rows written for this write
    if (write) {
        if (!grown) {
            bump(g_not_grown);
            fs_bad();
        } else if (!write_raw(mapped + kRows2, e2, sizeof(e2))) {
            bump(g_fault);
            fs_bad();
        } else {
            bump(computed ? g_mirrored : g_wrote_eye1);  // (the CB is mapped: the draw's own commit uploads 0x90 bytes)
            if (eye1 || !computed) {
                fs_bad();
            } else {
                fs_wrote();
                mine = true;
                if (g_gate_counting.load(std::memory_order_relaxed)) {  // (SF5, test only: the rows as written, for the check)
                    std::memcpy(g_chk_rows + 16, e2, sizeof(e2));
                    g_chk_ok = raw(mapped, g_chk_rows, 64);
                    g_chk_at = mapped;
                }
            }
        }
    }
    if (g_rec && computed) {
        float key[12];
        key_of(wf, key);
        record(key, e2);
    }
    if (timed) {
        QueryPerformanceCounter(&t1);
        bump(g_t_ticks, static_cast<uint64_t>(t1.QuadPart - t0.QuadPart));
        bump(g_t_n);
    }
    return mine;
}

// pass 2, after the game's write on the scene viewport: its own (eye 2's) rows against pass 1's mirror of the same W_prev
void pass2(uintptr_t v, const float* w, uintptr_t mapped) {
    if (!g_cmp2) return;
    alignas(16) float wf[16], game[16];
    if (!raw(reinterpret_cast<uintptr_t>(w), wf, sizeof(wf)) || !raw(mapped, game, sizeof(game))) {
        bump(g_fault);
        return;
    }
    bump(g_cmp_calls);
    if (g_in2_pending) {  // once a frame: vp2's inputs (pass 1) against this viewport's (pass 2's eye 2)
        g_in2_pending = false;
        float cam[4], pvp[16], vport[4];
        if (g_in_ok && raw(v + kVpCam, cam, sizeof(cam)) && raw(v + kVpPrevVp, pvp, sizeof(pvp))) {
            bump(g_in_checks);
            if (!std::memcmp(cam, g_in_cam, sizeof(cam)) && !std::memcmp(pvp, g_in_pvp, sizeof(pvp))) {
                bump(g_in_exact);
            } else {
                bump(g_in_diff);
                fmax_store(g_in_worst, std::fmax(max_abs_diff(g_in_cam, cam, 4, nullptr), max_abs_diff(g_in_pvp, pvp, 16, nullptr)));
            }
        }
        if (g_vport_ok && raw(mapped + kViewportOff, vport, sizeof(vport))) {  // (gViewport at each pass's first write: reported only)
            bump(g_vport_checks);
            const float d = max_abs_diff(g_in_vport, vport, 4, nullptr);
            if (d != 0.0f) bump(g_vport_diff);
            fmax_store(g_vport_worst, d);
        }
    }
    float key[12];
    key_of(wf, key);
    const Slot* s = find(key);
    if (!s) {
        bump(g_unmatched);
        return;
    }
    bump(g_matched);
    if (!std::memcmp(s->e2, game, sizeof(game))) {
        bump(g_exact);
        return;
    }
    float rel = 0.0f;
    fmax_store(g_worst, max_abs_diff(s->e2, game, 16, &rel));
    fmax_store(g_worst_rel, rel);
    if (rel > 1e-4f) bump(g_over);
}

// (item 6c) did the game write rows 0-3 on this viewport? (its gates: +0x371 set, DAT_142ac09f0 clear; unreadable: yes)
bool other_wrote(const void* vp) {
    uint8_t fl = 0;
    uint64_t sp = 1;
    return vp && (!rd(reinterpret_cast<uintptr_t>(vp) + kVpFlag, &fl) || (fl && (!rd(g_suppress, &sp) || !sp)));
}
// after the game's 0x1322b0 in pass 1 or 2 (the render thread: dual_pass's pass is per thread). (item 6c) The write's verdict
// for the gate: 0 the game wrote no rows, 1 it wrote rows 0-3 and the mirror eye 2's own rows 5-8, 2 a write not mirrored
int after(int pass, void* vp, const float* w) {
    const int k = pass - 1;
    bump(g_calls[k]);
    const uint64_t f = state::presents.load(std::memory_order_relaxed);
    uint64_t& seen = k == 0 ? g_p1_frame : g_p2_frame;
    if (f != seen) {
        seen = f;
        bump(g_frames[k]);
    }
    if (k == 0) fs_open(f);  // (run 26: the frame state's frame, at pass 1's first call)
    if (!vp || vp != camera_lever::scene_viewport()) {
        bump(g_other_vp[k]);
        if (k == 0 && vp) {  // (run 26) did the game write rows 0-3 here? (785/785 did not in run 19's counts: +0x371 clear)
            if (other_wrote(vp)) {
                bump(g_other_wrote);
                fs_bad();
                return 2;
            }
            return 0;
        }
        return g_gate_counting.load(std::memory_order_relaxed) && other_wrote(vp) ? 2 : 0;  // (pass 2: looked at only once the gate was armed)
    }
    bump(g_scene[k]);
    // did the game write rows 0-3? (its gates: vp +0x371, DAT_142ac09f0 == 0, the handle and its CB; then +0x18 mapped)
    const uintptr_t v = reinterpret_cast<uintptr_t>(vp);
    uint8_t flag = 0;
    if (!rd(v + kVpFlag, &flag) || !flag) {
        bump(g_noflag[k]);
        return 0;
    }
    uint64_t sup = 1;
    if (!rd(g_suppress, &sup) || sup) {
        bump(g_suppressed[k]);
        return 0;
    }
    uintptr_t cb = 0, mapped = 0;
    int off = -1;
    if (!global_cb(g_handle_pw, &cb, &off) || off != 0 || !rd(cb + kCbMapped, &mapped) || !mapped) {
        bump(g_notours[k]);
        if (k == 0) fs_bad();  // (run 26: a write not confirmed, rows 5-8 not mirrored)
        return 2;  // (item 6c: counted as a write, not mirrored)
    }
    if (k == 0) {
        if (f != g_p1_open) frame_open1(f);
        const bool grown = g_grow.load(std::memory_order_relaxed) > 0 && cb == g_cb && mapped == g_block;
        return pass1(w, mapped, grown, f) ? 1 : 2;
    }
    if (f != g_p2_open) frame_open2(f);
    pass2(v, w, mapped);
    return 2;
}

// (SF5, test only) the walk's thread, while the stream says "mirrored": the CB storage still holds the rows of that write
void gate_check() {
    if (!g_chk_ok || !g_chk_at) return;
    alignas(16) float now[16 + 16];
    if (!raw(g_chk_at, now, 64) || !raw(g_chk_at + kRows2, now + 16, 64)) {
        bump(g_chk_unread);
        return;
    }
    bump(std::memcmp(now, g_chk_rows, sizeof(now)) == 0 ? g_chk_equal : g_chk_differ);
}
// the original, then the mirror (also the `call` cut_velocity's d1 runs in place of the original)
void call(void* vp, const float* w) {
    const int pass = dual_pass::pass();
    if (g_gate_counting.load(std::memory_order_relaxed) && g_walk_active.load(std::memory_order_relaxed) &&
        GetCurrentThreadId() == g_walk_tid.load(std::memory_order_relaxed) && g_sent == 1 && (++g_chk_tick & 7) == 0)
        gate_check();  // (before this write replaces them: the draws since the last one read these)
    o_pw(vp, w);
    if (pass == 1 || pass == 2) {
        const int v = after(pass, vp, w);
        if (v && g_gate_counting.load(std::memory_order_relaxed)) gate_write(v);  // (item 6c; never armed: one load)
        return;
    }
    g_outside.fetch_add(1, std::memory_order_relaxed);
    // (run 26 review) did the game write rows 0-3 here? (its gates: +0x371 set, DAT_142ac09f0 clear) Then rows 5-8 do not
    // match them: that frame falls back (conservative: the prevworld twins held for it)
    if (other_wrote(vp)) {
        g_outside_wrote.fetch_add(1, std::memory_order_relaxed);
        g_outside_frame.store(state::presents.load(std::memory_order_relaxed), std::memory_order_release);
        if (g_gate_counting.load(std::memory_order_relaxed)) gate_write(2);  // (item 6c)
    }
}
void hk(void* vp, const float* w) {
    const D1_t d1 = g_d1.load(std::memory_order_relaxed);
    if (d1) d1(vp, w, &call);
    else call(vp, w);
}

void reset() {  // (a test: the counts from now; the render thread racing it loses a count at most)
    for (int k = 0; k < 2; ++k)
        for (Count* c : {&g_calls[k], &g_frames[k], &g_scene[k], &g_other_vp[k], &g_noflag[k], &g_suppressed[k], &g_notours[k]}) c->store(0);
    for (Count* c : {&g_outside, &g_mirrored, &g_as_eye1, &g_unready, &g_wrote_eye1, &g_not_grown, &g_no_taa, &g_fault, &g_t_ticks, &g_t_n,
                     &g_cmp_frames, &g_cmp_calls, &g_matched, &g_exact, &g_unmatched, &g_over, &g_full, &g_in_checks, &g_in_exact, &g_in_diff,
                     &g_vport_checks, &g_vport_diff, &g_other_wrote})
        c->store(0);
    g_outside_wrote.store(0);
    for (auto* a : {&g_worst, &g_worst_rel, &g_in_worst, &g_vport_worst}) a->store(0.0f);
}
}  // namespace

void install(bool prereq) {
    const int want = config::get_int("Stereo", "PrepassPrevWorldMirror", 0);
    if (want <= 0) return;
    if (want > 3) {
        log::warn("[prevworld] PrepassPrevWorldMirror=%d not 1 (mirror), 2 (mirror and compare) or 3 (observe): off", want);
        return;
    }
    if (!prereq) {
        log::warn("[prevworld] PrepassPrevWorldMirror needs [Stereo] PrepassViews=2 (the private eye-2 viewport): off");
        return;
    }
    if (!anchors::exe_matches() || anchors::relocated() || !anchors::present(Id::InstPrevWvp) || !anchors::present(Id::HandlePrevWorldVP) ||
        !anchors::present(Id::HandleViewport) || !anchors::present(Id::ShaderGlobalTable)) {
        log::warn("[prevworld] PrepassPrevWorldMirror runs on the analysed RDR.exe build only (0x1322b0, DAT_142ac09f0; InstPrevWvp, the handles): off");
        return;
    }
    const uintptr_t at = anchors::base() + kRvaWriter;
    uint8_t now[sizeof(kWriterEntry)] = {};
    if (!raw(at, now, sizeof(now)) || std::memcmp(now, kWriterEntry, sizeof(now)) != 0) {
        log::warn("[prevworld] the PrevWorld writer's entry (RDR.exe+0x1322b0) is not as analysed (another hook on it?): off");
        return;
    }
    g_prev_wvp = reinterpret_cast<PrevWvp_t>(anchors::addr(Id::InstPrevWvp));
    g_handle_pw = anchors::addr(Id::HandlePrevWorldVP);
    g_handle_vport = anchors::addr(Id::HandleViewport);
    g_table = anchors::addr(Id::ShaderGlobalTable);
    g_suppress = anchors::base() + kRvaSuppress;  // (the analysed build only: checked above)
    LARGE_INTEGER fr = {};
    QueryPerformanceFrequency(&fr);
    g_qpc_ns = fr.QuadPart ? 1e9 / static_cast<double>(fr.QuadPart) : 0.0;
    if (!hooks::install("RDR PrevWorld writer (FUN_1401322b0, PrepassPrevWorldMirror)", reinterpret_cast<void*>(at), &hk, &o_pw)) return;
    g_installed = true;
    g_write = want <= 2;
    g_compare = want == 2;
    g_grow_wanted = want <= 2;
    log::info("[prevworld] [Stereo] PrepassPrevWorldMirror=%d on (test only): %s", want,
              want == 1   ? "eye 2's gPrevWorldViewProjSingle written at rage_prev_world rows 5-8 after each of pass 1's writes (no twin reads them)"
              : want == 2 ? "the mirror, and pass 2's own rows compared with it"
                          : "observe: the writer's calls counted, nothing written, nothing grown");
}

bool installed() { return g_installed.load(std::memory_order_relaxed); }
uintptr_t storage() { return g_grow.load(std::memory_order_relaxed) > 0 ? g_block : 0; }
uintptr_t storage_live() {
    if (g_grow.load(std::memory_order_relaxed) <= 0 || !g_cb || !g_block || !prepass::cb_still_grown(g_cb, g_block, kGrown)) return 0;
    return g_block;
}
bool rearm() {
    const int32_t m1 = -1;
    return g_cb && write_raw(g_cb + 0x24, &m1, sizeof(m1));
}

namespace {
bool live_now() {  // installed, writing, grown (a refusal or a re-made CB sets g_grow -1)
    return g_installed.load(std::memory_order_relaxed) && g_write.load(std::memory_order_relaxed) && g_grow.load(std::memory_order_relaxed) > 0;
}
}  // namespace
bool set_nudge(float d) {
    g_nudge.store(d, std::memory_order_relaxed);
    return live_now();
}
bool set_nudge_ndc(float d) {
    g_nudge_ndc.store(d, std::memory_order_relaxed);
    return live_now();
}
bool rows_written(uint64_t frame) {
    if (!live_now() || g_outside_frame.load(std::memory_order_acquire) == frame) return false;
    const uint64_t s = g_fstate.load(std::memory_order_acquire);
    return s != ~0ull && (s >> 2) == frame && (s & 3) == 1;
}

// ---- (GOAL-single-pass item 6c) the per-draw gate (prevworld_mirror.h)
void set_gate(bool on) {
    if (on && !g_installed.load()) return;
    if (on) g_gate_counting.store(true);  // (SF3: before the arming, so no write after it goes uncounted)
    if (g_gate.exchange(on) != on)
        log::info("[prevworld] the per-draw gate of the prevworld twins %s (test only)", on ? "armed" : "off");
}
bool gate_live() { return g_gate.load(std::memory_order_relaxed) && !g_gate_broken.load(std::memory_order_relaxed) && live_now(); }
uint64_t foreign_walks() { return g_g_foreign_walk.load(std::memory_order_relaxed); }
void walk_open() {  // (the render thread: main_inst's open marker for pass 1's DVL call 1 was appended just before)
    if (!g_gate.load(std::memory_order_relaxed)) return;
    g_walk_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_walk_active.store(true, std::memory_order_release);
    bump(g_g_walks);
    g_sent = 0;  // (the open marker's playback resets the verdict to "not": pb_walk_reset)
    gate_send(live_now() && g_mgen.load(std::memory_order_acquire) == g_wgen.load(std::memory_order_acquire));
}
void walk_close() {
    if (!g_walk_active.load(std::memory_order_relaxed) || GetCurrentThreadId() != g_walk_tid.load(std::memory_order_relaxed)) return;
    if (g_sent == 1) gate_check();  // (SF5: the walk's last draws read these)
    g_walk_active.store(false, std::memory_order_release);
    g_sent = -1;
}
void pb_walk_reset() {
    g_pb_v.store(0, std::memory_order_relaxed);
    g_pb_vtid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_pb_resets.fetch_add(1, std::memory_order_relaxed);
}
bool draw_mirrored() {
    return gate_live() && g_pb_v.load(std::memory_order_relaxed) == 1 && g_pb_vtid.load(std::memory_order_relaxed) == GetCurrentThreadId();
}
std::string gate_kv() {
    const auto c = [](const Count& x) { return static_cast<unsigned long long>(x.load(std::memory_order_relaxed)); };
    char b[600];
    std::snprintf(b, sizeof(b),
                  "gate=%d gate_broken=%d gate_live=%d wgen=%llu gate_writes_m=%llu gate_writes_u=%llu gate_walks=%llu gate_marks_m=%llu "
                  "gate_marks_u=%llu gate_mark_failed=%llu gate_mark_retry=%llu gate_foreign_walk=%llu gate_pb_m=%llu gate_pb_u=%llu gate_pb_resets=%llu "
                  "gate_skip=%u gate_skipped=%llu gate_counting=%d gate_chk_equal=%llu gate_chk_differ=%llu gate_chk_unread=%llu",
                  g_gate.load() ? 1 : 0, g_gate_broken.load() ? 1 : 0, gate_live() ? 1 : 0, static_cast<unsigned long long>(g_wgen.load()), c(g_gw_m),
                  c(g_gw_u), c(g_g_walks), c(g_gm_m), c(g_gm_u), c(g_gm_failed), c(g_gm_retry), c(g_g_foreign_walk), c(g_pb_m), c(g_pb_u), c(g_pb_resets),
                  g_gate_skip.load(), c(g_gate_skipped), g_gate_counting.load() ? 1 : 0, c(g_chk_equal), c(g_chk_differ), c(g_chk_unread));
    return b;
}

void set_d1(D1_t fn) { g_d1.store(fn, std::memory_order_relaxed); }

void note_eye2(const void* vp2, bool taa) {
    if (!g_installed.load(std::memory_order_relaxed)) return;
    const uint64_t f = state::presents.load();
    // the last call before the scene wins: camera_lever's (no TAA) first, then taa.cpp's eye-2 update on the same vp2
    g_vp2 = vp2;
    g_vp2_frame = vp2 ? f : ~0ull;
    g_taa_frame = vp2 && taa ? f : ~0ull;
    const int gs = g_grow.load(std::memory_order_relaxed);
    if (gs == 0 && g_grow_wanted.load(std::memory_order_relaxed)) try_grow();
    else if (gs > 0 && !prepass::cb_still_grown(g_cb, g_block, kGrown))
        refuse_grow("rage_prev_world was made again by the game (its storage or declared size changed)");
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load())
        return "prevworld: not installed (start with [Stereo] PrepassPrevWorldMirror=1|2|3 and PrepassViews=2, on the analysed RDR.exe)";
    const auto u = [](const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
    const double ns = u(g_t_n) ? static_cast<double>(u(g_t_ticks)) * g_qpc_ns / static_cast<double>(u(g_t_n)) : 0.0;
    if (t.size() >= 3) {
        const std::string& w = t[2];
        const bool on = t.size() >= 4 && t[3] == "on", off = t.size() >= 4 && t[3] == "off";
        if (w == "kv") {  // (the tests: the counters as name=value)
            const uint64_t fs = g_fstate.load(std::memory_order_acquire);  // (run 26: pass 1's frame state, pso's hold)
            char b[1600];
            std::snprintf(b, sizeof(b),
                          "write=%d compare=%d grow=%d calls1=%llu calls2=%llu frames1=%llu frames2=%llu scene1=%llu scene2=%llu other_vp1=%llu "
                          "other_vp2=%llu noflag1=%llu noflag2=%llu suppressed1=%llu suppressed2=%llu notours1=%llu notours2=%llu outside=%llu "
                          "mirrored=%llu as_eye1=%llu unready=%llu wrote_eye1=%llu not_grown=%llu no_taa=%llu fault=%llu grow_tries=%llu "
                          "ns_per_call=%.1f timed=%llu cmp_frames=%llu cmp_calls=%llu matched=%llu exact=%llu unmatched=%llu over=%llu full=%llu "
                          "worst=%.6g worst_rel=%.6g in_checks=%llu in_exact=%llu in_diff=%llu in_worst=%.6g vport_checks=%llu vport_diff=%llu "
                          "vport_worst=%.6g other_wrote=%llu outside_wrote=%llu fs_frame=%lld fs_wrote=%d fs_fell_back=%d nudge=%.6g nudged=%llu rows1=%d rows1_n=%llu",
                          g_write.load() ? 1 : 0, g_compare.load() ? 1 : 0, g_grow.load(), u(g_calls[0]), u(g_calls[1]), u(g_frames[0]),
                          u(g_frames[1]), u(g_scene[0]), u(g_scene[1]), u(g_other_vp[0]), u(g_other_vp[1]), u(g_noflag[0]), u(g_noflag[1]),
                          u(g_suppressed[0]), u(g_suppressed[1]), u(g_notours[0]), u(g_notours[1]), u(g_outside), u(g_mirrored), u(g_as_eye1),
                          u(g_unready), u(g_wrote_eye1), u(g_not_grown), u(g_no_taa), u(g_fault), u(g_grow_tries), ns, u(g_t_n), u(g_cmp_frames),
                          u(g_cmp_calls), u(g_matched), u(g_exact), u(g_unmatched), u(g_over), u(g_full), static_cast<double>(g_worst.load()),
                          static_cast<double>(g_worst_rel.load()), u(g_in_checks), u(g_in_exact), u(g_in_diff), static_cast<double>(g_in_worst.load()),
                          u(g_vport_checks), u(g_vport_diff), static_cast<double>(g_vport_worst.load()), u(g_other_wrote), u(g_outside_wrote),
                          fs == ~0ull ? -1ll : static_cast<long long>(fs >> 2), fs == ~0ull ? 0 : static_cast<int>(fs & 1),
                          fs == ~0ull ? 0 : static_cast<int>((fs >> 1) & 1), static_cast<double>(g_nudge.load()), u(g_nudged), g_rows1.load() ? 1 : 0, u(g_rows1_n));
            return b;
        }
        if (w == "on") {
            g_write = true;
            g_grow_wanted = true;  // (grown at the next pass 1's scene wait, if not yet)
        } else if (w == "off" || w == "observe") {  // (the calls still counted; rows 5-8 left as they are: nothing reads them)
            g_write = false;
            g_compare = false;
        } else if (w == "compare" && (on || off)) {
            g_compare = on;
        } else if (w == "compare") {
            g_compare = true;
        } else if (w == "reset") {
            reset();
        } else if (w == "gate" && t.size() >= 5 && t[3] == "skip") {  // (item 6c, a test: the per-draw gate's negative control)
            const int n = std::atoi(t[4].c_str());
            if (n < 0 || n > 1000000) return "ERROR prepass prevworld gate skip N (0 off)";
            g_gate_skip = static_cast<uint32_t>(n);
            log::info("[prevworld] gate skip %d (test only: every Nth mirrored write gets eye 1's rows, the gate's \"not\")", n);
            return gate_kv();
        } else if (w == "gate") {
            return gate_kv();
        } else if (w == "rows1" && (on || off)) {  // (the family compare's diagnostic; test only)
            g_rows1 = on;
            log::info("[prevworld] rows1 %s (test only: eye 1's rows at rows 5-8, the gate's verdict kept)", on ? "on" : "off");
            char nb[96];
            std::snprintf(nb, sizeof(nb), "prevworld rows1 %s (so far %llu)", on ? "on" : "off", static_cast<unsigned long long>(g_rows1_n.load()));
            return nb;
        } else if (w == "nudge" && t.size() >= 4) {  // (the family compare's pw nudge control; test only)
            const float d = static_cast<float>(std::atof(t[3].c_str()));
            if (!(d > -1.0f && d < 1.0f)) return "ERROR prepass prevworld nudge D (|D| < 1, 0 off)";
            g_nudge = d;
            log::info("[prevworld] nudge %g (test only: added to row 8.x of each computed eye-2 write)", static_cast<double>(d));
            char nb[96];
            std::snprintf(nb, sizeof(nb), "prevworld nudge %g (nudged so far %llu)", static_cast<double>(d), static_cast<unsigned long long>(g_nudged.load()));
            return nb;
        }
        if (w != "reset") log::info("[prevworld] mirror %s, compare %s (the session; from the next frame)", g_write.load() ? "on" : "off",
                                    g_compare.load() ? "on" : "off");
    }
    const int gs = g_grow.load();
    char b[1900];
    std::snprintf(b, sizeof(b),
                  "prevworld mirror %s%s, rage_prev_world %s%s%s: calls pass 1 %llu (frames %llu), pass 2 %llu (frames %llu), outside the passes %llu; "
                  "on the scene viewport %llu / %llu, other viewports %llu / %llu; not written by the game: +0x371 clear %llu / %llu, "
                  "DAT_142ac09f0 set %llu / %llu, no CB or not mapped %llu / %llu | pass 1: eye 2's rows written %llu, eye 1's (no eye-2 viewport "
                  "this frame) %llu, vp2 +0x371 clear (eye 1's kept) %llu, not grown %llu, frames without the eye-2 TAA update %llu, faults %llu; "
                  "%.1f ns a call (sampled %llu)",
                  g_write.load() ? "on" : "off", g_compare.load() ? ", compare" : "",
                  gs > 0 ? "grown 0x50 -> 0x90" : gs < 0 ? "refused" : "not grown", gs < 0 ? ": " : "", gs < 0 ? g_grow_why : "", u(g_calls[0]),
                  u(g_frames[0]), u(g_calls[1]), u(g_frames[1]), u(g_outside), u(g_scene[0]), u(g_scene[1]), u(g_other_vp[0]), u(g_other_vp[1]),
                  u(g_noflag[0]), u(g_noflag[1]), u(g_suppressed[0]), u(g_suppressed[1]), u(g_notours[0]), u(g_notours[1]), u(g_mirrored),
                  u(g_wrote_eye1), u(g_as_eye1), u(g_not_grown), u(g_no_taa), u(g_fault), ns, u(g_t_n));
    std::string out = b;
    std::snprintf(b, sizeof(b),
                  " | compare: pass 2 frames %llu, writes %llu, matched by W %llu (bit-exact %llu), unmatched %llu, worst diff %.6g (relative %.3g), "
                  "over 1e-4 relative %llu, table full %llu; vp2's inputs against pass 2's viewport: frames %llu, exact %llu, differ %llu (worst "
                  "%.6g); gViewport at each pass's first write: frames %llu, differ %llu (worst %.6g)",
                  u(g_cmp_frames), u(g_cmp_calls), u(g_matched), u(g_exact), u(g_unmatched), static_cast<double>(g_worst.load()),
                  static_cast<double>(g_worst_rel.load()), u(g_over), u(g_full), u(g_in_checks), u(g_in_exact), u(g_in_diff),
                  static_cast<double>(g_in_worst.load()), u(g_vport_checks), u(g_vport_diff), static_cast<double>(g_vport_worst.load()));
    out += b;
    return out;
}

}  // namespace rdrvr::prevworld
