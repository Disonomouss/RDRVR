#include "core/rec_state.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/anchors.h"
#include "core/config.h"
#include "core/dual_pass.h"
#include "core/log.h"

// draw_masks.asm: the wrapper's switch for the render thread's pass 2 (0 = the stub as before), and its call of the walk
extern "C" volatile unsigned char rdrvr_prepass_wrap = 0;
extern "C" volatile unsigned long rdrvr_prepass_wrap_tid = 0;
extern "C" void rdrvr_draw_masks_entry(void* pass, void* viewport, void* a3, unsigned a4);
// rec_skip.asm: the 22 writer thunks' gate (the render thread's recorder and thread), the originals and the counts
constexpr int kSlots = 22;
extern "C" volatile unsigned char rdrvr_rs_skip = 0;
extern "C" volatile unsigned long rdrvr_rs_tid = 0;
extern "C" volatile uintptr_t rdrvr_rs_rec = 0;
extern "C" uintptr_t rdrvr_rs_orig[kSlots] = {};
extern "C" volatile unsigned long long rdrvr_rs_skips[kSlots] = {};
extern "C" const uintptr_t rdrvr_rs_thunks[kSlots];

namespace rdrvr::rec_state {
namespace {
using anchors::Id;

// The skipped writers (p2.md section 6, with the fork's three missing draws +0x180, +0x1a0, +0x260), each a void
// function (section 8, check 1); rec_skip.asm's thunk i stands in for slot kSlotOff[i]. Never skipped: 0x25 targets,
// the clears, copies, queries and timers, the 0x4a callbacks, the stream control.
constexpr uint16_t kSlotOff[kSlots] = {0x78,  0x80,  0xa0,  0xa8,  0xb8,  0xc0,  0xe0,  0x100, 0x140, 0x158, 0x168,
                                       0x170, 0x1f0, 0x178, 0x180, 0x188, 0x190, 0x198, 0x1a0, 0x1a8, 0x1b8, 0x260};
constexpr uint8_t kSlotOp[kSlots] = {0x7,  0x8,  0x9,  0xb,  0xd,  0xe,  0x12, 0xa,  0xa,  0x26, 0x24,
                                     0x27, 0x28, 0x2a, 0x29, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x32, 0x33};

enum Why { kNoCtx, kNotRenderCtx, kNotRecorder, kNotInFrame, kInPass, kCbCommitted, kNullState, kVariantReads, kWhys };
const char* const kWhyName[kWhys] = {"no ctx", "not the render ctx", "not the recorder", "not in a frame", "in a pass",
                                     "CBs committed", "no targets or depth-stencil", "the variant's reads"};

// R0's variants (run 19 item 5, p2.md section 10; test only, "prepass reemit variant ..."), for the re-emit after a recorded
// (not skipped) walk only. A record-skipped walk (R1) and reemit_now (PrepassCutVelocity's d3) always get the full re-emit.
//   full    p2.md section 3 at 0x706050's exit, as run 18 built it (the default)
//   cbonly  the CB tables' invalidation alone (no 0x24/0x26, no FUN_140ec7090, the PSO class untouched)
//   late    the full re-emit, recorded at the walk's end in hk_letterbox (return 0x5cc334) after the walk-end marker, so it
//           plays after the pre-pass region's end (its 0x25 is then the region's first list call: it ends it)
//   nobind  the full re-emit without the 0x25 x2 and without a 9: FUN_140ec7090's other writers called one by one (the
//           same order and arguments, its disassembly 0x140ec70cd-0x140ec731e), ctx +0x248/+0x120/+0x124 left alone
enum Variant { kFull, kCbOnly, kLate, kNoBind, kVariants };
const char* const kVariantName[kVariants] = {"full", "cbonly", "late", "nobind"};

// The drift census (reads only): the ctx's cached fields by class, dwords that moved across a walk (p2.md section 6).
enum Cls { kCb, kRootsig, kPso, kVp, kTgt, kBlend, kProg, kIa, kRast, kDss, kClasses };
const char* const kClsName[kClasses] = {"cb", "rootsig", "pso", "vp", "tgt", "blend", "prog", "ia", "rast", "dss"};
struct Range {
    Cls cls;
    uint16_t lo, hi;  // [lo, hi) bytes of the ctx
};
constexpr Range kRanges[] = {{kCb, 0x10, 0x110},   {kRootsig, 0x110, 0x120}, {kPso, 0x120, 0x124},  {kVp, 0x130, 0x158},
                             {kTgt, 0x158, 0x168}, {kBlend, 0x168, 0x178},   {kProg, 0x178, 0x188}, {kIa, 0x198, 0x238},
                             {kPso, 0x248, 0x250}, {kRast, 0x290, 0x2a0},    {kDss, 0x2a0, 0x2ac}};
constexpr size_t kSnapBytes = 0x2b0;
struct Snap {
    uint32_t d[kSnapBytes / 4];
    uintptr_t rec_c0;  // the recorder's program (the 0xb writer's)
};

std::atomic<bool> g_installed{false}, g_live{false}, g_skip_installed{false}, g_skip_live{false};
std::atomic<bool> g_skip_failed{false};  // a record-skipped walk ended in a wrong state: the skip off for the session (sticky)
thread_local int t_depth = 0;            // the wrapper's nesting (a nested walk passes straight through)
std::atomic<uint64_t> g_walks{0}, g_skipped{0}, g_reemits{0}, g_refused[kWhys], g_refused_entry[kWhys], g_after_skip{0},
    g_repaired{0}, g_bytes{0}, g_rolled{0}, g_armed{0}, g_missed{0}, g_no_blend{0}, g_nested{0}, g_repair_unsafe{0},
    g_now{0},  // (re-emits by reemit_now: [Stereo] PrepassCutVelocity's D3)
    g_late{0}, g_late_missed{0};  // (the late variant: re-emits at the walk's end; walks whose end never came before disarm)
std::atomic<int> g_variant{kFull};
bool g_late_pending = false;  // the render thread: late, a recorded walk of this pass 2 left its re-emit to the walk's end
// (item 7's S2, research\run25\item7.md 1.4 and 5; [Stereo] Pass2FrontSkip, pass2_skip.cpp; test only) the stretches: their
// own install (the vtable patched without PrepassRecordSkip's walk skip), their own stand-down (sticky, not the walk's
// g_skip_failed) and counters. g_st_open, g_st_ctx, g_st_sum: the render thread's open stretch, its ctx and the thunks' skip
// count at its begin
std::atomic<bool> g_st_installed{false}, g_st_failed{false};
std::atomic<uint64_t> g_st_null_begin{0}, g_st_null_end{0};  // (item 7's S2) stretches begun / ended with the null target set
std::atomic<uint64_t> g_st_begun{0}, g_st_ended{0}, g_st_refused_entry[kWhys], g_st_refused_exit[kWhys], g_st_nested{0}, g_st_not_ready{0},
    g_st_after{0}, g_st_repaired{0}, g_st_unsafe{0}, g_st_reemits{0}, g_st_bytes{0}, g_st_rolled{0}, g_st_writes{0};
bool g_st_open = false;
uintptr_t g_st_ctx = 0, g_st_rec = 0;
uint64_t g_st_sum = 0;
std::atomic<uint64_t> g_census[2][kClasses], g_census_walks[2][kClasses], g_census_n[2];  // [0] recorded walks, [1] skipped
bool g_skip_armed = false, g_skip_ran = false;  // the render thread

void bump(std::atomic<uint64_t>& a, uint64_t n = 1) { a.fetch_add(n, std::memory_order_relaxed); }

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
bool fill(uintptr_t a, int byte, size_t n) {
    if (!a) return false;
    __try {
        std::memset(reinterpret_cast<void*>(a), byte, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Ctx {
    uintptr_t ctx = 0, rec = 0;
};
// p2.md section 3's preconditions, read under SEH: the render thread's own ctx (TLS +0x28 == the device's +0x58), its
// recorder, inside a frame, outside every pass bracket (no program, no current or persistent pass, the CB tables not
// committed), and the targets and depth-stencil FUN_140ec7090 writes set. Returns -1, or the first failed reason.
// c->ctx and c->rec are filled as soon as known (the census needs them even when the walk is not between brackets).
// (run 25, item 7's S2, research\run25\item7.md 10-11) null_targets_ok, the stretches' begin and end only: a null target set
// (ctx+0x158 = 0 with its id ctx+0x160 = 0, what the Unlock's ec6430(ctx, NULL) leaves: the back buffer resolved at playback)
// is accepted; RecReemitState passes it on without a dereference (its 0x25 is the Unlock's own packet) [V]. The depth-stencil
// state ctx+0x2a0 stays required. *null_tgt set when accepted. The walk's own checks stay strict
int check(Ctx* c, bool null_targets_ok = false, bool* null_tgt = nullptr) {
    uintptr_t dev = 0, ctx = 0, rec = 0, block = 0, tctx = 0, vt = 0, chunk = 0, a0 = 0, prog = 0, pass = 0, persist = 0, tgt = 0, dss = 0;
    uint32_t prog_id = 0;
    uint8_t ready = 0, committed = 0;
    if (!rd(anchors::addr(Id::D3dDeviceSingleton), &dev) || !rd(dev + 0x58, &ctx) || !ctx || !rd(ctx + 0x128, &rec) || !rec) return kNoCtx;
    // the game's own TLS block (RDR.exe's index 0: [gs:58h][0], as FUN_140151430 reads it), initialised (+0x38)
    if (!rd(static_cast<uintptr_t>(__readgsqword(0x58)), &block) || !rd(block + 0x38, &ready) || !ready || !rd(block + 0x28, &tctx) || tctx != ctx)
        return kNotRenderCtx;
    if (!rd(rec, &vt) || vt != anchors::addr(Id::RecorderVtbl)) return kNotRecorder;
    c->ctx = ctx;
    c->rec = rec;
    if (!rd(rec + 0x80, &chunk) || !chunk || !rd(rec + 0xa0, &a0) || !a0) return kNotInFrame;
    // (the program's id too: 0 makes the next pass bind write its 0xb, so rec +0xc0 is set before any 0xa)
    if (!rd(ctx + 0x178, &prog) || prog || !rd(ctx + 0x180, &prog_id) || prog_id || !rd(anchors::addr(Id::PassCurrent), &pass) || pass ||
        !rd(anchors::addr(Id::BindCache), &persist) || persist)
        return kInPass;
    if (!rd(anchors::addr(Id::CbCommitted), &committed) || committed) return kCbCommitted;
    if (!rd(ctx + 0x2a0, &dss) || !dss || !rd(ctx + 0x158, &tgt)) return kNullState;
    if (!tgt) {
        uint32_t id = 1;
        if (!null_targets_ok || !rd(ctx + 0x160, &id) || id) return kNullState;
        if (null_tgt) *null_tgt = true;
    }
    return -1;
}

bool snap(const Ctx& c, Snap* s) { return raw(c.ctx, s->d, sizeof(s->d)) && rd(c.rec + 0xc0, &s->rec_c0); }
void census(int bucket, const Snap& a, const Snap& b) {
    uint32_t moved[kClasses] = {};
    for (const Range& r : kRanges)
        for (unsigned o = r.lo; o < r.hi; o += 4) moved[r.cls] += a.d[o / 4] != b.d[o / 4];
    moved[kProg] += a.rec_c0 != b.rec_c0;
    for (int k = 0; k < kClasses; ++k)
        if (moved[k]) {
            bump(g_census[bucket][k], moved[k]);
            bump(g_census_walks[bucket][k]);
        }
    bump(g_census_n[bucket]);
}

// The stream's used size in the recorder's current chunk (rec +0x80 the chunk, +0x88 its buffer: +8 the used bytes).
bool used(uintptr_t rec, uintptr_t* chunk, int* n) {
    uintptr_t buf = 0;
    return rd(rec + 0x80, chunk) && rd(rec + 0x88, &buf) && rd(buf + 8, n);
}

using Writer_t = void (*)(uintptr_t rec, uintptr_t obj);
using Reemit_t = void (*)(uintptr_t ctx);
uintptr_t writer(uintptr_t rec, unsigned slot) {  // the recorder vtable's slot (its thunk when patched: off, it passes)
    uintptr_t vt = 0, w = 0;
    return rd(rec, &vt) && rd(vt + slot, &w) ? w : 0;
}
// The CB tables invalidated as the frame reset FUN_140ec5fb0 does, without its stamp bump (no upload again this frame):
// ctx +0x10..+0x108 = -1 (32 handles), +0x110 = +0x118 = 0 (the root signatures)
bool invalidate_cbs(uintptr_t ctx) { return fill(ctx + 0x10, 0xff, 0x100) && wr<uint64_t>(ctx + 0x110, 0) && wr<uint64_t>(ctx + 0x118, 0); }

// p2.md section 3, steps 2-5 (the preconditions held: step 1). stretch: (item 7's S2) counted with the stretches, not the walks
bool reemit(const Ctx& c, bool stretch = false) {
    uintptr_t chunk0 = 0, chunk1 = 0, blend = 0, rast = 0;
    int n0 = 0, n1 = 0;
    const bool counted = used(c.rec, &chunk0, &n0);
    if (!rd(c.ctx + 0x168, &blend) || !rd(c.ctx + 0x290, &rast) || !invalidate_cbs(c.ctx)) return false;
    // the blend (0x24) written directly, so the lock byte ctx +0x2e4 cannot hold it back (FUN_140ec7090 does not write
    // it); a null one is left to the next set (playback reads the blend state's +0x48); the rasterizer (0x26, a nullsub
    // at playback) for symmetry
    const uintptr_t w_blend = writer(c.rec, 0x168), w_rast = writer(c.rec, 0x158);
    if (blend && w_blend) reinterpret_cast<Writer_t>(w_blend)(c.rec, blend);
    else bump(g_no_blend);
    if (rast && w_rast) reinterpret_cast<Writer_t>(w_rast)(c.rec, rast);
    // the PSO left to the next draw's resolve (ctx +0x124 below), as the frame reset FUN_140ec5fb0 zeroes +0x248 before
    // its FUN_140ec7090: no 9 for a pipeline the stream may hold against another root signature
    wr<uint64_t>(c.ctx + 0x248, 0);
    reinterpret_cast<Reemit_t>(anchors::addr(Id::RecReemitState))(c.ctx);  // 0x25 x2, 0x27, 7, 8, 0xb (ctx +0x178), 0x12, IA; +0x120 = 0
    wr<uint8_t>(c.ctx + 0x124, 1);  // the next draw re-resolves its PSO and writes 9
    bump(stretch ? g_st_reemits : g_reemits);
    if (counted && used(c.rec, &chunk1, &n1) && chunk1 == chunk0 && n1 >= n0) bump(stretch ? g_st_bytes : g_bytes, static_cast<uint64_t>(n1 - n0));
    else bump(stretch ? g_st_rolled : g_rolled);  // (a new chunk: the bytes not counted)
    return true;
}

void count_bytes(bool counted, uintptr_t rec, uintptr_t chunk0, int n0) {
    uintptr_t chunk1 = 0;
    int n1 = 0;
    if (counted && used(rec, &chunk1, &n1) && chunk1 == chunk0 && n1 >= n0) bump(g_bytes, static_cast<uint64_t>(n1 - n0));
    else bump(g_rolled);
}

// cbonly: step 2 alone (no packet now; the next commit's 0xa re-bind every table). rec +0xc0 is the last program's, but no
// 0xa can come before the next pass bind's 0xb (the preconditions: no program, no pass, CbCommitted 0), as in full.
bool reemit_cbonly(const Ctx& c) {
    uintptr_t chunk0 = 0;
    int n0 = 0;
    const bool counted = used(c.rec, &chunk0, &n0);
    if (!invalidate_cbs(c.ctx)) return false;
    bump(g_reemits);
    count_bytes(counted, c.rec, chunk0, n0);
    return true;
}

// nobind: full's steps 2 and 3, then FUN_140ec7090 from its 0x27 on (0x140ec70cd), each writer through the recorder's
// vtable as it calls them: 0x27, 7, 8, 0xb, 0x12; its 9 skipped (full zeroes +0x248 so it never runs either); the IA (0xd
// and 8 x 0xe, or 0x10) when +0x270 is 0, the legacy texture loops (+0xf0 by the device's +0x98, then +0x650 zeroed by its
// +0x90) and (+0x130 by +0x9c, then +0x6d0 by +0x94) when +0x280 is 0; the tail +0x98 (a nullsub). Not the 0x25 x2, not
// its +0x120 = 0, and not full's +0x248 = 0 or +0x124 = 1 (the next draw's 9): the PSO class as the walk left it.
// Everything is read first (SEH); a failed read, or a count past the ctx's spans, writes nothing (refused: "the variant's reads").
using WriterIa_t = void (*)(uintptr_t rec, int slot, uintptr_t buf, uint32_t offset, uint32_t stride, uint32_t zero);
using WriterTex_t = void (*)(uintptr_t rec, int slot, uintptr_t a, uintptr_t b);
using WriterNone_t = void (*)(uintptr_t rec);
constexpr int kTex1Max = 24, kTex2Max = 16;  // (the ctx's spans: +0x2e8..+0x3a8 is 24 entries, +0x468..+0x4e8 16, +0x650..+0x6d0 16)
bool reemit_nobind(const Ctx& c) {
    uintptr_t blend = 0, rast = 0, dss = 0, prog = 0, ib = 0, f198 = 0, f1a0 = 0, f270 = 0, f280 = 0, dev = 0;
    uintptr_t vb[8] = {}, t1[kTex1Max] = {}, t1b[kTex1Max] = {}, t2[kTex2Max] = {}, t2b[kTex2Max] = {};
    uint32_t vb_off[8] = {}, vb_stride[8] = {};
    int n98 = 0, n90 = 0, n9c = 0, n94 = 0;
    if (!rd(c.ctx + 0x168, &blend) || !rd(c.ctx + 0x290, &rast) || !rd(c.ctx + 0x2a0, &dss) || !rd(c.ctx + 0x178, &prog) ||
        !rd(c.ctx + 0x230, &ib) || !rd(c.ctx + 0x198, &f198) || !rd(c.ctx + 0x1a0, &f1a0) || !rd(c.ctx + 0x270, &f270) ||
        !rd(c.ctx + 0x280, &f280) || !rd(anchors::addr(Id::D3dDeviceSingleton), &dev) || !dev || !rd(dev + 0x98, &n98) ||
        !rd(dev + 0x90, &n90) || !rd(dev + 0x9c, &n9c) || !rd(dev + 0x94, &n94))
        return false;
    if (n98 > kTex1Max || n9c > kTex2Max || n90 > 17 || n94 > 17) return false;
    for (int i = 0; i < 8; ++i)
        if (!rd(c.ctx + 0x1b0 + 8 * i, &vb[i]) || !rd(c.ctx + 0x1f0 + 4 * i, &vb_off[i]) || !rd(c.ctx + 0x210 + 4 * i, &vb_stride[i])) return false;
    for (int i = 0; i < n98; ++i)
        if (!rd(c.ctx + 0x3a8 + 8 * i, &t1[i]) || !rd(c.ctx + 0x3a8 - 0xc0 + 8 * i, &t1b[i])) return false;
    for (int i = 0; i < n9c; ++i)
        if (!rd(c.ctx + 0x4e8 + 8 * i, &t2[i]) || !rd(c.ctx + 0x4e8 - 0x80 + 8 * i, &t2b[i])) return false;
    const uintptr_t w_blend = writer(c.rec, 0x168), w_rast = writer(c.rec, 0x158), w_dss = writer(c.rec, 0x170), w_vp = writer(c.rec, 0x78),
                    w_sc = writer(c.rec, 0x80), w_prog = writer(c.rec, 0xa8), w_ib = writer(c.rec, 0xe0), w_decl = writer(c.rec, 0xb8),
                    w_vb = writer(c.rec, 0xc0), w_ia_none = writer(c.rec, 0xc8), w_tex1 = writer(c.rec, 0xf0), w_tex2 = writer(c.rec, 0x130),
                    w_tail = writer(c.rec, 0x98);
    if (!w_dss || !w_vp || !w_sc || !w_prog || !w_ib || !w_decl || !w_vb || !w_ia_none || !w_tex1 || !w_tex2 || !w_tail) return false;
    uintptr_t chunk0 = 0;
    int n0 = 0;
    const bool counted = used(c.rec, &chunk0, &n0);
    if (!invalidate_cbs(c.ctx)) return false;  // (the first write: nothing written when it fails)
    if (blend && w_blend) reinterpret_cast<Writer_t>(w_blend)(c.rec, blend);
    else bump(g_no_blend);
    if (rast && w_rast) reinterpret_cast<Writer_t>(w_rast)(c.rec, rast);
    reinterpret_cast<Writer_t>(w_dss)(c.rec, dss);           // 0x27
    reinterpret_cast<Writer_t>(w_vp)(c.rec, c.ctx + 0x130);  // 7 (a pointer into the ctx)
    reinterpret_cast<Writer_t>(w_sc)(c.rec, c.ctx + 0x148);  // 8
    reinterpret_cast<Writer_t>(w_prog)(c.rec, prog);         // 0xb (NULL here: the preconditions)
    reinterpret_cast<Writer_t>(w_ib)(c.rec, ib);             // 0x12
    if (f270 == 0) {
        if (f198 == 0) {
            reinterpret_cast<Writer_t>(w_decl)(c.rec, f1a0);  // 0xd
            for (int i = 0; i < 8; ++i) reinterpret_cast<WriterIa_t>(w_vb)(c.rec, i, vb[i], vb_off[i], vb_stride[i], 0);  // 0xe
        } else {
            reinterpret_cast<Writer_t>(w_ia_none)(c.rec, f198);  // 0x10 (rdx = ctx +0x198, 0x140ec7199)
        }
        for (int i = 0; i < n98; ++i) reinterpret_cast<WriterTex_t>(w_tex1)(c.rec, i, t1[i], t1b[i]);
        if (n90 > 1) fill(c.ctx + 0x650, 0, static_cast<size_t>(n90 - 1) * 8);
    }
    if (f280 == 0) {
        for (int i = 0; i < n9c; ++i) reinterpret_cast<WriterTex_t>(w_tex2)(c.rec, i, t2[i], t2b[i]);
        if (n94 > 1) fill(c.ctx + 0x6d0, 0, static_cast<size_t>(n94 - 1) * 8);
    }
    reinterpret_cast<WriterNone_t>(w_tail)(c.rec);
    bump(g_reemits);
    count_bytes(counted, c.rec, chunk0, n0);
    return true;
}

// A walk whose writers were skipped but whose exit fails the preconditions (inside a pass bracket): the stream is stale.
// The skip stands down for the session (sticky: no command turns it on again), and the part that can fault is put right.
// With the targets and depth-stencil set, the full re-emit (FUN_140ec7090 writes the bound program's 0xb from ctx +0x178
// itself, so rec +0xc0 is set before any 0xa); without them, the bound program's 0xb alone, the CB tables invalidated only
// when no 0xa can then resolve against a stale program (a 0xb went out, or no program is bound: the next bind writes
// its 0xb first), the PSO re-resolved. The material CB the pass bind alone binds may be missing until the next bind (a
// wrong image, not a fault).
// stretch: (item 7's S2) a record-skipped stretch around one game call (stretch_end): the same repair, its own stand-down
// (the stretches off for the session; PrepassRecordSkip's walk skip is not touched) and counters
void after_skip(const Ctx& c, int why, bool stretch = false) {
    if (stretch) {
        bump(g_st_after);
        g_st_failed = true;
        log::warn("[rec_state] a record-skipped stretch (Pass2FrontSkip) ended %s: the stretches stood down for the session", kWhyName[why]);
    } else {
        bump(g_after_skip);
        g_skip_failed = true;
        g_skip_live = false;
        g_skip_armed = false;  // (walk() tests only this: the rest of this frame records)
        log::warn("[rec_state] a record-skipped walk ended %s: the skip stood down for the session (the cut instead)", kWhyName[why]);
    }
    if (!c.ctx || !c.rec || (why != kInPass && why != kCbCommitted && why != kNullState)) return;
    uintptr_t prog = 0, tgt = 0, dss = 0, cb_list = 0;
    uint8_t committed = 0;
    if (!rd(c.ctx + 0x178, &prog) || !rd(c.ctx + 0x158, &tgt) || !rd(c.ctx + 0x2a0, &dss) || !rd(anchors::addr(Id::CbCommitted), &committed))
        return;
    // CbCommitted gates the per-draw table commit (FUN_14014a150 at 0x14014a70e: set, FUN_14014a770/ab50 are not called
    // until the next pass bind clears it; p2.md section 8). A 0xb written here can switch the root signature at playback
    // and unbind every table, so the repair must let the next draw commit again: CbCommitted = 0, which needs the bound
    // pass's CB list (FUN_14014a770 reads its +0x40). Without the list the repair is not safe: nothing is written (no 0xa
    // can follow before the next bind either, so the stale rec +0xc0 cannot be reached).
    if (committed && (!rd(anchors::addr(Id::CbList), &cb_list) || !cb_list)) {
        bump(stretch ? g_st_unsafe : g_repair_unsafe);
        log::warn("[rec_state] the repair after the skip not made: the CB tables committed and no CB list bound");
        // (item 8c's crash review SF1, stretches only) the CB caches still dropped: no commit can happen while CbCommitted is
        // set, so the next bind's 0xb comes first and its commit then writes every table again. The PSO hash (ctx +0x120) is
        // left: zeroing it would make the next program/dss/decl setter in this bracket write a 9 against the stream's
        // pre-skip root signature (the delta review; p2.md section 1.2 item 2)
        if (stretch) invalidate_cbs(c.ctx);
        return;
    }
    bool ok = false;
    // (item 7's S2, the null-target review) a stretch's null target set (id 0: the Unlock's own state) takes the full re-emit
    // too: RecReemitState passes ctx+0x158 on without a dereference, and the partial repair below would leave its sets out
    uint32_t tgt_id = 1;
    const bool null_tgt_ok = stretch && !tgt && dss && rd(c.ctx + 0x160, &tgt_id) && tgt_id == 0;
    if ((tgt && dss) || null_tgt_ok) {
        ok = reemit(c, stretch);
    } else {
        const uintptr_t w_prog = writer(c.rec, 0xa8);
        bool prog_out = false;
        if (prog && w_prog) {
            reinterpret_cast<Writer_t>(w_prog)(c.rec, prog);
            prog_out = true;
        }
        ok = (prog_out || !prog) && invalidate_cbs(c.ctx) && wr<uint32_t>(c.ctx + 0x120, 0) && wr<uint8_t>(c.ctx + 0x124, 1);
    }
    if (ok && committed) ok = wr<uint8_t>(anchors::addr(Id::CbCommitted), 0);  // (the next draw commits its tables: 0xa after the 0xb)
    if (ok) bump(stretch ? g_st_repaired : g_repaired);
}

void walk(void* pass, void* viewport, void* a3, unsigned a4) {
    if (t_depth > 0) {  // a nested pre-pass call (not seen statically): straight through, the outer walk's skip and re-emit as they are
        bump(g_nested);
        rdrvr_draw_masks_entry(pass, viewport, a3, a4);
        return;
    }
    struct Depth {
        Depth() { ++t_depth; }
        ~Depth() { --t_depth; }
    } depth;
    bump(g_walks);
    Ctx c;
    Snap s0, s1;
    const int pre = check(&c);
    const bool snapped = c.ctx && snap(c, &s0);
    bool skip = false;
    if (g_skip_armed) {
        if (pre < 0 && snapped) {
            rdrvr_rs_rec = c.rec;
            rdrvr_rs_tid = GetCurrentThreadId();
            rdrvr_rs_skip = 1;
            skip = g_skip_ran = true;
        } else {
            bump(g_refused_entry[pre < 0 ? kNoCtx : pre]);  // (the walk recorded: walk_cut_end counts the handover's problem)
        }
    }
    rdrvr_draw_masks_entry(pass, viewport, a3, a4);
    if (skip) {
        rdrvr_rs_skip = 0;  // (only its own: item 7's S2 stretches set it too; stretch_begin refuses inside a walk)
        bump(g_skipped);
    }
    if (snapped && snap(c, &s1)) census(skip ? 1 : 0, s0, s1);
    if (!skip && !g_live.load(std::memory_order_relaxed)) return;  // (R0's control: the walk as recorded, no re-emit)
    const int var = skip ? kFull : g_variant.load(std::memory_order_relaxed);  // (R0's variants: recorded walks only)
    if (var == kLate) {  // (the re-emit at the walk's end: walk_end, hk_letterbox)
        g_late_pending = true;
        return;
    }
    Ctx e;
    const int why = check(&e);
    if (why < 0 && e.ctx == c.ctx && e.rec == c.rec && (var == kCbOnly ? reemit_cbonly(e) : var == kNoBind ? reemit_nobind(e) : reemit(e))) return;
    // (another ctx than at entry; a variant's reads failed (nothing written); or the ctx's own reads failed before anything
    // was written: no ctx)
    const int w = why >= 0 ? why : e.ctx != c.ctx || e.rec != c.rec ? kNotRenderCtx : var != kFull ? kVariantReads : kNoCtx;
    bump(g_refused[w]);
    log::limited("rec_state.refused", 10, "[rec_state] re-emit refused after pass 2's pre-pass walk: %s%s", kWhyName[w], skip ? " (writers skipped)" : "");
    if (skip) after_skip(e, w);
}

bool patch_vtable() {
    const uintptr_t vt = anchors::addr(Id::RecorderVtbl), lo = anchors::base();
    LONG lfanew = 0;
    DWORD image = 0;
    if (!vt || !lo || !rd(lo + 0x3c, &lfanew) || !rd(lo + lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader.SizeOfImage), &image)) return false;
    uintptr_t cur[kSlots];
    for (int i = 0; i < kSlots; ++i)
        if (!rd(vt + kSlotOff[i], &cur[i]) || cur[i] < lo || cur[i] >= lo + image) {
            log::warn("[rec_state] the recorder vtable's +%#x is not RDR.exe code: PrepassRecordSkip off", kSlotOff[i]);
            return false;
        }
    void* const first = reinterpret_cast<void*>(vt + kSlotOff[0]);
    const size_t span = 0x260 + sizeof(void*) - kSlotOff[0];
    DWORD old = 0;
    if (!VirtualProtect(first, span, PAGE_READWRITE, &old)) {
        log::warn("[rec_state] the recorder vtable: VirtualProtect failed (%lu): PrepassRecordSkip off", GetLastError());
        return false;
    }
    for (int i = 0; i < kSlots; ++i) {
        rdrvr_rs_orig[i] = cur[i];  // (before the slot: a thunk reached from it always has its original)
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vt + kSlotOff[i]), reinterpret_cast<void*>(rdrvr_rs_thunks[i]));
    }
    VirtualProtect(first, span, old, &old);
    return true;
}
}  // namespace

void install(bool cut_gates) {
    const bool want = config::get_bool("Stereo", "RecorderReemit", false), want_skip = config::get_bool("Stereo", "PrepassRecordSkip", false);
    if (!want && !want_skip) return;
    if (!want) {
        log::warn("[rec_state] PrepassRecordSkip needs [Stereo] RecorderReemit=1 (the skip is never left without the re-emit): off");
        return;
    }
    if (!dual_pass::masks_hooked()) {
        log::warn("[rec_state] RecorderReemit needs [Compat] DrawMaskInit=1 (the DrawMasksFn stub): off");
        return;
    }
    if (!anchors::exe_matches() || anchors::relocated()) {
        log::warn("[rec_state] RecorderReemit runs on the analysed RDR.exe build only (the ctx and vtable offsets): off");
        return;
    }
    if (!anchors::present(Id::RecReemitState) || !anchors::present(Id::CbCommitted) || !anchors::present(Id::PassCurrent) ||
        !anchors::present(Id::CbList)) {
        log::warn("[rec_state] RecorderReemit: this build has no RecReemitState/CbCommitted/PassCurrent/CbList: off");
        return;
    }
    g_live = true;
    g_installed = true;
    log::info("[rec_state] [Stereo] RecorderReemit on (test only: the record side's state re-emitted after each of pass 2's pre-pass walks, "
              "between pass brackets only)");
    if (!want_skip) return;
    if (!cut_gates) {
        log::warn("[rec_state] PrepassRecordSkip needs PrepassWalkCut's gates (PrepassWalkCut=1 and its switches): off");
        return;
    }
    if (!patch_vtable()) return;
    g_skip_live = true;
    g_skip_installed = true;
    log::info("[rec_state] [Stereo] PrepassRecordSkip on (test only: the cut's frames walk pass 2's pre-pass with the recorder's %d state and draw "
              "writers skipped, then the re-emit; the vtable's slots %p.. patched)",
              kSlots, reinterpret_cast<void*>(anchors::addr(Id::RecorderVtbl) + kSlotOff[0]));
}

bool installed() { return g_installed.load(std::memory_order_relaxed); }

bool reemit_now() {
    if (!g_installed.load(std::memory_order_relaxed)) return false;
    Ctx e;
    const int why = check(&e);
    if (why < 0 && reemit(e)) {
        bump(g_now);
        return true;
    }
    const int w = why >= 0 ? why : kNoCtx;
    bump(g_refused[w]);
    log::limited("rec_state.now", 10, "[rec_state] re-emit at the cut walk's end refused: %s", kWhyName[w]);
    return false;
}

void walk_end() {
    if (!g_late_pending || !g_installed.load(std::memory_order_relaxed) || rdrvr_prepass_wrap_tid != GetCurrentThreadId()) return;
    g_late_pending = false;
    if (!g_live.load(std::memory_order_relaxed) || g_variant.load(std::memory_order_relaxed) != kLate) return;  // (switched since the walk)
    Ctx e;
    const int why = check(&e);
    if (why < 0 && reemit(e)) {
        bump(g_late);
        return;
    }
    const int w = why >= 0 ? why : kNoCtx;
    bump(g_refused[w]);
    log::limited("rec_state.late", 10, "[rec_state] late re-emit at the walk's end refused: %s", kWhyName[w]);
}

// ---- (item 7's S2, research\run25\item7.md 1.4 and 5; [Stereo] Pass2FrontSkip, pass2_skip.cpp; test only) the stretches
// rdrvr_rs_skips summed: the writer calls the thunks have skipped so far (all slots)
static uint64_t skips_sum() {
    uint64_t n = 0;
    for (int i = 0; i < kSlots; ++i) n += rdrvr_rs_skips[i];
    return n;
}
bool stretch_install() {
    if (!g_installed.load()) {
        log::warn("[rec_state] the stretches (Pass2FrontSkip) need [Stereo] RecorderReemit=1 installed: off");
        return false;
    }
    if (!g_skip_installed.load()) {
        if (!patch_vtable()) {
            log::warn("[rec_state] the stretches (Pass2FrontSkip): the recorder vtable not patched: off");
            return false;
        }
        // (item7.md 5: R0's re-emit after uncut walks is not implied; "prepass reemit on" turns it on. PrepassRecordSkip
        // keeps it, its skip is never left without the re-emit)
        g_live = false;
        log::info("[rec_state] the recorder vtable's %d slots patched for the stretches (Pass2FrontSkip); R0's re-emit after pass 2's "
                  "walks off (\"prepass reemit on\" turns it on)", kSlots);
    }
    g_st_installed = true;
    return true;
}
bool stretch_ready() { return g_st_installed.load(std::memory_order_relaxed) && !g_st_failed.load(std::memory_order_relaxed); }
// The render thread, just before one game call: p2.md section 3's preconditions (check()), then the 22 writers skipped for
// this thread's recorder. Refused (counted by reason; nothing skipped, the call records as the game's): not installed or
// stood down, another skip on (a walk's, or a stretch still open), inside a walk, or check() failing
bool stretch_begin() {
    if (!stretch_ready()) {
        bump(g_st_not_ready);
        return false;
    }
    if (g_st_open || rdrvr_rs_skip || t_depth > 0) {
        bump(g_st_nested);
        return false;
    }
    Ctx c;
    bool null_tgt = false;
    const int pre = check(&c, true, &null_tgt);
    if (pre >= 0) {
        bump(g_st_refused_entry[pre]);
        return false;
    }
    if (null_tgt) bump(g_st_null_begin);
    g_st_ctx = c.ctx;
    g_st_rec = c.rec;
    g_st_sum = skips_sum();
    g_st_open = true;
    rdrvr_rs_rec = c.rec;
    rdrvr_rs_tid = GetCurrentThreadId();
    rdrvr_rs_skip = 1;
    bump(g_st_begun);
    return true;
}
// Just after it: the skip off, then the full re-emit (p2.md section 3, the same as after a record-skipped walk). Its
// preconditions failing (or another ctx than at the begin): after_skip's repair, and the stretches stand down for the session.
// skipped: the writer calls the stretch skipped (every thread's thunk count moved meanwhile: the render thread's only, as the
// gate is its recorder and thread)
bool stretch_end(uint64_t* skipped) {
    if (!g_st_open) return false;
    rdrvr_rs_skip = 0;
    g_st_open = false;
    const uint64_t n = skips_sum() - g_st_sum;
    bump(g_st_writes, n);
    if (skipped) *skipped = n;
    Ctx e;
    bool null_tgt = false;
    const int why = check(&e, true, &null_tgt);
    if (why < 0 && e.ctx == g_st_ctx && e.rec == g_st_rec && reemit(e, true)) {
        if (null_tgt) bump(g_st_null_end);
        bump(g_st_ended);
        return true;
    }
    const int w = why >= 0 ? why : e.ctx != g_st_ctx || e.rec != g_st_rec ? kNotRenderCtx : kNoCtx;
    bump(g_st_refused_exit[w]);
    log::limited("rec_state.stretch", 10, "[rec_state] re-emit refused after a record-skipped stretch (Pass2FrontSkip): %s", kWhyName[w]);
    after_skip(e, w, true);
    return false;
}
std::string stretch_kv() {
    auto v = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
    uint64_t in = 0, out = 0;
    for (int k = 0; k < kWhys; ++k) in += g_st_refused_entry[k].load(), out += g_st_refused_exit[k].load();
    char b[700];
    std::snprintf(b, sizeof(b),
                  "st_installed=%d st_failed=%d st_begun=%llu st_ended=%llu st_refused_entry=%llu st_refused_exit=%llu st_nested=%llu "
                  "st_not_ready=%llu st_after=%llu st_repaired=%llu st_unsafe=%llu st_reemits=%llu st_bytes=%llu st_rolled=%llu st_writes=%llu st_null_begin=%llu st_null_end=%llu",
                  g_st_installed.load() ? 1 : 0, g_st_failed.load() ? 1 : 0, v(g_st_begun), v(g_st_ended), static_cast<unsigned long long>(in),
                  static_cast<unsigned long long>(out), v(g_st_nested), v(g_st_not_ready), v(g_st_after), v(g_st_repaired), v(g_st_unsafe),
                  v(g_st_reemits), v(g_st_bytes), v(g_st_rolled), v(g_st_writes), v(g_st_null_begin), v(g_st_null_end));
    std::string s = b;
    for (int k = 0; k < kWhys; ++k)  // (by reason, kWhyName's order: st_why<k>=refused at entry/at exit, only the non-zero ones)
        if (g_st_refused_entry[k].load() || g_st_refused_exit[k].load()) {
            std::snprintf(b, sizeof(b), " st_why%d=%llu/%llu", k, v(g_st_refused_entry[k]), v(g_st_refused_exit[k]));
            s += b;
        }
    return s;
}
void stretch_reset() {  // (a test: the counts from now; the stand-down kept)
    for (auto* a : {&g_st_begun, &g_st_ended, &g_st_nested, &g_st_not_ready, &g_st_reemits, &g_st_bytes, &g_st_rolled, &g_st_writes})
        a->store(0, std::memory_order_relaxed);
    for (int k = 0; k < kWhys; ++k) g_st_refused_entry[k] = 0;
    g_st_null_begin = 0;
    g_st_null_end = 0;
    // (st_refused_exit, st_after, st_repaired, st_unsafe kept: they mark a stand-down and its cause)
}

bool skip_wanted() {  // (never without the re-emit: "prepass reemit off" makes the cut's frames cut again)
    return g_skip_installed.load(std::memory_order_relaxed) && g_skip_live.load(std::memory_order_relaxed) && g_live.load(std::memory_order_relaxed) &&
           !g_skip_failed.load(std::memory_order_relaxed);
}

void arm(bool skip) {
    if (!g_installed.load(std::memory_order_relaxed)) return;
    g_skip_armed = skip;
    g_skip_ran = false;
    g_late_pending = false;
    if (skip) bump(g_armed);
    rdrvr_prepass_wrap_tid = GetCurrentThreadId();
    rdrvr_prepass_wrap = 1;
}

bool disarm() {
    if (!g_installed.load(std::memory_order_relaxed)) return true;
    rdrvr_prepass_wrap = 0;
    rdrvr_rs_skip = 0;
    const bool missed = g_skip_armed && !g_skip_ran;
    if (missed) bump(g_missed);
    g_skip_armed = false;
    if (g_late_pending) bump(g_late_missed);  // (late: a walk whose end hk_letterbox never saw in this pass 2)
    g_late_pending = false;
    return !missed;
}

std::string command(const std::vector<std::string>& t) {  // t[0] "prepass", t[1] "reemit"
    if (!g_installed.load()) return "reemit: not installed (start with [Stereo] RecorderReemit=1, [Compat] DrawMaskInit=1)";
    if (t.size() == 3 && (t[2] == "on" || t[2] == "off")) g_live = t[2] == "on";
    if (t.size() == 4 && t[2] == "variant") {  // (R0's variants; a walk already left to its end by late keeps it: walk_end checks again)
        int v = -1;
        for (int k = 0; k < kVariants; ++k)
            if (t[3] == kVariantName[k]) v = k;
        if (v < 0) return "reemit: variant full|cbonly|late|nobind";
        g_variant = v;
    }
    if (t.size() == 4 && t[2] == "skip" && (t[3] == "on" || t[3] == "off")) {
        if (!g_skip_installed.load()) return "reemit: the skip not installed (start with [Stereo] PrepassRecordSkip=1 and PrepassWalkCut's gates)";
        g_skip_live = t[3] == "on" && !g_skip_failed.load();  // (a skip that stood down stays down for the session)
    }
    if (t.size() == 3 && t[2] == "reset") {  // (a test: the counts from now; the render thread racing it loses a count at most)
        for (auto* a : {&g_walks, &g_skipped, &g_reemits, &g_after_skip, &g_repaired, &g_bytes, &g_rolled, &g_armed, &g_missed, &g_no_blend, &g_nested, &g_repair_unsafe, &g_now, &g_late, &g_late_missed})
            a->store(0, std::memory_order_relaxed);
        for (int k = 0; k < kWhys; ++k) g_refused[k] = 0, g_refused_entry[k] = 0;
        for (int b = 0; b < 2; ++b) {
            g_census_n[b] = 0;
            for (int k = 0; k < kClasses; ++k) g_census[b][k] = 0, g_census_walks[b][k] = 0;
        }
        for (int i = 0; i < kSlots; ++i) rdrvr_rs_skips[i] = 0;
    }
    auto v = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
    uint64_t refused = 0, refused_entry = 0;
    for (int k = 0; k < kWhys; ++k) refused += g_refused[k].load(), refused_entry += g_refused_entry[k].load();
    char b[700];
    std::snprintf(b, sizeof(b),
                  "reemit %s, skip %s: walks %llu, skipped %llu, re-emits %llu, refused %llu, refused at entry %llu, after a skip %llu (repaired %llu, unsafe %llu), "
                  "bytes %llu (chunks rolled %llu), blend null %llu, skips armed %llu, armed but none skipped %llu, nested walks %llu, at a cut walk's end %llu",
                  g_live.load() ? "on" : "off",
                  !g_skip_installed.load() ? "not installed" : g_skip_failed.load() ? "stood down" : g_skip_live.load() ? "on" : "off", v(g_walks), v(g_skipped),
                  v(g_reemits), static_cast<unsigned long long>(refused), static_cast<unsigned long long>(refused_entry), v(g_after_skip), v(g_repaired), v(g_repair_unsafe),
                  v(g_bytes), v(g_rolled), v(g_no_blend), v(g_armed), v(g_missed), v(g_nested), v(g_now));
    std::string out = b;
    std::snprintf(b, sizeof(b), ", variant %s (late re-emits %llu, late walks unended %llu)", kVariantName[g_variant.load()], v(g_late), v(g_late_missed));
    out += b;
    out += " | refused by reason:";
    for (int k = 0; k < kWhys; ++k) {
        std::snprintf(b, sizeof(b), " %s %llu/%llu;", kWhyName[k], v(g_refused[k]), v(g_refused_entry[k]));
        out += b;
    }
    for (int bucket = 1; bucket >= 0; --bucket) {
        std::snprintf(b, sizeof(b), " | census %s (%llu walks):", bucket ? "skipped" : "recorded", v(g_census_n[bucket]));
        out += b;
        for (int k = 0; k < kClasses; ++k) {
            std::snprintf(b, sizeof(b), " %s %llu/%llu", kClsName[k], v(g_census[bucket][k]), v(g_census_walks[bucket][k]));
            out += b;
        }
    }
    if (g_skip_installed.load()) {
        out += " | skips by op:";
        for (int i = 0; i < kSlots; ++i) {
            std::snprintf(b, sizeof(b), " %x:%llu", kSlotOp[i], static_cast<unsigned long long>(rdrvr_rs_skips[i]));
            out += b;
        }
    }
    return out;
}
}  // namespace rdrvr::rec_state

// draw_masks.asm's tail jump (pass 2's pre-pass walk on the render thread, while armed)
extern "C" void rdrvr_prepass_walk_wrap(void* pass, void* viewport, void* a3, unsigned a4) { rdrvr::rec_state::walk(pass, viewport, a3, a4); }
