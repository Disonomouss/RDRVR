// [Stereo] RouteFCensus (GOAL-single-pass item 8h' step 1; test only, off by default): route F's state census (fcensus.h,
// research\run25\item8f_census.md). Read only: the CPU-side caches DrawVisEntity and the bucket callbacks leave, snapshotted at
// five points of DVL call 1's e1 in both passes of a double frame, pass 1's kept until pass 2 reaches the same point (the
// render thread only; the frame keyed by dual_pass's double-frame count, so stale data is never compared). Nothing is skipped,
// written or called in the game: every read of its memory is a memcpy under SEH (rd, raw), each pointer read checked for
// null, each CB's size bounded (kCbMax, kCbTotal).
//   A snapshot is one blob: the fixed fields (built once at install, their addresses fixed on the analysed build), then the
// global shader-variable CB shadows (laid out at pass 1's DVL call 1 open from the variable table, rebuilt when the table
// changes). Each region of the blob is split into elements (a render state, a device word, a CB variable): the predicate is
// judged per element, the failures counted per field and point (frame level) and per element (its key: a fixed element's
// index, a CB variable's id, a textures-CB word).
//   The predicate: route F leaves L = (D1 ? X1 : E2) at P_b (pass 2's entities skipped, the D1 elements written back to pass
// 1's values); ok = (L == X2). The spec's first rule (D1 == D2 and (not D1 or X1 == X2)) is kept as "strict": it differs only
// where D1 and not D2 with X1 == X2, exact for route F. At F: L = X1F over bucket 1's D1 elements (the write-back's values),
// so both rules are X1F == X2F there.
//   The cursor class (not judged): the device cache's two arrays of three std::vectors (begin, end, cap; one per frame slot,
// 0x142b073f0 and 0x142b07470) of the state objects the pre-draw flush 0x14a150 creates and binds (blend, vt+0x48; the other
// of 0x142b074b8's desc, vt+0x58): a keep-alive list, its references released at the slot's next frame (0x12f0c0) [V]. Their
// values carry no render state (read only by push_back, the release and the destructor) [V]: reported as how many pushes pass
// 2's entities did (what route F leaves pass 2's end behind by), never compared.
//   Route F's probe (item 8h' step 2, fcensus.h; test only, the simulator only): the same points and snapshots. Pass 1 of a
// skipwb frame snapshots at E0/P0/E1/P1 (the census off: those only, timed) and lists at P_b the write-back set's elements its
// bucket changed; pass 2 skips the e1 entities of buckets 0 and 1 and writes those values back at P_b. With the census on as
// well, its pass-2 snapshots come after the write-back: it judges exactly what the probe leaves.

#include "core/fcensus.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/dual_pass.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/main_instanced.h"
#include "core/main_region.h"
#include "core/pass2_skip.h"

#pragma intrinsic(_ReturnAddress)

namespace rdrvr::fcensus {
std::atomic<bool> g_on_flag{false};
std::atomic<int> g_probe_flag{0};
std::atomic<bool> g_seg_flag{false};
namespace {
using anchors::Id;

// ---- the analysed build's addresses (RVAs; base 0x140000000 in Ghidra), read in the design [V] and checked at install where
// they are code. The census installs only on that build (exe_matches, not relocated).
constexpr uintptr_t kDveCall = 0x6421a9;      // DrawVisList's entity loop: call DrawVisEntity (return 0x6421ae)
constexpr uintptr_t kB0Jmp = 0x5c1968;        // the stub 0x5c1950 (bucket 0's post): jmp DvlPostB0Body, its only caller
constexpr uintptr_t kPostRet[6] = {0x64220e, 0x64222f, 0x642239, 0x642247, 0x642254, 0x642265};  // DVL's post-callback calls
                                                                                                 // (call [rbx+0x38]), by kind
constexpr uintptr_t kRsShadow = 0x2ac0b60;    // RenderStateSet's shadow, 4 bytes a state, 0..0x2b (its own prologue's operand)
constexpr uintptr_t kBiasBase = 0x2ac557c;    // DVL's depth-bias reset value (list+0x10 and render state 0xc before each post)
constexpr uintptr_t kOvr = 0x2272488, kOvrByte = 0x2ac1d47;  // the technique override and its byte (field 11's asserts)
constexpr uintptr_t kWriteGate = 0x2ac09f0;   // the global-write gate (u64; DrawVisEntity's AO write needs it 0)
constexpr uintptr_t kFrameCounter = 0x2ac1ea0;  // the render frame counter (the grmMatrixSet stamp's, mset +0xd0)
constexpr uintptr_t kFadeId = 0x2ac2854, kAoId = 0x22e5aec;  // the fade's and the AO modifier's cached variable ids
constexpr uintptr_t kRimObj = 0x2ad1fc8;      // the rim object (a pointer): +0x30/+0x34 RimAmount's and RimGlowColor's ids
constexpr uintptr_t kTreeObj = 0x2c8ab30;     // the tree-wind object (static, 0x59a510 returns it): +0x10 a cached id
constexpr uintptr_t kTreeIds[4] = {0x22f86f0, 0x22f86f4, 0x22f86fc, 0x22f8700};  // the tree-wind caches' variable ids
constexpr uint32_t kFa6 = 0x1fa6;             // renderer +0x1fa6: the skeleton pre-draw's special mode
constexpr uint32_t kDevSlot = 0xb0;           // field 12: the grc context's vt+0xb0 (its storage unknown: logged once)

enum Field : uint8_t { kFRs, kFDev, kFFade, kFAo, kFRim, kFTree, kFLight, kFWorld, kFCb, kFOvr, kFields };
const char* const kFieldName[kFields] = {"rs", "dev", "fade", "ao", "rim", "tree", "light", "world", "cb", "ovr"};
enum Point : int { kE0, kP0, kE1, kP1, kF, kPoints };
const char* const kPointName[kPoints] = {"E0", "P0", "E1", "P1", "F"};
constexpr int kCmp = 3;  // the judged points: P0 (bucket 0), P1 (bucket 1), F
const char* const kCmpName[kCmp] = {"P0", "P1", "F"};
constexpr uint32_t bit(int p) { return 1u << p; }

// ---- the layout
enum Kind : uint8_t { kAbs, kPtr, kCb };
struct FixedDef {
    const char* name;
    uint8_t field, kind;
    uintptr_t rva;    // abs: the bytes; ptr: the pointer
    uint32_t off;     // ptr: from the pointer
    uint32_t len;
    uint32_t word;    // element size (0: the region is one element)
    uint32_t nb;      // the elements' names' base offset
};
// fields 1-11 of the design's section 2 (5 and 11's 0x142272488/0x142ac1d47 asserted, not compared; 12 not stored)
const FixedDef kFixedDefs[] = {
    {"rs", kFRs, kAbs, 0x2ac0b60, 0, 0x30, 4, 0},  // states 0..0xb (0xc excluded: equal by DVL's reset)
    {"rs", kFRs, kAbs, 0x2ac0b94, 0, 0x7c, 4, 0x34},  // states 0xd..0x2b
    {"devstate", kFDev, kAbs, 0x2272490, 0, 0xc4, 4, 0},  // 0x142272490..0x142272553 (the dirty flags outside: not compared)
    {"devcache", kFDev, kAbs, 0x2b07398, 0, 0x150, 4, 0},  // 0x142b07398..0x142b074e7 (+0x58..+0x9f, +0xd8..+0x11f: cursors)
    {"fade", kFFade, kAbs, 0x2b0d0f0, 0, 4, 0, 0},
    {"ao", kFAo, kAbs, 0x2b6f758, 0, 8, 0, 0},
    {"aoflag", kFAo, kAbs, 0x22e5afd, 0, 1, 0, 0},
    {"rimobj", kFRim, kPtr, kRimObj, 0x24, 0x14, 4, 0x24},  // +0x24 (temporary), +0x28, +0x2c (0 or 2.0), +0x30/+0x34 (ids)
    {"rimstatic", kFRim, kAbs, 0x5a494490, 0, 0x20, 16, 0},  // 0x19a494490 RimAmount, 0x19a4944a0 RimGlowColor
    {"treecache", kFTree, kAbs, 0x22f86c0, 0, 0x30, 4, 0},
    {"treebyte", kFTree, kAbs, 0x2ad1ca6, 0, 1, 0, 0},
    {"treeobj", kFTree, kAbs, kTreeObj + 0x70, 0, 0x70, 4, 0x70},
    {"lightcursor", kFLight, kPtr, 0x2c8ac98, 0x1090, 8, 0, 0x1090},  // *(*(u64*)0x142c8ac98 + 0x1090)
    {"world", kFWorld, kPtr, 0x2ac0a10, 0, 0x40, 0, 0},  // ViewportCurrent +0x00
    {"worldcopy", kFWorld, kPtr, 0x2ac0a10, 0x80, 0x40, 0, 0x80},
    {"ovr484", kFOvr, kAbs, 0x2272484, 0, 4, 0, 0},
};
// the cursor class: the device cache's keep-alive vectors, 3 frame slots of {begin, end, cap} each (FUN_14014a150 pushes the
// bound state object at the current slot 0x142c37220's end; FUN_14012f0c0 releases [begin, end) and sets end = begin) [V]
constexpr uintptr_t kCursorVec[2] = {0x2b073f0, 0x2b07470};
constexpr uint32_t kCursorSlots = 3, kCursorStride = 0x18, kCursorLen = kCursorSlots * kCursorStride;
enum Cls : uint8_t { kJudged, kCursor };
constexpr int kMaxRegions = 192, kMaxElems = 4096, kMaxVars = 4096, kMaxFixedElems = 512, kTexWords = 64;
constexpr uint32_t kFixedBlob = 0x800, kCbMax = 0x4000, kCbTotal = 0x10000, kBlob = kFixedBlob + kCbTotal;
// the failure keys: a fixed element's index, a CB variable's id, a CB's bytes before its first variable, a textures-CB word
constexpr int kKeyVar = kMaxFixedElems, kKeyHead = kKeyVar + kMaxVars, kKeyTex = kKeyHead + 1, kKeyTexRest = kKeyTex + kTexWords,
              kKeys = kKeyTexRest + 1;

struct Region {
    const char* name;  // (fixed) a literal; a CB's is named by its index
    uint8_t field, kind;
    bool tex;          // (cb) the textures CB
    uintptr_t addr;    // abs: the bytes; ptr: the pointer; cb: the CB object
    uint32_t off, len, at;  // ptr's offset, the bytes, where in the blob
    uint32_t e0, en;   // its elements
    uintptr_t storage;  // (cb) its storage at the layout's build: a snapshot that finds another one marks it unread
    int first_id;      // (cb) its lowest variable id
};
struct Elem {
    uint32_t at, len;  // in the blob
    uint32_t roff;     // (names) the offset within its region, from the region's name base
    uint16_t region;
    uint8_t cls;       // kJudged, or kCursor (not compared)
    int32_t key;
};
struct Snap {
    bool taken;
    uint8_t valid[kMaxRegions];
    uint8_t blob[kBlob];  // (read with memcpy only: no alignment needed)
};

std::atomic<bool> g_installed{false};
uintptr_t g_base = 0, g_dvl_ret = 0, g_dve_ret = 0;
uintptr_t g_post_ret[6] = {};
int g_rcur = -1;           // the region holding the cursor vectors (devcache)
uint32_t g_rcur_off[2] = {};  // the vectors' offsets in it
// route F's probe: the fixed regions its write-back uses (found by name at install)
int g_ri_ao = -1, g_ri_aoflag = -1, g_ri_rimobj = -1, g_ri_rimstatic = -1, g_ri_world = -1, g_ri_worldcopy = -1;

// the layout and the snapshots: the render thread's (the fixed part, built at install, is read-only afterwards)
Region g_reg[kMaxRegions];
int g_nreg = 0, g_nfixed = 0;
Elem g_el[kMaxElems];
int g_nel = 0, g_nfixed_el = 0;
Snap g_p1[kPoints], g_p2e[2], g_cur;
uint8_t g_tab[(kMaxVars + 1) * 16], g_tab_new[(kMaxVars + 1) * 16];  // the variable table (16 bytes an id: the CB, the offset)
int g_tab_n = -1;
uintptr_t g_tab_tex = 0;
bool g_rebuild = true;
struct Var {
    int region, off, id;
};
Var g_vars[kMaxVars];
struct Known {
    int id;
    const char* name;
};
constexpr int kMaxKnown = 24;
Known g_known[kMaxKnown];  // (the render thread's; a copy for the commands under g_mx)
int g_nknown = 0;

// ---- SEH reads (no objects with destructors in these functions)
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

// ---- the counters
struct PassAcc {  // the open pass's (render thread), folded into the totals at its close
    uint64_t hits[kPoints];
    uint64_t a5_n, a5_fail, a11_n, a11_fail, ovr_p1;  // asserts #5 and #11; P1's override not -1 (info)
    uint64_t gate_nz;                                  // points where 0x142ac09f0 was not 0
    uint64_t ent, stamp_n, stamp_diff, qslot, qslot2;  // e1's entity calls from DVL's loop; (pass 2) stamps, query slots
    uint64_t cb_moved, order, foreign;                 // a CB's storage moved; a point out of order; a post from another caller
};
struct Fail {
    uint64_t frame;
    uint8_t point, field, has;  // has: 1 pass 1's E value, 2 pass 2's
    uint32_t len;
    char name[48];
    uint64_t v[4];  // the element's first 8 bytes: pass 1's E, X; pass 2's E, X (at F: X only)
};
struct FrameSt {  // the double frame in flight (render thread)
    bool p1, p2, stale, fa6;
    bool snaps;  // pass 1 took its snapshots (the census on at its open, or route F's probe latched skipwb)
    int pmode;   // route F's probe, latched at pass 1's open: 0 none, 1 skip, 2 skipwb
    uint64_t key;
    uint32_t gen, p1_seen, p2_eval;  // p2_eval: bit 0 P0 judged, 1 P1, 2 F
    bool d1_any, d2_any, fail_any, sfail_any, bmis;  // (fail: the L rule; sfail: the strict rule)
    uint8_t ffail[kFields][kCmp], sffail[kFields][kCmp], d1f[kFields][2], d2f[kFields][2];
    uint8_t d1only[kFields][2], d2only[kFields][2];  // an element D1 and not D2; not D1 and D2 (by bucket)
    // the cursor class: pass 1's and pass 2's pushes by bucket (E_b -> P_b; 0 when no entity), a slot's begin moved (a
    // reallocation, or a release inside e1: the delta not used); the class's own L rule failed (info)
    int64_t cur1[2], cur2[2];
    bool cur_bad[2], cfail[kCmp];
    uint32_t unread[kFields][kCmp];
    uint32_t ent[2][2];  // entity calls by pass and bucket (the same list in both passes)
    Fail cand[16];
    int ncand;
    uint64_t failbits[kMaxElems / 64], d1b1[kMaxElems / 64];  // the elements that failed; D1 at bucket 1 (F's set)
};
FrameSt g_fr;
PassAcc g_pa;
uint64_t g_frame_no = 0;
std::atomic<uintptr_t> g_dev_b0{0};  // (field 12's slot target: the render thread writes it once)
bool g_dev_logged = false;

struct Layout {  // (under g_mx) the CB layout's summary
    int vars, cbs, derived, skipped, elems, rebuilds;
    uint32_t bytes;
    uint64_t elem_over, bad;
};
struct Totals {
    uint64_t frames, ok, sok, d2none, d1none, incomplete, stale, unpaired, bmis, ent_mis, fa6, thread;
    uint64_t ffail[kFields][kCmp], sffail[kFields][kCmp], unread[kFields][kCmp], d1f[kFields][2], d2f[kFields][2];
    uint64_t d1only[kFields][2], d2only[kFields][2];
    // the cursor class by point (P0, P1, F; at F the lag is both buckets' pass-2 pushes): frames measured, pass 2's pushes
    // summed and their largest, frames with any, pass 1's pushes summed, frames not measured (a begin moved), the class's L
    // rule failing (frames)
    uint64_t cur_n[kCmp], cur2_sum[kCmp], cur2_max[kCmp], cur2_nz[kCmp], cur1_sum[kCmp], cur_bad[kCmp], cur_fail[kCmp];
    PassAcc pass[2];
    uint64_t fail_key[kKeys];
    Fail first[16];
    int nfirst;
};
std::mutex g_mx;
Totals g_t;
Layout g_lay;
Known g_known_cmd[kMaxKnown];
int g_nknown_cmd = 0;
std::atomic<uint32_t> g_gen{1};
std::atomic<DWORD> g_owner{0};
double g_window_ms = 0;
// the render thread's DVL nesting (call 1 at depth 1) and the census'd call 1's state
thread_local int t_depth = 0, t_cpass = 0;
thread_local bool t_e1 = false;
thread_local uintptr_t t_list = 0;
thread_local uint32_t t_seen = 0;
thread_local bool t_census = false;  // the census on at this call's open (its own work only then)
// route F's probe, this pass 2's: latched (a frame of the probe), skipping, writing back; e1's end met; the entities skipped
thread_local bool t_platched = false, t_pskip = false, t_pwb = false, t_pf = false;
thread_local uint32_t t_pskipped = 0, t_pskb[2] = {};
thread_local uint32_t t_pq2 = 0, t_pqz = 0, t_pm1 = 0, t_pmo = 0;  // (by the entity's arg 7: mode 2, its slots zeroed; mode 1; other)

// ---- the layout's build
uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }
void add_elems(Region& r, uint32_t word, uint32_t nb) {  // (fixed) words of the region, or one element
    r.e0 = static_cast<uint32_t>(g_nel);
    const uint32_t w = word ? word : r.len;
    for (uint32_t o = 0; o < r.len && g_nel < kMaxFixedElems; o += w) {
        Elem& e = g_el[g_nel];
        e.at = r.at + o;
        e.len = std::min(w, r.len - o);
        e.roff = nb + o;
        e.region = static_cast<uint16_t>(&r - g_reg);
        e.cls = kJudged;
        if (r.kind == kAbs)
            for (int v = 0; v < 2; ++v) {
                const uintptr_t a = r.addr + o, lo = g_base + kCursorVec[v];
                if (a >= lo && a < lo + kCursorLen) e.cls = kCursor;
            }
        e.key = g_nel;
        ++g_nel;
    }
    r.en = static_cast<uint32_t>(g_nel) - r.e0;
}
bool build_fixed() {
    uint32_t at = 0;
    for (const FixedDef& d : kFixedDefs) {
        Region& r = g_reg[g_nreg++];
        r = Region{};
        r.name = d.name;
        r.field = d.field;
        r.kind = d.kind;
        r.addr = g_base + d.rva;  // (abs: the bytes; ptr: the pointer's own address)
        r.off = d.off;
        r.len = d.len;
        r.at = at;
        at = align16(at + d.len);
        add_elems(r, d.word, d.nb);
        if (d.kind == kAbs && d.rva <= kCursorVec[0] && kCursorVec[1] + kCursorLen <= d.rva + d.len && d.word == 4) {
            g_rcur = g_nreg - 1;
            for (int v = 0; v < 2; ++v) g_rcur_off[v] = static_cast<uint32_t>(kCursorVec[v] - d.rva);
        }
    }
    g_nfixed = g_nreg;
    g_nfixed_el = g_nel;
    for (int i = 0; i < g_nfixed; ++i) {  // (route F's probe: the regions its write-back reads)
        const char* n = g_reg[i].name;
        int* ri = !std::strcmp(n, "ao") ? &g_ri_ao : !std::strcmp(n, "aoflag") ? &g_ri_aoflag : !std::strcmp(n, "rimobj") ? &g_ri_rimobj
                  : !std::strcmp(n, "rimstatic") ? &g_ri_rimstatic : !std::strcmp(n, "world") ? &g_ri_world : !std::strcmp(n, "worldcopy") ? &g_ri_worldcopy
                  : nullptr;
        if (ri) *ri = i;
    }
    return at <= kFixedBlob && g_nel < kMaxFixedElems && g_rcur >= 0 && g_ri_ao >= 0 && g_ri_aoflag >= 0 && g_ri_rimobj >= 0 && g_ri_rimstatic >= 0 &&
           g_ri_world >= 0 && g_ri_worldcopy >= 0;
}

bool tab_id_ok(int h, int n) { return h > 0 && h <= n; }
uintptr_t tab_cb(int i) {
    uintptr_t cb = 0;
    std::memcpy(&cb, g_tab + static_cast<size_t>(i) * 16, 8);
    return cb;
}
int tab_off(int i) {
    int off = 0;
    std::memcpy(&off, g_tab + static_cast<size_t>(i) * 16 + 8, 4);
    return off;
}
void known_add(int id, const char* name) {
    if (id > 0 && g_nknown < kMaxKnown) g_known[g_nknown++] = Known{id, name};
}
const char* known_name(const Known* k, int n, int id) {
    for (int i = 0; i < n; ++i)
        if (k[i].id == id) return k[i].name;
    return nullptr;
}
int read_id(uintptr_t a) {
    int v = 0;
    return rd(a, &v) ? v : 0;
}

// (the render thread, at pass 1's DVL call 1 open) the CB part of the layout, from the variable table: each CB its region,
// each variable its element (its bytes up to the next variable's offset); the CBs of the per-eye camera variables (gWorldView,
// gViewInverse, gPrevWorldViewProjSingle, gPrevViewProj, gViewport) left out as derived (judged through the world, field 9)
void build_cbs() {
    int n = 0;
    uintptr_t tcb = 0;
    const bool ok = rd(anchors::addr(Id::GlobalVarCount), &n) && n >= 1 && n < kMaxVars &&
                    raw(anchors::addr(Id::ShaderGlobalTable), g_tab_new, static_cast<size_t>(n + 1) * 16);
    rd(anchors::addr(Id::GlobalTexturesCb), &tcb);
    if (!ok) {  // (no CB part this frame: the fixed fields only)
        g_nreg = g_nfixed;
        g_nel = g_nfixed_el;
        g_tab_n = -1;
        std::lock_guard lk(g_mx);
        ++g_lay.bad;
        return;
    }
    const size_t tb = static_cast<size_t>(n + 1) * 16;
    if (!g_rebuild && n == g_tab_n && tcb == g_tab_tex && std::memcmp(g_tab, g_tab_new, tb) == 0) return;
    std::memcpy(g_tab, g_tab_new, tb);
    g_tab_n = n;
    g_tab_tex = tcb;
    g_rebuild = false;
    g_nreg = g_nfixed;
    g_nel = g_nfixed_el;
    // the names: the game's own cached ids (never the hash 0x0c7510)
    g_nknown = 0;
    uintptr_t rim = 0;
    rd(g_base + kRimObj, &rim);
    known_add(read_id(g_base + kFadeId), "fade");
    known_add(read_id(g_base + kAoId), "AOModifier");
    if (rim) {
        known_add(read_id(rim + 0x30), "RimAmount");
        known_add(read_id(rim + 0x34), "RimGlowColor");
    }
    const char* const tree_names[4] = {"tree_f0", "tree_f4", "tree_fc", "tree_100"};
    for (int k = 0; k < 4; ++k) known_add(read_id(g_base + kTreeIds[k]), tree_names[k]);
    known_add(read_id(g_base + kTreeObj + 0x10), "treeobj_id");
    struct Named {
        Id id;
        const char* name;
        bool derived;
    };
    const Named named[] = {{Id::HandleWorldView, "gWorldView", true},       {Id::HandleViewInverse, "gViewInverse", true},
                           {Id::HandlePrevWorldVP, "gPrevWorldViewProjSingle", true}, {Id::HandlePrevViewProj, "gPrevViewProj", true},
                           {Id::HandleViewport, "gViewport", true},          {Id::WindParamsId, "gWindParams", false},
                           {Id::ShadowCamMtxId, "gCameraMatrix", false},     {Id::ShadowPixelSizeId, "Shadowmap_PixelSize0", false}};
    uintptr_t derived[8] = {};
    int nd = 0;
    for (const Named& m : named) {
        if (!anchors::present(m.id)) continue;
        const int h = read_id(anchors::addr(m.id));
        known_add(h, m.name);
        if (m.derived && tab_id_ok(h, n) && tab_cb(h)) {
            const uintptr_t cb = tab_cb(h);
            bool dup = false;
            for (int k = 0; k < nd; ++k) dup = dup || derived[k] == cb;
            if (!dup) derived[nd++] = cb;
        }
    }
    // the CBs, in the order of their lowest id; skipped: derived, unreadable, too large or past the blob
    uintptr_t skipped[kMaxRegions];
    int nskip = 0, nv = 0;
    uint32_t at = kFixedBlob;
    for (int i = 1; i <= n; ++i) {
        const uintptr_t cb = tab_cb(i);
        const int off = tab_off(i);
        if (!cb || off < 0) continue;
        int r = -1;
        for (int k = g_nfixed; k < g_nreg && r < 0; ++k)
            if (g_reg[k].addr == cb) r = k;
        if (r < 0) {
            bool skip = false;
            for (int k = 0; k < nskip && !skip; ++k) skip = skipped[k] == cb;
            if (skip) continue;
            bool is_derived = false;
            for (int k = 0; k < nd; ++k) is_derived = is_derived || derived[k] == cb;
            uintptr_t st = 0, layout = 0;
            int size = 0;
            const bool readable = rd(cb, &st) && st && rd(cb + 8, &layout) && layout && rd(layout + 0x14, &size);
            if (is_derived || !readable || size <= 0 || static_cast<uint32_t>(size) > kCbMax ||
                at + static_cast<uint32_t>(size) > kBlob || g_nreg >= kMaxRegions) {
                if (nskip < kMaxRegions) skipped[nskip++] = cb;
                continue;
            }
            r = g_nreg++;
            Region& g = g_reg[r];
            g = Region{};
            g.name = nullptr;
            g.field = kFCb;
            g.kind = kCb;
            g.addr = cb;
            g.len = static_cast<uint32_t>(size);
            g.at = at;
            g.storage = st;
            g.first_id = i;
            at = align16(at + g.len);
        }
        if (nv < kMaxVars) g_vars[nv++] = Var{r, off, i};
    }
    std::sort(g_vars, g_vars + nv, [](const Var& a, const Var& b) {
        return a.region != b.region ? a.region < b.region : a.off != b.off ? a.off < b.off : a.id < b.id;
    });
    uint64_t over = 0;
    auto elem = [&](int r, uint32_t o, uint32_t len, int32_t key) {
        if (g_nel >= kMaxElems) {
            ++over;
            return false;
        }
        Elem& e = g_el[g_nel++];
        e.at = g_reg[r].at + o;
        e.len = len;
        e.roff = o;
        e.region = static_cast<uint16_t>(r);
        e.cls = kJudged;
        e.key = key;
        return true;
    };
    int v = 0;
    for (int r = g_nfixed; r < g_nreg; ++r) {
        Region& g = g_reg[r];
        g.e0 = static_cast<uint32_t>(g_nel);
        while (v < nv && g_vars[v].region < r) ++v;
        uint32_t cur = 0;
        bool room = true;
        while (v < nv && g_vars[v].region == r && room) {
            const uint32_t off = static_cast<uint32_t>(g_vars[v].off);
            const int id = g_vars[v].id;
            while (v < nv && g_vars[v].region == r && static_cast<uint32_t>(g_vars[v].off) == off) ++v;  // (aliases: the lowest id)
            if (off >= g.len) break;
            if (off > cur) room = elem(r, cur, off - cur, kKeyHead);  // (bytes before the first variable)
            uint32_t next = g.len;
            if (v < nv && g_vars[v].region == r && static_cast<uint32_t>(g_vars[v].off) < g.len) next = static_cast<uint32_t>(g_vars[v].off);
            if (room) room = elem(r, off, next - off, kKeyVar + id);
            cur = next;
        }
        if (cur < g.len && g_nel < kMaxElems) elem(r, cur, g.len - cur, kKeyHead);  // (the rest: no variable, or no room)
        else if (cur < g.len) ++over;
        while (v < nv && g_vars[v].region == r) ++v;
        g.en = static_cast<uint32_t>(g_nel) - g.e0;
    }
    // the textures CB (its storage at +0, written per texture register by 0x149140): words
    bool tex_in = false;
    for (int r = g_nfixed; r < g_nreg; ++r) tex_in = tex_in || g_reg[r].addr == tcb;
    uintptr_t tst = 0, tlay = 0;
    int tsize = 0;
    if (tcb && !tex_in && g_nreg < kMaxRegions && rd(tcb, &tst) && tst && rd(tcb + 8, &tlay) && tlay && rd(tlay + 0x14, &tsize) &&
        tsize > 0 && static_cast<uint32_t>(tsize) <= kCbMax && at + static_cast<uint32_t>(tsize) <= kBlob) {
        const int r = g_nreg++;
        Region& g = g_reg[r];
        g = Region{};
        g.name = "tex";
        g.field = kFCb;
        g.kind = kCb;
        g.tex = true;
        g.addr = tcb;
        g.len = static_cast<uint32_t>(tsize);
        g.at = at;
        g.storage = tst;
        g.e0 = static_cast<uint32_t>(g_nel);
        uint32_t o = 0;
        for (int w = 0; w < kTexWords && o + 4 <= g.len && elem(r, o, 4, kKeyTex + w); ++w) o += 4;
        if (o < g.len) elem(r, o, g.len - o, kKeyTexRest);
        g.en = static_cast<uint32_t>(g_nel) - g.e0;
        at = align16(at + g.len);
    }
    std::lock_guard lk(g_mx);
    g_lay.vars = n;
    g_lay.cbs = g_nreg - g_nfixed;
    g_lay.derived = nd;
    g_lay.skipped = nskip;
    g_lay.elems = g_nel;
    g_lay.bytes = at - kFixedBlob;
    g_lay.elem_over += over;
    ++g_lay.rebuilds;
    std::memcpy(g_known_cmd, g_known, sizeof(g_known));
    g_nknown_cmd = g_nknown;
}

// ---- the snapshot (render thread)
void take(Snap& s) {
    for (int i = 0; i < g_nreg; ++i) {
        const Region& r = g_reg[i];
        uintptr_t a = 0;
        if (r.kind == kAbs) {
            a = r.addr;
        } else if (r.kind == kPtr) {
            uintptr_t p = 0;
            if (rd(r.addr, &p) && p) a = p + r.off;
        } else {
            uintptr_t st = 0;
            if (rd(r.addr, &st) && st) {
                if (st == r.storage) {
                    a = st;
                } else {
                    ++g_pa.cb_moved;  // (unread this frame; the layout rebuilt at the next pass 1)
                    g_rebuild = true;
                }
            }
        }
        s.valid[i] = a && raw(a, s.blob + r.at, r.len) ? 1 : 0;
    }
    s.taken = true;
}

// ---- the names (any thread: the fixed part is read-only after install; the known ids from the caller's copy)
void key_name(int key, const Known* k, int nk, char* out, size_t len) {
    if (key >= 0 && key < g_nfixed_el) {
        const Elem& e = g_el[key];
        const Region& r = g_reg[e.region];
        if (r.field == kFRs) std::snprintf(out, len, "rs[%#x]", e.roff / 4);
        else std::snprintf(out, len, "%s+%#x", r.name, e.roff);
    } else if (key >= kKeyVar && key < kKeyHead) {
        const char* nm = known_name(k, nk, key - kKeyVar);
        if (nm) std::snprintf(out, len, "%s", nm);
        else std::snprintf(out, len, "v%d", key - kKeyVar);
    } else if (key == kKeyHead) {
        std::snprintf(out, len, "cbhead");
    } else if (key >= kKeyTex && key < kKeyTexRest) {
        std::snprintf(out, len, "tex+%#x", (key - kKeyTex) * 4);
    } else {
        std::snprintf(out, len, "texrest");
    }
}

// ---- the predicate (render thread, pass 2)
void fail(int ci, int e, const Snap* e1, const Snap& x1, const Snap* e2, const Snap& x2) {
    const Elem& el = g_el[e];
    const Region& r = g_reg[el.region];
    g_fr.fail_any = true;
    g_fr.ffail[r.field][ci] = 1;
    g_fr.failbits[e >> 6] |= 1ull << (e & 63);
    if (g_fr.ncand >= 16) return;
    Fail& f = g_fr.cand[g_fr.ncand++];
    f.frame = g_frame_no;
    f.point = static_cast<uint8_t>(ci);
    f.field = r.field;
    f.has = static_cast<uint8_t>((e1 ? 1 : 0) | (e2 ? 2 : 0));
    f.len = el.len;
    const uint32_t n = std::min<uint32_t>(el.len, 8);
    const Snap* s[4] = {e1, &x1, e2, &x2};
    for (int k = 0; k < 4; ++k) {
        f.v[k] = 0;
        if (s[k]) std::memcpy(&f.v[k], s[k]->blob + el.at, n);
    }
    key_name(el.key, g_known, g_nknown, f.name, sizeof(f.name));
    if (r.kind == kCb && el.key == kKeyHead) {  // (which CB's)
        const size_t used = std::strlen(f.name);
        std::snprintf(f.name + used, sizeof(f.name) - used, "%d+%#x", el.region - g_nfixed, el.roff);
    }
}

// the cursor class: the pushes between e and x (the six ends' advance, in 8-byte references); false when a slot's begin moved,
// an end went back or the region is unread (not measured)
bool cursor_push(const Snap& e, const Snap& x, int64_t* out) {
    *out = 0;
    const Region& r = g_reg[g_rcur];
    if (!e.valid[g_rcur] || !x.valid[g_rcur]) return false;
    for (int v = 0; v < 2; ++v)
        for (uint32_t s = 0; s < kCursorSlots; ++s) {
            const uint32_t o = r.at + g_rcur_off[v] + s * kCursorStride;
            uint64_t be = 0, bx = 0, ee = 0, ex = 0;
            std::memcpy(&be, e.blob + o, 8);
            std::memcpy(&bx, x.blob + o, 8);
            std::memcpy(&ee, e.blob + o + 8, 8);
            std::memcpy(&ex, x.blob + o + 8, 8);
            if (be != bx || ex < ee) return false;
            *out += static_cast<int64_t>((ex - ee) / 8);
        }
    return true;
}

// bucket b at P_b: pass 1's E_b (absent: no entity), X = P_b; pass 2's E_b, X = g_cur. Route F leaves L = D1 ? X1 : E2 (E2 = X2
// when pass 2 drew no entity there): ok = L == X2; strict = D1 == D2 and (not D1 or X1 == X2)
void judge_bucket(int b) {
    const Snap* e1 = g_p1[b == 0 ? kE0 : kE1].taken ? &g_p1[b == 0 ? kE0 : kE1] : nullptr;
    const Snap& x1 = g_p1[b == 0 ? kP0 : kP1];
    const Snap* e2 = g_p2e[b].taken ? &g_p2e[b] : nullptr;
    const Snap& x2 = g_cur;
    if (!e1 != !e2) g_fr.bmis = true;  // (an entity in one pass's bucket only: the list should be the same)
    const int ci = b;
    if (b == 1) std::memset(g_fr.d1b1, 0, sizeof(g_fr.d1b1));
    bool ok1 = true, ok2 = true;
    if (e1) ok1 = cursor_push(*e1, x1, &g_fr.cur1[b]);
    if (e2) ok2 = cursor_push(*e2, x2, &g_fr.cur2[b]);
    g_fr.cur_bad[b] = !ok1 || !ok2;
    for (int i = 0; i < g_nreg; ++i) {
        const Region& r = g_reg[i];
        if (!x1.valid[i] || !x2.valid[i] || (e1 && !e1->valid[i]) || (e2 && !e2->valid[i])) {
            ++g_fr.unread[r.field][ci];
            continue;
        }
        const bool r1 = e1 && std::memcmp(x1.blob + r.at, e1->blob + r.at, r.len) != 0;
        const bool r2 = e2 && std::memcmp(x2.blob + r.at, e2->blob + r.at, r.len) != 0;
        if (!r1 && !r2) continue;  // (no element changed in either pass: ok)
        for (uint32_t k = r.e0; k < r.e0 + r.en; ++k) {
            const Elem& el = g_el[k];
            const bool d1 = r1 && std::memcmp(x1.blob + el.at, e1->blob + el.at, el.len) != 0;
            const bool d2 = r2 && std::memcmp(x2.blob + el.at, e2->blob + el.at, el.len) != 0;
            const bool x12 = d1 && std::memcmp(x1.blob + el.at, x2.blob + el.at, el.len) == 0;
            const bool ok = d1 ? x12 : !d2;
            if (el.cls == kCursor) {  // (not judged: the class's own count)
                if (!ok) g_fr.cfail[ci] = true;
                continue;
            }
            if (d1) {
                g_fr.d1_any = true;
                g_fr.d1f[r.field][b] = 1;
                if (b == 1) g_fr.d1b1[k >> 6] |= 1ull << (k & 63);
            }
            if (d2) {
                g_fr.d2_any = true;
                g_fr.d2f[r.field][b] = 1;
            }
            if (d1 && !d2) g_fr.d1only[r.field][b] = 1;
            if (!d1 && d2) g_fr.d2only[r.field][b] = 1;
            if (d1 != d2 || (d1 && !x12)) {  // (strict)
                g_fr.sfail_any = true;
                g_fr.sffail[r.field][ci] = 1;
            }
            if (!ok) fail(ci, static_cast<int>(k), e1, x1, e2, x2);
        }
    }
}

// F: pass 1's X against pass 2's over the elements pass 1's bucket-1 entities changed (what b1 post's prefix does to them; the
// cursor class never in that set). L = X1F there (the write-back's values): the L rule and the strict one are the same
void judge_f() {
    const Snap& x1 = g_p1[kF];
    const Snap& x2 = g_cur;
    for (int i = 0; i < g_nreg; ++i) {
        const Region& r = g_reg[i];
        bool any = false;
        for (uint32_t k = r.e0; k < r.e0 + r.en && !any; ++k) any = (g_fr.d1b1[k >> 6] >> (k & 63)) & 1;
        if (!any) continue;
        if (!x1.valid[i] || !x2.valid[i]) {
            ++g_fr.unread[r.field][2];
            continue;
        }
        if (std::memcmp(x1.blob + r.at, x2.blob + r.at, r.len) == 0) continue;
        for (uint32_t k = r.e0; k < r.e0 + r.en; ++k) {
            const Elem& el = g_el[k];
            if (((g_fr.d1b1[k >> 6] >> (k & 63)) & 1) && std::memcmp(x1.blob + el.at, x2.blob + el.at, el.len) != 0) {
                g_fr.sfail_any = true;
                g_fr.sffail[r.field][2] = 1;
                fail(2, static_cast<int>(k), nullptr, x1, nullptr, x2);
            }
        }
    }
}

// ---- route F's probe (item 8h' step 2; test only): pass 2's e1 entities not drawn, pass 1's exit values written back
using Count = std::atomic<uint64_t>;
inline void bump(Count& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }
inline unsigned long long u(const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); }
void cas_max(Count& c, uint64_t v) {
    uint64_t o = c.load(std::memory_order_relaxed);
    while (v > o && !c.compare_exchange_weak(o, v, std::memory_order_relaxed)) {
    }
}
void cas_min(Count& c, uint64_t v) {  // (0: none yet)
    uint64_t o = c.load(std::memory_order_relaxed);
    while ((o == 0 || v < o) && !c.compare_exchange_weak(o, v, std::memory_order_relaxed)) {
    }
}
// the write-back's fields (by what is written)
enum WbF : int { kWCb, kWAo, kWAoFlag, kWRim, kWRim2c, kWWorld, kWCopy, kWbF };
const char* const kWbName[kWbF] = {"cb", "ao", "aoflag", "rimstatic", "rim2c", "world", "cbcopy"};
// a write's outcome: written; already pass 1's value; the write gate 0x142ac09f0 set (the game's CB stores skip too); its target
// moved or unresolvable (a CB's storage, the rim object, ViewportCurrent, a variable id); a CB mapped to another buffer than
// its storage (never written); an SEH fault (the probe stands down)
enum Put : int { kPutDone, kPutSame, kPutGated, kPutMoved, kPutOther, kPutFault, kPuts };
const char* const kPutName[kPuts] = {"done", "same", "gated", "moved", "other", "fault"};
Count g_pr_calls, g_pr_p2, g_pr_skipped_frames, g_pr_none, g_pr_nof, g_pr_ent, g_pr_ent_max, g_pr_ent_min, g_pr_ent_b[2], g_pr_q2, g_pr_qzero,
    g_pr_mode1, g_pr_mode_other, g_pr_order, g_pr_stand;
// refused, by reason: MainInstanced armed, the p2skip probe on, renderer +0x1fa6 set (at pass 1); pass 1 missing or of another
// frame, the setting changed since pass 1 (at pass 2)
enum Ref : int { kRefMain, kRefP2skip, kRefFa6, kRefStale, kRefChanged, kRefs };
const char* const kRefName[kRefs] = {"main", "p2skip", "fa6", "stale", "changed"};
Count g_pr_ref[kRefs];
Count g_wb_calls[2], g_wb_nosnap, g_wb_res[kWbF][kPuts], g_wb_missing[kFields], g_wb_left[kFields], g_wb_tex_left, g_wb_over, g_wb_rim2c_nod1,
    g_wb_ticks, g_p1_ticks, g_p1_frames;
std::atomic<bool> g_pstood{false};
char g_pwhy[200] = {};  // (under g_mx)
uintptr_t g_setworld = 0;  // ViewportSetWorldMtx (its anchor's signature checked), 0: skipwb refused
double g_qpc_ms = 0;
struct WbList {  // (render thread) bucket b's write-back: the elements pass 1's entities changed (D1), found at pass 1's P_b
    bool built, rim2c_d1;
    int n;
    uint16_t el[kMaxElems];
};
WbList g_wbl[2];

void probe_stand_down(const char* why) {
    g_probe_flag = 0;
    t_pskip = t_pwb = false;
    if (g_pstood.exchange(true)) return;
    bump(g_pr_stand);
    {
        std::lock_guard lk(g_mx);
        std::snprintf(g_pwhy, sizeof(g_pwhy), "%s", why);
    }
    log::error("[fcensus] route F's probe: %s: the probe stands down for the session", why);
}

// ---- its SEH reads and writes (no objects with destructors in these functions): 1 read, 0 a null address, -1 a fault
template <class T>
int pread(uintptr_t a, T* out) {
    if (!a) return 0;
    __try {
        *out = *reinterpret_cast<const T*>(a);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// n bytes at a set to src when they differ (a plain store: the AO cache and flag, the rim statics, rimobj +0x2c)
int put_raw(uintptr_t a, const void* src, size_t n) {
    if (!a) return kPutMoved;
    __try {
        if (std::memcmp(reinterpret_cast<const void*>(a), src, n) == 0) return kPutSame;
        std::memcpy(reinterpret_cast<void*>(a), src, n);
        return kPutDone;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kPutFault;
    }
}
// a global CB's bytes [off, off + n) set to src the game's way (SetShaderGlobal 0x1490a0, the AO store 0x640ac7, 0x72b210's):
// the CB not yet mapped (+0x18 == 0), FUN_140ec8bd0(cb, -1, 0)'s body first (+0x18 = its storage +0x00, +0x10 = its layout's
// size, *(+8)+0x14, +0x24 = -1, +0x20 = 0: the dirty map the commit uploads), then the store at +0x18's buffer. storage != 0:
// the CB's storage must still be the snapshot's; a CB mapped to any other buffer than its storage is left alone
int cb_put(uintptr_t cb, uintptr_t storage, uint32_t off, const void* src, uint32_t n) {
    if (!cb) return kPutMoved;
    __try {
        const uintptr_t st = *reinterpret_cast<const uintptr_t*>(cb);
        if (!st || (storage && st != storage)) return kPutMoved;
        uintptr_t m = *reinterpret_cast<const uintptr_t*>(cb + 0x18);
        if (m && m != st) return kPutOther;
        if (std::memcmp(reinterpret_cast<const void*>(st + off), src, n) == 0) return kPutSame;
        if (!m) {  // FUN_140ec8bd0(cb, -1, 0)
            const uintptr_t lay = *reinterpret_cast<const uintptr_t*>(cb + 8);
            if (!lay) return kPutMoved;
            const int size = *reinterpret_cast<const int*>(lay + 0x14);
            *reinterpret_cast<uintptr_t*>(cb + 0x18) = st;
            *reinterpret_cast<int*>(cb + 0x10) = size;
            *reinterpret_cast<int*>(cb + 0x24) = -1;
            *reinterpret_cast<uint8_t*>(cb + 0x20) = 0;
            m = st;
        }
        std::memcpy(reinterpret_cast<void*>(m + off), src, n);
        return kPutDone;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kPutFault;
    }
}
// the world matrix through ViewportSetWorldMtx(ViewportCurrent, m): it copies the 3x3 and the translation (+0x00 and its copy
// at +0x80), then Regenerate re-derives the current viewport's WV/WVP and pushes its globals. Called only when one of the twelve
// floats it writes differs (bitwise)
using SetWorld_t = void (*)(void* vp, const float* m);
int put_world(uintptr_t fn, uintptr_t vpp, const uint8_t* src) {
    __try {
        const uintptr_t vp = *reinterpret_cast<const uintptr_t*>(vpp);
        if (!vp) return kPutMoved;
        alignas(16) float m[16];
        std::memcpy(m, src, sizeof(m));
        const int set[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
        bool same = true;
        for (int k = 0; k < 12 && same; ++k) same = std::memcmp(reinterpret_cast<const float*>(vp) + set[k], m + set[k], 4) == 0;
        if (same) return kPutSame;
        reinterpret_cast<SetWorld_t>(fn)(reinterpret_cast<void*>(vp), m);
        return kPutDone;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kPutFault;
    }
}
// rec's query slot for bucket b zeroed when non-zero (DrawVisEntity's mode-2 tail, 0x1406413bf-0x1406413d4: rdi = rec, r14d =
// the bucket from its home [rbp+0x128]): 1 zeroed, 0 already zero, -1 a fault
int qslot_zero(uintptr_t a) {
    __try {
        uint16_t* q = reinterpret_cast<uint16_t*>(a);
        if (!*q) return 0;
        *q = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// a variable's CB copy (its id's table entry), as the game stores it after a cache change: the gate clear, the id in range
int cb_copy(int id, const void* src, uint32_t n, uint64_t gate) {
    if (gate) return kPutGated;
    int cnt = 0, off = -1;
    uintptr_t cb = 0;
    const uintptr_t e = anchors::addr(Id::ShaderGlobalTable) + static_cast<uintptr_t>(id) * 16;
    if (pread(anchors::addr(Id::GlobalVarCount), &cnt) < 0) return kPutFault;
    if (id <= 0 || id > cnt || cnt >= kMaxVars) return kPutMoved;
    if (pread(e, &cb) < 0 || pread(e + 8, &off) < 0) return kPutFault;
    if (!cb || off < 0 || static_cast<uint32_t>(off) + n > kCbMax) return kPutMoved;
    return cb_put(cb, 0, static_cast<uint32_t>(off), src, n);
}
void tally(int f, int r) { bump(g_wb_res[f][r]); }

// (pass 1, at P_b, after its snapshot) bucket b's write-back list: of the write-back set's regions (the CBs, the AO cache and
// flag, the rim statics, the world), the elements pass 1's entities changed; the census's other fields changed are counted as
// left (route F leaves them; the cursor class and rimobj +0x2c aside); the textures CB is never written (its per-variable
// texture cache 0x142b07708 would no longer match), counted
void build_wb(int b) {
    WbList& w = g_wbl[b];
    w.n = 0;
    w.built = true;
    w.rim2c_d1 = false;
    const Snap* e = g_p1[b ? kE1 : kE0].taken ? &g_p1[b ? kE1 : kE0] : nullptr;
    const Snap& x = g_p1[b ? kP1 : kP0];
    if (!e) return;  // (no entity in pass 1's bucket: nothing changed; rimobj +0x2c is still reset at P_b)
    for (int i = 0; i < g_nreg; ++i) {
        const Region& r = g_reg[i];
        const bool wb = r.kind == kCb || i == g_ri_ao || i == g_ri_aoflag || i == g_ri_rimstatic || i == g_ri_world;
        if (!x.valid[i] || !e->valid[i]) {
            if (wb) bump(g_wb_missing[r.field]);
            continue;
        }
        if (i == g_ri_worldcopy || std::memcmp(x.blob + r.at, e->blob + r.at, r.len) == 0) continue;  // (the copy: the setter's)
        if (r.tex || (r.kind == kCb && r.addr == g_tab_tex)) {  // (the textures CB, as its own region or a variables' CB)
            bump(g_wb_tex_left);
            continue;
        }
        bool left = false;
        for (uint32_t k = r.e0; k < r.e0 + r.en; ++k) {
            const Elem& el = g_el[k];
            if (std::memcmp(x.blob + el.at, e->blob + el.at, el.len) == 0) continue;
            if (i == g_ri_rimobj && el.roff == 0x2c) {
                w.rim2c_d1 = true;
            } else if (!wb) {
                left = left || el.cls == kJudged;
            } else if (w.n < kMaxElems) {
                w.el[w.n++] = static_cast<uint16_t>(k);
            } else {
                bump(g_wb_over);
            }
        }
        if (left) bump(g_wb_left[r.field]);
    }
}

// (pass 2, at P_b, before the callback) pass 1's exit values of bucket b's list, then rimobj +0x2c, then the world
void writeback(int b) {
    bump(g_wb_calls[b]);
    const WbList& w = g_wbl[b];
    const Snap& x = g_p1[b ? kP1 : kP0];
    if (!w.built || !x.taken) {
        bump(g_wb_nosnap);
        return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uint64_t gate = 0;
    if (pread(g_base + kWriteGate, &gate) < 0) return probe_stand_down("an SEH fault reading the write gate 0x142ac09f0");
    uintptr_t rim = 0;
    if (pread(g_base + kRimObj, &rim) < 0) return probe_stand_down("an SEH fault reading the rim object 0x142ad1fc8");
    int world = -1;
    for (int j = 0; j < w.n; ++j) {
        const Elem& el = g_el[w.el[j]];
        const Region& r = g_reg[el.region];
        const uint8_t* src = x.blob + el.at;
        int f = kWCb, res = kPutMoved;
        if (el.region == g_ri_world) {
            world = w.el[j];
            continue;
        } else if (el.region == g_ri_ao) {  // the cache (8 bytes), then on a change its CB copy (AOModifier, *(i32*)0x1422e5aec)
            f = kWAo;
            res = put_raw(r.addr, src, 8);
            if (res == kPutDone) {
                int id = 0;
                const int rr = pread(g_base + kAoId, &id);
                const int c = rr < 0 ? kPutFault : cb_copy(id, src, 8, gate);
                tally(kWCopy, c);
                if (c == kPutFault) return probe_stand_down("an SEH fault writing the AO modifier's CB copy");
            }
        } else if (el.region == g_ri_aoflag) {
            f = kWAoFlag;
            res = put_raw(r.addr, src, 1);
        } else if (el.region == g_ri_rimstatic) {  // RimAmount (+0x00, id rim+0x30) or RimGlowColor (+0x10, id rim+0x34): 16 bytes
            f = kWRim;
            res = put_raw(r.addr + el.roff, src, 16);
            if (res == kPutDone) {
                int id = 0;
                const int rr = rim ? pread(rim + (el.roff ? 0x34 : 0x30), &id) : 0;
                const int c = rr < 0 ? kPutFault : rr == 0 || id == 0 ? kPutMoved : cb_copy(id, src, 16, gate);
                tally(kWCopy, c);
                if (c == kPutFault) return probe_stand_down("an SEH fault writing a rim static's CB copy");
            }
        } else if (r.kind == kCb) {
            // (the crash review's SF6) the region's CB is still the table's for its first variable: never a write into a reused block
            uintptr_t live = 0;
            const int lr = pread(anchors::addr(Id::ShaderGlobalTable) + static_cast<uintptr_t>(r.first_id) * 16, &live);
            if (lr < 0) return probe_stand_down("an SEH fault reading the shader-global table");
            res = gate ? kPutGated : live != r.addr ? kPutMoved : cb_put(r.addr, r.storage, el.roff, src, el.len);
        }
        tally(f, res);
        if (res == kPutFault) return probe_stand_down("an SEH fault in the write-back");
    }
    // rimobj +0x2c (0 or 2.0, never restored by DrawVisEntity): pass 1's value at P_b, D1 or not (the census's 1-4 frames a
    // config: pass 2 entered with 2.0 left over and its entities set 0)
    if (x.valid[g_ri_rimobj] && rim) {
        const int res = put_raw(rim + 0x2c, x.blob + g_reg[g_ri_rimobj].at + (0x2c - g_reg[g_ri_rimobj].off), 4);
        tally(kWRim2c, res);
        if (res == kPutDone && !w.rim2c_d1) bump(g_wb_rim2c_nod1);
        if (res == kPutFault) return probe_stand_down("an SEH fault writing rimobj +0x2c");
    } else {
        bump(g_wb_missing[kFRim]);
    }
    if (world >= 0) {  // last: Regenerate's derived values are pass 2's view's
        const int res = put_world(g_setworld, anchors::addr(Id::ViewportCurrent), x.blob + g_el[world].at);
        tally(kWWorld, res);
        if (res == kPutFault) return probe_stand_down("an SEH fault in ViewportSetWorldMtx's write-back");
    }
    QueryPerformanceCounter(&t1);
    bump(g_wb_ticks, static_cast<uint64_t>(t1.QuadPart - t0.QuadPart));
}

// (pass 1's DVL call 1 open) this frame's probe mode, refusals counted (g_fr.fa6 read first)
int probe_latch() {
    const int m = g_probe_flag.load(std::memory_order_relaxed);
    if (!m || g_pstood.load(std::memory_order_relaxed)) return 0;
    if (main_inst::armed()) return bump(g_pr_ref[kRefMain]), 0;
    if (p2skip::probe_on()) return bump(g_pr_ref[kRefP2skip]), 0;
    if (g_fr.fa6) return bump(g_pr_ref[kRefFa6]), 0;  // (the skeleton pre-draw's special mode: route F stands down while set)
    return m;
}
// (pass 2's open) skip this pass's e1 entities when its pass 1 latched the probe and is this frame's
void probe_open2(bool stale) {
    t_platched = t_pskip = t_pwb = t_pf = false;
    t_pskipped = t_pskb[0] = t_pskb[1] = 0;
    t_pq2 = t_pqz = t_pm1 = t_pmo = 0;
    const int m = g_fr.pmode;
    if (!m) return;
    bump(g_pr_calls);
    if (stale) return bump(g_pr_ref[kRefStale]);
    if (g_probe_flag.load(std::memory_order_relaxed) != m || g_pstood.load(std::memory_order_relaxed)) return bump(g_pr_ref[kRefChanged]);
    if (main_inst::armed()) return bump(g_pr_ref[kRefMain]);
    if (p2skip::probe_on()) return bump(g_pr_ref[kRefP2skip]);
    if (m == 2 && !g_fr.snaps) return bump(g_pr_ref[kRefStale]);
    uintptr_t ren = 0;  // (the crash review's SF5) renderer +0x1fa6 read again: route F stands down while it is set
    uint8_t fa6 = 0;
    if (rd(anchors::addr(Id::RendererSingleton), &ren) && ren && rd(ren + kFa6, &fa6) && fa6) return bump(g_pr_ref[kRefFa6]);
    bump(g_pr_p2);
    t_platched = t_pskip = true;
    t_pwb = m == 2;
}
void probe_close2() {
    if (!t_platched) return;
    t_platched = t_pskip = t_pwb = false;  // (the crash review's SF1: nothing skipped past the close, whatever in_e1 says)
    bump(g_pr_q2, t_pq2);  // (counted here, not per entity: no atomic in the skip itself)
    bump(g_pr_qzero, t_pqz);
    bump(g_pr_mode1, t_pm1);
    bump(g_pr_mode_other, t_pmo);
    if (!t_pf) bump(g_pr_nof);
    if (!t_pskipped) return bump(g_pr_none);
    bump(g_pr_skipped_frames);
    bump(g_pr_ent, t_pskipped);
    for (int b = 0; b < 2; ++b) bump(g_pr_ent_b[b], t_pskb[b]);
    cas_max(g_pr_ent_max, t_pskipped);
    cas_min(g_pr_ent_min, t_pskipped);
}
// a point out of order in pass 2: this pass stops skipping and writing back (counted); in pass 1: the frame is not latched
void probe_order_abort() {
    if (t_cpass == 1 && g_fr.pmode) {
        g_fr.pmode = 0;
        bump(g_pr_order);
    } else if (t_cpass == 2 && t_pskip) {
        t_pskip = t_pwb = false;
        bump(g_pr_order);
    }
}
// (pass 2, inside e1, an entity call of bucket 0 or 1 from DVL's loop) the skip: the query slot as mode 2's tail leaves it
bool probe_skip(uintptr_t r, uint32_t b, uint64_t a7) {
    const uint32_t mode = static_cast<uint32_t>(a7);  // (the 7th argument, [rsp+0x38] at entry: 32 bits)
    if (mode == 2) {
        ++t_pq2;
        const int z = qslot_zero(r + 12 + 16 * (b >> 1) + 2 * (b & 1));
        if (z < 0) {
            probe_stand_down("an SEH fault zeroing an entity's query slot (the call made)");
            return false;
        }
        if (z) ++t_pqz;
    } else if (mode == 1) {
        ++t_pm1;  // (mode 1 allocates and stores a query index for the next frame: not done, so the slot keeps pass 1's index
                  // (a query begun and ended this frame, eye 1's) or 0: next frame predicates on eye 1's query or not at all)
    } else {
        ++t_pmo;
    }
    ++t_pskipped;
    ++t_pskb[b];
    return true;
}

std::string probe_kv() {
    std::string out;
    char b[1024];
    auto add = [&](const char* fmt, auto... a) {
        std::snprintf(b, sizeof(b), fmt, a...);
        out += b;
    };
    const uint64_t sf = g_pr_skipped_frames.load(), p1f = g_p1_frames.load();
    add("probe=%d stood=%d census=%d setworld=%d calls=%llu p2_frames=%llu skipped_frames=%llu none_skipped=%llu no_f=%llu ent_skipped=%llu "
        "ent_per_frame=%.2f ent_max=%llu ent_min=%llu ent_b0=%llu ent_b1=%llu qslot_mode2=%llu qslot_zeroed=%llu mode1=%llu mode_other=%llu "
        "order_abort=%llu stand_downs=%llu",
        g_probe_flag.load(), g_pstood.load() ? 1 : 0, g_on_flag.load() ? 1 : 0, g_setworld ? 1 : 0, u(g_pr_calls), u(g_pr_p2), u(g_pr_skipped_frames),
        u(g_pr_none), u(g_pr_nof), u(g_pr_ent), sf ? static_cast<double>(g_pr_ent.load()) / static_cast<double>(sf) : 0.0, u(g_pr_ent_max),
        u(g_pr_ent_min), u(g_pr_ent_b[0]), u(g_pr_ent_b[1]), u(g_pr_q2), u(g_pr_qzero), u(g_pr_mode1), u(g_pr_mode_other), u(g_pr_order),
        u(g_pr_stand));
    for (int r = 0; r < kRefs; ++r) add(" refused_%s=%llu", kRefName[r], u(g_pr_ref[r]));
    const uint64_t wc = g_wb_calls[0].load() + g_wb_calls[1].load();
    add(" wb_calls_P0=%llu wb_calls_P1=%llu wb_nosnap=%llu wb_tex_left=%llu wb_over=%llu wb_rim2c_nod1=%llu wb_ms_per_frame=%.5f p1_snap_frames=%llu "
        "p1_snap_ms_per_frame=%.5f",
        u(g_wb_calls[0]), u(g_wb_calls[1]), u(g_wb_nosnap), u(g_wb_tex_left), u(g_wb_over), u(g_wb_rim2c_nod1),
        wc ? static_cast<double>(g_wb_ticks.load()) * g_qpc_ms / static_cast<double>(std::max<uint64_t>(1, g_wb_calls[0].load())) : 0.0, u(g_p1_frames),
        p1f ? static_cast<double>(g_p1_ticks.load()) * g_qpc_ms / static_cast<double>(p1f) : 0.0);
    for (int f = 0; f < kWbF; ++f)
        for (int r = 0; r < kPuts; ++r) add(" wb_%s_%s=%llu", kWbName[f], kPutName[r], u(g_wb_res[f][r]));
    for (int f = 0; f < kFields; ++f) add(" wb_missing_%s=%llu left_%s=%llu", kFieldName[f], u(g_wb_missing[f]), kFieldName[f], u(g_wb_left[f]));
    std::string why;
    {
        std::lock_guard lk(g_mx);
        why = g_pwhy;
    }
    for (char& c : why)
        if (c == ' ') c = '_';
    return out + " why=" + (why.empty() ? "none" : why) + " kv_end=1";
}
void probe_reset() {  // (stand_downs kept: it marks the session's stand-down)
    for (Count* c : {&g_pr_calls, &g_pr_p2, &g_pr_skipped_frames, &g_pr_none, &g_pr_nof, &g_pr_ent, &g_pr_ent_max, &g_pr_ent_min, &g_pr_ent_b[0],
                     &g_pr_ent_b[1], &g_pr_q2, &g_pr_qzero, &g_pr_mode1, &g_pr_mode_other, &g_pr_order, &g_wb_calls[0], &g_wb_calls[1], &g_wb_nosnap,
                     &g_wb_tex_left, &g_wb_over, &g_wb_rim2c_nod1, &g_wb_ticks, &g_p1_ticks, &g_p1_frames})
        c->store(0);
    for (Count& c : g_pr_ref) c.store(0);
    for (auto& f : g_wb_res)
        for (Count& c : f) c.store(0);
    for (Count& c : g_wb_missing) c.store(0);
    for (Count& c : g_wb_left) c.store(0);
}
std::string probe_command(const std::vector<std::string>& t) {
    if (t.size() == 3 || (t.size() == 4 && t[3] == "kv")) return probe_kv();
    if (t.size() != 4) return "ERROR usage: prepass fcensus probe [off|skip|skipwb|reset|kv]";
    const std::string& w = t[3];
    if (w == "reset") {
        probe_reset();
    } else if (w == "off") {
        g_probe_flag = 0;
        log::info("[fcensus] route F's probe off");
    } else if (w == "skip" || w == "skipwb") {
        if (g_pstood.load()) {
            std::lock_guard lk(g_mx);
            return std::string("ERROR stood down (") + g_pwhy + "): restart the game";
        }
        if (main_inst::armed()) return "ERROR refused: MainInstanced is armed (route F's probe never inside a main region)";
        if (p2skip::probe_on()) return "ERROR refused: the p2skip probe is on (\"prepass p2skip probe off\" first)";
        if (w == "skipwb" && !g_setworld) return "ERROR refused: ViewportSetWorldMtx's anchor is not there (the world's write-back)";
        g_probe_flag = w == "skip" ? 1 : 2;  // (from the next double frame's pass 1)
        log::info("[fcensus] route F's probe %s (test only; the simulator only: eye 2 has no e1 entities)", w.c_str());
    } else {
        return "ERROR usage: prepass fcensus probe [off|skip|skipwb|reset|kv]";
    }
    return probe_kv();
}

// ---- the points (render thread)
bool in_e1() { return t_cpass != 0 && t_e1 && t_depth == 1; }
bool post_caller(uintptr_t ret) {
    for (uintptr_t r : g_post_ret)
        if (ret == r) return true;
    return false;
}
void gate_read() {
    uint64_t g = 0;
    if (rd(g_base + kWriteGate, &g) && g) ++g_pa.gate_nz;
}

void open(int pass, void* list) {
    t_cpass = pass;
    t_e1 = true;
    t_list = reinterpret_cast<uintptr_t>(list);
    t_seen = 0;
    t_census = g_on_flag.load(std::memory_order_relaxed);
    std::memset(&g_pa, 0, sizeof(g_pa));
    const uint64_t key = dual_pass::double_seq();
    const uint32_t gen = g_gen.load(std::memory_order_relaxed);
    if (pass == 1) {
        if (g_fr.p1 && !g_fr.p2 && t_census) {  // (the last pass 1 never met its pass 2)
            std::lock_guard lk(g_mx);
            ++g_t.unpaired;
        }
        std::memset(&g_fr, 0, sizeof(g_fr));
        g_fr.p1 = true;
        g_fr.key = key;
        g_fr.gen = gen;
        ++g_frame_no;
        for (Snap& s : g_p1) s.taken = false;
        g_wbl[0].built = g_wbl[1].built = false;
        uintptr_t ren = 0;
        uint8_t fa6 = 0;
        g_fr.fa6 = rd(anchors::addr(Id::RendererSingleton), &ren) && ren && rd(ren + kFa6, &fa6) && fa6;
        g_fr.pmode = probe_latch();  // (route F's probe: this frame's mode)
        g_fr.snaps = t_census || g_fr.pmode == 2;
        if (g_fr.pmode == 2) bump(g_p1_frames);
        if (g_fr.snaps) build_cbs();
        if (t_census && !g_dev_logged) {  // (field 12: the grc context's vt+0xb0, once)
            uintptr_t ctx = 0, vt = 0, fn = 0;
            if (rd(anchors::addr(Id::GrcContext), &ctx) && ctx && rd(ctx, &vt) && vt && rd(vt + kDevSlot, &fn) && fn) {
                g_dev_logged = true;
                g_dev_b0.store(fn, std::memory_order_relaxed);
                log::info("[fcensus] field 12: the grc context %#llx's vt+0xb0 = %#llx (RDR.exe+%#llx)", static_cast<unsigned long long>(ctx),
                          static_cast<unsigned long long>(fn), static_cast<unsigned long long>(fn - g_base));
            }
        }
    } else {
        const bool other = !g_fr.p1 || g_fr.p2 || g_fr.key != key;  // (no pass 1, or another frame's)
        g_fr.stale = other || g_fr.gen != gen;
        g_fr.p2 = true;
        g_p2e[0].taken = g_p2e[1].taken = false;
        probe_open2(other);
    }
}

void fold_pass(PassAcc& t, const PassAcc& a) {
    const uint64_t* s = reinterpret_cast<const uint64_t*>(&a);
    uint64_t* d = reinterpret_cast<uint64_t*>(&t);
    for (size_t k = 0; k < sizeof(PassAcc) / 8; ++k) d[k] += s[k];
}

void close_call() {
    const int p = t_cpass;
    t_cpass = 0;
    t_e1 = false;
    if (p == 1) g_fr.p1_seen = t_seen;
    if (p == 2) probe_close2();
    if (!t_census) {  // (the census off at this call's open: nothing of its counted)
        if (p == 2) g_fr.p1 = false;
        return;
    }
    std::lock_guard lk(g_mx);
    if (g_fr.gen != g_gen.load(std::memory_order_relaxed)) {  // (a reset since this frame's pass 1: not counted)
        if (p == 2) g_fr.p1 = false;
        return;
    }
    fold_pass(g_t.pass[p - 1], g_pa);
    if (p != 2) return;
    g_fr.p1 = false;  // (consumed)
    if (g_fr.stale) {
        ++g_t.stale;
        return;
    }
    const uint32_t need = bit(kP0) | bit(kP1) | bit(kF);
    if ((g_fr.p1_seen & need) != need || g_fr.p2_eval != 7) {
        ++g_t.incomplete;
        return;
    }
    ++g_t.frames;
    if (!g_fr.fail_any) ++g_t.ok;
    if (!g_fr.sfail_any) ++g_t.sok;
    if (!g_fr.d2_any) ++g_t.d2none;
    if (!g_fr.d1_any) ++g_t.d1none;
    for (int c = 0; c < kCmp; ++c) {  // the cursor class (at F: both buckets' pass-2 pushes, what route F leaves at e1's end)
        const bool bad = c < 2 ? g_fr.cur_bad[c] : g_fr.cur_bad[0] || g_fr.cur_bad[1];
        if (c < 2 && g_fr.cfail[c]) ++g_t.cur_fail[c];
        if (bad) {
            ++g_t.cur_bad[c];
            continue;
        }
        const int64_t p2 = c < 2 ? g_fr.cur2[c] : g_fr.cur2[0] + g_fr.cur2[1];
        const int64_t p1 = c < 2 ? g_fr.cur1[c] : g_fr.cur1[0] + g_fr.cur1[1];
        ++g_t.cur_n[c];
        g_t.cur2_sum[c] += static_cast<uint64_t>(p2);
        g_t.cur1_sum[c] += static_cast<uint64_t>(p1);
        g_t.cur2_max[c] = std::max(g_t.cur2_max[c], static_cast<uint64_t>(p2));
        if (p2) ++g_t.cur2_nz[c];
    }
    if (g_fr.bmis) ++g_t.bmis;
    if (g_fr.ent[0][0] != g_fr.ent[1][0] || g_fr.ent[0][1] != g_fr.ent[1][1]) ++g_t.ent_mis;
    if (g_fr.fa6) ++g_t.fa6;
    for (int f = 0; f < kFields; ++f) {
        for (int c = 0; c < kCmp; ++c) {
            g_t.ffail[f][c] += g_fr.ffail[f][c];
            g_t.sffail[f][c] += g_fr.sffail[f][c];
            g_t.unread[f][c] += g_fr.unread[f][c];
        }
        for (int b = 0; b < 2; ++b) {
            g_t.d1f[f][b] += g_fr.d1f[f][b];
            g_t.d2f[f][b] += g_fr.d2f[f][b];
            g_t.d1only[f][b] += g_fr.d1only[f][b];
            g_t.d2only[f][b] += g_fr.d2only[f][b];
        }
    }
    for (int w = 0; w < kMaxElems / 64; ++w)
        for (uint64_t m = g_fr.failbits[w]; m; m &= m - 1) {
            unsigned long low = 0;
            _BitScanForward64(&low, m);
            const int e = w * 64 + static_cast<int>(low);
            if (e < g_nel && g_el[e].key >= 0 && g_el[e].key < kKeys) ++g_t.fail_key[g_el[e].key];
        }
    for (int k = 0; k < g_fr.ncand && g_t.nfirst < 16; ++k) g_t.first[g_t.nfirst++] = g_fr.cand[k];
}

void at_post(int pt, uintptr_t ret) {
    if (!in_e1()) return;
    if (!post_caller(ret)) {
        if (t_census) ++g_pa.foreign;
        return;
    }
    if (t_seen & bit(pt)) return;
    const bool order_ok = pt == kP0 ? !(t_seen & (bit(kE1) | bit(kP1) | bit(kF))) : (t_seen & bit(kP0)) && !(t_seen & bit(kF));
    t_seen |= bit(pt);
    if (!order_ok) {
        if (t_census) ++g_pa.order;
        probe_order_abort();
        return;
    }
    if (main_region::armed()) main_region::segment(pt == kP0 ? 2 : 4);  // ([Stereo] MainRegion, item 8d: the post's segment starts here)
    const int b = pt == kP0 ? 0 : 1;
    if (!t_census) {  // (route F's probe only: pass 1's snapshot and write-back list, pass 2's write-back)
        if (t_cpass == 1 && g_fr.pmode == 2) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            take(g_p1[pt]);
            build_wb(b);
            QueryPerformanceCounter(&t1);
            bump(g_p1_ticks, static_cast<uint64_t>(t1.QuadPart - t0.QuadPart));
        } else if (t_cpass == 2 && t_pwb && !g_pstood.load(std::memory_order_relaxed)) {
            writeback(b);
        }
        return;
    }
    ++g_pa.hits[pt];
    // #5: list+0x10 and render state 0xc both hold DVL's reset value; #11 at P0: the stub's 0x142ac1d47 = 0, 0x142272488 = -1
    uint32_t lb = 0, base = 0, rc = 0;
    int ovr = 0;
    uint8_t ob = 0;
    if (rd(t_list + 0x10, &lb) && rd(g_base + kBiasBase, &base) && rd(g_base + kRsShadow + 0xc * 4, &rc)) {
        ++g_pa.a5_n;
        if (lb != base || rc != base) ++g_pa.a5_fail;
    }
    if (rd(g_base + kOvr, &ovr) && rd(g_base + kOvrByte, &ob)) {
        if (pt == kP0) {
            ++g_pa.a11_n;
            if (ovr != -1 || ob != 0) ++g_pa.a11_fail;
        } else if (ovr != -1) {
            ++g_pa.ovr_p1;
        }
    }
    gate_read();
    if (t_cpass == 1) {
        take(g_p1[pt]);
        if (g_fr.pmode == 2) build_wb(b);
    } else {
        if (t_pwb && !g_pstood.load(std::memory_order_relaxed)) writeback(b);  // (before the census's snapshot: it judges what the probe leaves)
        if (!g_fr.stale) {
            take(g_cur);
            if (g_p1[pt].taken) {
                judge_bucket(b);
                g_fr.p2_eval |= pt == kP0 ? 1u : 2u;
            }
        }
    }
}

using Post_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
Post_t o_post0 = nullptr, o_post1 = nullptr;
// bucket 0's post body 0x6ddbe0 (rcx = *0x142ad1ce0) and bucket 1's post 0x5c1970 (rcx, rdx): every register argument forwarded
uintptr_t hk_post0(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    if (on()) at_post(kP0, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return o_post0(a, b, c, d);
}
uintptr_t hk_post1(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    if (on()) at_post(kP1, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return o_post1(a, b, c, d);
}

// (the leaf-hook rule) the target is a .pdata function starting there
bool has_pdata(uintptr_t f) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(static_cast<DWORD64>(f), &image, nullptr);
    return fn && image && image + fn->BeginAddress == f;
}
// a rel32 call or jmp (op) at site whose target is to
bool rel32_to(uintptr_t site, uint8_t op, uintptr_t to) {
    uint8_t c = 0;
    int32_t rel = 0;
    return rd(site, &c) && c == op && rd(site + 1, &rel) && site + 5 + static_cast<intptr_t>(rel) == to;
}

// ---- the reports (under g_mx)
void reset_totals() {
    std::memset(&g_t, 0, sizeof(g_t));
    g_lay.elem_over = g_lay.bad = 0;
    g_gen.fetch_add(1);
    g_window_ms = log::now_ms();
}
uint64_t hits(int p, int pt) { return g_t.pass[p].hits[pt]; }
double pct(uint64_t a, uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }
// the keys with the most failing frames, at most n
int top_keys(int* out, int n) {
    int m = 0;
    for (int k = 0; k < kKeys; ++k) {
        if (!g_t.fail_key[k]) continue;
        if (m < n) out[m++] = k;
        else if (g_t.fail_key[k] > g_t.fail_key[out[n - 1]]) out[n - 1] = k;
        else continue;
        for (int j = m - 1; j > 0 && g_t.fail_key[out[j - 1]] < g_t.fail_key[out[j]]; --j) std::swap(out[j - 1], out[j]);
    }
    return m;
}
std::string fail_text(const Fail& f, char sep) {
    char b[300];
    std::snprintf(b, sizeof(b), "%llu%c%s%c%s%c%s%c%u%c%u%c%016llx%c%016llx%c%016llx%c%016llx", static_cast<unsigned long long>(f.frame), sep,
                  kCmpName[f.point], sep, kFieldName[f.field], sep, f.name, sep, f.len, sep, static_cast<unsigned>(f.has), sep,
                  static_cast<unsigned long long>(f.v[0]), sep, static_cast<unsigned long long>(f.v[1]), sep,
                  static_cast<unsigned long long>(f.v[2]), sep, static_cast<unsigned long long>(f.v[3]));
    return b;
}

std::string kv() {
    std::lock_guard lk(g_mx);
    std::string out;
    char b[512];
    auto add = [&](const char* fmt, auto... a) {
        std::snprintf(b, sizeof(b), fmt, a...);
        out += b;
    };
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    add("on=%d window_s=%.1f frames=%llu ok=%llu ok_pct=%.4f strict_ok=%llu strict_ok_pct=%.4f d2none=%llu d2none_pct=%.4f d1none=%llu incomplete=%llu "
        "stale=%llu unpaired=%llu bucket_mismatch=%llu ent_mismatch=%llu fa6_frames=%llu thread=%llu",
        g_on_flag.load() ? 1 : 0, (log::now_ms() - g_window_ms) / 1000.0, u(g_t.frames), u(g_t.ok), pct(g_t.ok, g_t.frames), u(g_t.sok), pct(g_t.sok, g_t.frames),
        u(g_t.d2none), pct(g_t.d2none, g_t.frames), u(g_t.d1none), u(g_t.incomplete), u(g_t.stale), u(g_t.unpaired), u(g_t.bmis), u(g_t.ent_mis), u(g_t.fa6),
        u(g_t.thread));
    for (int c = 0; c < kCmp; ++c)
        add(" cur_n_%s=%llu cur2_sum_%s=%llu cur2_max_%s=%llu cur2_nz_%s=%llu cur1_sum_%s=%llu cur_bad_%s=%llu cur_fail_%s=%llu", kCmpName[c], u(g_t.cur_n[c]),
            kCmpName[c], u(g_t.cur2_sum[c]), kCmpName[c], u(g_t.cur2_max[c]), kCmpName[c], u(g_t.cur2_nz[c]), kCmpName[c], u(g_t.cur1_sum[c]), kCmpName[c],
            u(g_t.cur_bad[c]), kCmpName[c], u(g_t.cur_fail[c]));
    for (int p = 0; p < 2; ++p)
        for (int pt = 0; pt < kPoints; ++pt) add(" hit_p%d_%s=%llu", p + 1, kPointName[pt], u(hits(p, pt)));
    for (int f = 0; f < kFields; ++f) {
        for (int c = 0; c < kCmp; ++c)
            add(" fail_%s_%s=%llu sfail_%s_%s=%llu unread_%s_%s=%llu", kFieldName[f], kCmpName[c], u(g_t.ffail[f][c]), kFieldName[f], kCmpName[c],
                u(g_t.sffail[f][c]), kFieldName[f], kCmpName[c], u(g_t.unread[f][c]));
        add(" d1_%s_b0=%llu d1_%s_b1=%llu d2_%s_b0=%llu d2_%s_b1=%llu", kFieldName[f], u(g_t.d1f[f][0]), kFieldName[f], u(g_t.d1f[f][1]), kFieldName[f],
            u(g_t.d2f[f][0]), kFieldName[f], u(g_t.d2f[f][1]));
        add(" d1only_%s_b0=%llu d1only_%s_b1=%llu d2only_%s_b0=%llu d2only_%s_b1=%llu", kFieldName[f], u(g_t.d1only[f][0]), kFieldName[f],
            u(g_t.d1only[f][1]), kFieldName[f], u(g_t.d2only[f][0]), kFieldName[f], u(g_t.d2only[f][1]));
    }
    const PassAcc& a = g_t.pass[0];
    const PassAcc& c = g_t.pass[1];
    add(" a5_n=%llu a5_fail=%llu a11_n=%llu a11_fail=%llu ovr_p1_not_m1=%llu gate_nz_p1=%llu gate_nz_p2=%llu ent_p1=%llu ent_p2=%llu stamp_n=%llu "
        "stamp_diff=%llu qslot_nonzero=%llu qslot_mode2=%llu cb_moved=%llu order=%llu foreign_post=%llu",
        u(a.a5_n + c.a5_n), u(a.a5_fail + c.a5_fail), u(a.a11_n + c.a11_n), u(a.a11_fail + c.a11_fail), u(a.ovr_p1 + c.ovr_p1), u(a.gate_nz), u(c.gate_nz),
        u(a.ent), u(c.ent), u(c.stamp_n), u(c.stamp_diff), u(c.qslot), u(c.qslot2), u(a.cb_moved + c.cb_moved), u(a.order + c.order),
        u(a.foreign + c.foreign));
    add(" vars=%d cbs=%d cbs_derived=%d cbs_skipped=%d cb_bytes=%u elems=%d elem_over=%llu layout_bad=%llu rebuilds=%d dev_b0=%#llx", g_lay.vars, g_lay.cbs,
        g_lay.derived, g_lay.skipped, g_lay.bytes, g_lay.elems, u(g_lay.elem_over), u(g_lay.bad), g_lay.rebuilds,
        u(g_dev_b0.load() ? g_dev_b0.load() - g_base : 0));
    int top[24];
    const int nt = top_keys(top, 24);
    for (int i = 0; i < nt; ++i) {
        char nm[48];
        key_name(top[i], g_known_cmd, g_nknown_cmd, nm, sizeof(nm));
        add(" top%d=%s:%llu", i, nm, u(g_t.fail_key[top[i]]));
    }
    bool cut = false;  // (the test channel's reply holds 12 KB: the first failures cut short past 10.5 KB, flagged)
    for (int i = 0; i < g_t.nfirst; ++i) {
        const std::string f = " first" + std::to_string(i) + "=" + fail_text(g_t.first[i], ':');
        if (out.size() + f.size() > 10500) {
            cut = true;
            break;
        }
        out += f;
    }
    return out + (cut ? " kv_truncated=1" : "") + " kv_end=1";
}

std::string status() {
    std::lock_guard lk(g_mx);
    char b[2048];
    int n = std::snprintf(b, sizeof(b),
                          "prepass fcensus %s: frames %llu over %.1f s, equal at P0, P1 and F %llu (%.3f%%; strict %llu, %.3f%%), pass 2's entities "
                          "changed nothing %llu (%.3f%%) | not judged: stale %llu, incomplete %llu, unpaired pass 1 %llu | bucket mismatch %llu, entity "
                          "count mismatch %llu, +0x1fa6 set %llu | points p1/p2:",
                          g_on_flag.load() ? "on" : "off", static_cast<unsigned long long>(g_t.frames), (log::now_ms() - g_window_ms) / 1000.0,
                          static_cast<unsigned long long>(g_t.ok), pct(g_t.ok, g_t.frames), static_cast<unsigned long long>(g_t.sok),
                          pct(g_t.sok, g_t.frames), static_cast<unsigned long long>(g_t.d2none),
                          pct(g_t.d2none, g_t.frames), static_cast<unsigned long long>(g_t.stale), static_cast<unsigned long long>(g_t.incomplete),
                          static_cast<unsigned long long>(g_t.unpaired), static_cast<unsigned long long>(g_t.bmis),
                          static_cast<unsigned long long>(g_t.ent_mis), static_cast<unsigned long long>(g_t.fa6));
    for (int pt = 0; pt < kPoints && n > 0 && n < static_cast<int>(sizeof(b)); ++pt)
        n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), " %s %llu/%llu", kPointName[pt], static_cast<unsigned long long>(hits(0, pt)),
                           static_cast<unsigned long long>(hits(1, pt)));
    for (int c = 0; c < kCmp && n > 0 && n < static_cast<int>(sizeof(b)); ++c)
        n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), "%s %s pass 2 pushed %llu (max %llu) in %llu of %llu, pass 1 %llu, unmeasured %llu",
                           c ? "," : " | cursors (not judged):", kCmpName[c], static_cast<unsigned long long>(g_t.cur2_sum[c]),
                           static_cast<unsigned long long>(g_t.cur2_max[c]), static_cast<unsigned long long>(g_t.cur2_nz[c]),
                           static_cast<unsigned long long>(g_t.cur_n[c]), static_cast<unsigned long long>(g_t.cur1_sum[c]),
                           static_cast<unsigned long long>(g_t.cur_bad[c]));
    if (n > 0 && n < static_cast<int>(sizeof(b))) n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), " | failing frames by field (P0/P1/F):");
    for (int f = 0; f < kFields && n > 0 && n < static_cast<int>(sizeof(b)); ++f)
        if (g_t.ffail[f][0] || g_t.ffail[f][1] || g_t.ffail[f][2])
            n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), " %s %llu/%llu/%llu", kFieldName[f], static_cast<unsigned long long>(g_t.ffail[f][0]),
                               static_cast<unsigned long long>(g_t.ffail[f][1]), static_cast<unsigned long long>(g_t.ffail[f][2]));
    const PassAcc& a = g_t.pass[0];
    const PassAcc& c = g_t.pass[1];
    if (n > 0 && n < static_cast<int>(sizeof(b)))
        std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n),
                      " | asserts: #5 %llu of %llu failed, #11 %llu of %llu failed, stamps differing in pass 2 %llu of %llu, query slots set in pass 2 %llu "
                      "(mode 2 %llu), write gate set p1 %llu p2 %llu | CBs %d (%u B, %d derived, %d skipped), %d elements",
                      static_cast<unsigned long long>(a.a5_fail + c.a5_fail), static_cast<unsigned long long>(a.a5_n + c.a5_n),
                      static_cast<unsigned long long>(a.a11_fail + c.a11_fail), static_cast<unsigned long long>(a.a11_n + c.a11_n),
                      static_cast<unsigned long long>(c.stamp_diff), static_cast<unsigned long long>(c.stamp_n), static_cast<unsigned long long>(c.qslot),
                      static_cast<unsigned long long>(c.qslot2), static_cast<unsigned long long>(a.gate_nz), static_cast<unsigned long long>(c.gate_nz),
                      g_lay.cbs, g_lay.bytes, g_lay.derived, g_lay.skipped, g_lay.elems);
    return b;
}

std::string dump() {
    std::string out = status();
    std::lock_guard lk(g_mx);
    out += "\nfirst failures (frame point field element len has pass1-E pass1-X pass2-E pass2-X; at F: X only):";
    for (int i = 0; i < g_t.nfirst; ++i) out += "\n  " + fail_text(g_t.first[i], ' ');
    char b[160];
    out += "\nby field: failing frames L rule P0/P1/F, strict P0/P1/F; frames with an element D1 and not D2 b0/b1, not D1 and D2 b0/b1:";
    for (int f = 0; f < kFields; ++f) {
        std::snprintf(b, sizeof(b), "\n  %-6s %llu/%llu/%llu  %llu/%llu/%llu  d1only %llu/%llu  d2only %llu/%llu", kFieldName[f],
                      static_cast<unsigned long long>(g_t.ffail[f][0]), static_cast<unsigned long long>(g_t.ffail[f][1]),
                      static_cast<unsigned long long>(g_t.ffail[f][2]), static_cast<unsigned long long>(g_t.sffail[f][0]),
                      static_cast<unsigned long long>(g_t.sffail[f][1]), static_cast<unsigned long long>(g_t.sffail[f][2]),
                      static_cast<unsigned long long>(g_t.d1only[f][0]), static_cast<unsigned long long>(g_t.d1only[f][1]),
                      static_cast<unsigned long long>(g_t.d2only[f][0]), static_cast<unsigned long long>(g_t.d2only[f][1]));
        out += b;
    }
    std::snprintf(b, sizeof(b), "\ncursor class's L rule failing (info, not judged): P0 %llu, P1 %llu", static_cast<unsigned long long>(g_t.cur_fail[0]),
                  static_cast<unsigned long long>(g_t.cur_fail[1]));
    out += b;
    out += "\nfailing frames by element:";
    int top[64];
    const int nt = top_keys(top, 64);
    for (int i = 0; i < nt; ++i) {
        char nm[48];
        key_name(top[i], g_known_cmd, g_nknown_cmd, nm, sizeof(nm));
        std::snprintf(b, sizeof(b), "\n  %s %llu", nm, static_cast<unsigned long long>(g_t.fail_key[top[i]]));
        out += b;
    }
    out += "\nknown variable ids:";
    for (int i = 0; i < g_nknown_cmd; ++i) {
        std::snprintf(b, sizeof(b), " %s=%d", g_known_cmd[i].name, g_known_cmd[i].id);
        out += b;
    }
    return out;
}

}  // namespace

void install() {
    const int want = config::get_int("Stereo", "RouteFCensus", 0);
    if (want <= 0) return;
    if (want != 1) {
        log::warn("[fcensus] RouteFCensus=%d is not 1: off", want);
        return;
    }
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated()) {
        log::warn("[fcensus] RouteFCensus needs the analysed RDR.exe build (its fields' addresses are that build's): off");
        return;
    }
    for (Id id : {Id::DvlPostB0Body, Id::DvlPostB1, Id::MainDvlCall, Id::DrawVisList, Id::DrawVisEntity, Id::ShaderGlobalTable, Id::GlobalVarCount,
                  Id::GlobalTexturesCb, Id::RendererSingleton, Id::GrcContext})
        if (!anchors::present(id)) {
            log::warn("[fcensus] the anchor %s is absent on this build: off", anchors::name(id));
            return;
        }
    g_base = anchors::base();
    const char* why = nullptr;
    const uintptr_t site = anchors::addr(Id::MainDvlCall);
    if (!rel32_to(site, 0xe8, anchors::addr(Id::DrawVisList))) why = "MainDvlCall is not a call of DrawVisList";
    else if (!rel32_to(g_base + kDveCall, 0xe8, anchors::addr(Id::DrawVisEntity))) why = "DVL's entity loop does not call DrawVisEntity at 0x1406421a9";
    else if (!rel32_to(g_base + kB0Jmp, 0xe9, anchors::addr(Id::DvlPostB0Body))) why = "the stub 0x1405c1950 does not jump to 0x1406ddbe0";
    for (int k = 0; k < 6 && !why; ++k) {
        uint8_t c[3] = {};
        const uint8_t call38[3] = {0xff, 0x53, 0x38};  // call [rbx+0x38]
        g_post_ret[k] = g_base + kPostRet[k];
        if (!raw(g_post_ret[k] - 3, c, 3) || std::memcmp(c, call38, 3) != 0) why = "DVL's post-callback calls are not call [rbx+0x38]";
    }
    if (!why && (!has_pdata(anchors::addr(Id::DvlPostB0Body)) || !has_pdata(anchors::addr(Id::DvlPostB1))))
        why = "a post callback has no .pdata entry starting there (a leaf hook would need a stub)";
    if (!why && !build_fixed()) why = "the fixed fields do not fit their blob";
    if (why) {
        log::warn("[fcensus] %s: off", why);
        return;
    }
    g_dvl_ret = site + 5;
    g_dve_ret = g_base + kDveCall + 5;
    if (!hooks::install("RDR bucket 0's post body (FUN_1406ddbe0, RouteFCensus)", reinterpret_cast<void*>(anchors::addr(Id::DvlPostB0Body)), &hk_post0,
                        &o_post0) ||
        !hooks::install("RDR bucket 1's post (FUN_1405c1970, RouteFCensus)", reinterpret_cast<void*>(anchors::addr(Id::DvlPostB1)), &hk_post1, &o_post1)) {
        log::warn("[fcensus] a post callback's hook not installed: off (an installed one stays inert)");
        return;
    }
    {
        std::lock_guard lk(g_mx);
        reset_totals();
    }
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    g_qpc_ms = qf.QuadPart ? 1000.0 / static_cast<double>(qf.QuadPart) : 0.0;
    // route F's probe's world write-back (skipwb): ViewportSetWorldMtx, its anchor's signature checked, and ViewportCurrent
    if (anchors::present(Id::ViewportSetWorldMtx) && anchors::present(Id::ViewportCurrent)) g_setworld = anchors::addr(Id::ViewportSetWorldMtx);
    else log::warn("[fcensus] ViewportSetWorldMtx or ViewportCurrent absent: route F's probe \"skipwb\" refused (\"skip\" available)");
    g_installed = true;
    g_on_flag = true;
    log::info("[fcensus] [Stereo] RouteFCensus on (test only, read only): pass 1's CPU caches against pass 2's at bucket 0's and 1's first entity "
              "and post entry and at ForestDraw mode 0 in DVL call 1, \"prepass fcensus\" (%d fixed regions, %d elements); route F's probe off "
              "(\"prepass fcensus probe skip|skipwb\")",
              g_nfixed, g_nfixed_el);
}

uintptr_t dvl_ret() { return g_dvl_ret; }
bool installed() { return g_installed.load(std::memory_order_relaxed); }
void set_segments(bool on) { g_seg_flag.store(on && g_installed.load(std::memory_order_relaxed), std::memory_order_relaxed); }

void dvl_enter(int pass, bool call1, void* list) {
    ++t_depth;
    if (!call1 || t_depth != 1 || (pass != 1 && pass != 2) || !g_installed.load(std::memory_order_relaxed)) return;
    const DWORD tid = GetCurrentThreadId();
    DWORD o = 0;
    if (!g_owner.compare_exchange_strong(o, tid) && o != tid) {  // (one render thread: the snapshots are its own)
        std::lock_guard lk(g_mx);
        ++g_t.thread;
        return;
    }
    open(pass, list);
}

void dvl_leave(int pass, bool call1) {
    if (call1 && t_depth == 1 && t_cpass != 0 && t_cpass == pass) close_call();
    if (t_depth > 0) --t_depth;
}

namespace {
void census_entity(uintptr_t r, uint32_t bucket, uint64_t a7, int p) {
    ++g_pa.ent;
    if (bucket <= 1) ++g_fr.ent[p - 1][bucket];
    if (p == 2) {
        // the grmMatrixSet stamp (rec[0x12] +0xd0; pass 1 stamped it this frame) and the query slot (mode 2 zeroes it)
        uintptr_t mset = 0;
        int st = 0, fc = 0;
        if (rd(r + 0x12 * 8, &mset) && mset && rd(mset + 0xd0, &st) && rd(g_base + kFrameCounter, &fc)) {
            ++g_pa.stamp_n;
            if (st != fc) ++g_pa.stamp_diff;
        }
        uint16_t q = 0;
        if (bucket < 32 && rd(r + 12 + 16 * (bucket >> 1) + 2 * (bucket & 1), &q) && q) {
            ++g_pa.qslot;
            if (static_cast<uint32_t>(a7) == 2) ++g_pa.qslot2;
        }
    }
}

// E_b: bucket b's first entity call (the census's snapshots; pass 1's also the probe's write-back's E)
void entity_point(uint32_t bucket, int p) {
    const int pt = bucket == 0 ? kE0 : kE1;
    if (t_seen & bit(pt)) return;
    const bool order_ok = bucket == 0 ? !(t_seen & (bit(kP0) | bit(kE1) | bit(kP1) | bit(kF))) : (t_seen & bit(kP0)) && !(t_seen & (bit(kP1) | bit(kF)));
    t_seen |= bit(pt);
    if (!order_ok) {
        if (t_census) ++g_pa.order;
        probe_order_abort();
        return;
    }
    if (main_region::armed()) main_region::segment(bucket == 0 ? 1 : 3);  // ([Stereo] MainRegion, item 8d: the bucket's entities start here)
    if (t_census) {
        ++g_pa.hits[pt];
        if (pt == kE1) {  // #11: bucket 1's pre wrote 0x142272488 = -1
            int ovr = 0;
            if (rd(g_base + kOvr, &ovr)) {
                ++g_pa.a11_n;
                if (ovr != -1) ++g_pa.a11_fail;
            }
        }
        gate_read();
    }
    if (p == 1) {
        if (!g_fr.snaps) return;
        if (t_census) return take(g_p1[pt]);
        LARGE_INTEGER t0, t1;  // (the probe's own snapshot: its cost)
        QueryPerformanceCounter(&t0);
        take(g_p1[pt]);
        QueryPerformanceCounter(&t1);
        bump(g_p1_ticks, static_cast<uint64_t>(t1.QuadPart - t0.QuadPart));
    } else if (t_census && !g_fr.stale) {
        take(g_p2e[bucket]);
    }
}
}  // namespace

bool entity(void* const* rec, uint64_t bucket_arg, uint64_t a7, uintptr_t ret) {
    if (!in_e1() || ret != g_dve_ret || !rec) return false;
    const uint32_t bucket = static_cast<uint32_t>(bucket_arg);  // (the game's argument is 32 bits: r9's upper half is not its)
    const int p = t_cpass;
    const uintptr_t r = reinterpret_cast<uintptr_t>(rec);
    if (t_census) census_entity(r, bucket, a7, p);
    if (bucket <= 1) entity_point(bucket, p);
    // ([Stereo] MainRegion's tier 2, item 8d, test only; off: one relaxed load) the entity's marker, after its bucket's segment marker
    if (bucket <= 1 && main_region::why_on()) main_region::entity(r, bucket, p);
    // route F's probe: pass 2's e1 entities of buckets 0 and 1 not drawn (the points above already taken: E2 is the state DVL
    // left for the bucket's first entity)
    if (p != 2 || bucket > 1 || !t_pskip || g_pstood.load(std::memory_order_relaxed)) return false;
    return probe_skip(r, bucket, a7);
}

void forest_e1() {
    if (!in_e1()) return;
    t_e1 = false;  // (e1 ends here: no later point of this call counts)
    t_seen |= bit(kF);
    if (t_cpass == 2) t_pf = true;  // (route F's probe: e1's end met)
    if (!t_census) return;
    ++g_pa.hits[kF];
    gate_read();
    if (t_cpass == 1) {
        take(g_p1[kF]);
    } else if (!g_fr.stale) {
        take(g_cur);
        if (g_p1[kF].taken && (g_fr.p2_eval & 2u)) {
            judge_f();
            g_fr.p2_eval |= 4u;
        }
    }
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load()) return "prepass fcensus: not installed (start with [Stereo] RouteFCensus=1 on the analysed RDR.exe; test only)";
    if (t.size() >= 3 && t[2] == "probe") return probe_command(t);  // (route F's probe, item 8h' step 2; test only)
    if (t.size() > 3) return "ERROR usage: prepass fcensus [on|off|reset|dump|kv|probe off|skip|skipwb|reset|kv]";
    if (t.size() == 3) {
        const std::string& w = t[2];
        if (w == "kv") return kv();
        if (w == "dump") {
            const std::string d = dump();
            log::info("[fcensus] dump:\n%s", d.c_str());
            return d;
        }
        if (w == "on" || w == "off") {
            g_on_flag = w == "on";
        } else if (w == "reset") {
            std::lock_guard lk(g_mx);
            reset_totals();
        } else {
            return "ERROR usage: prepass fcensus [on|off|reset|dump|kv|probe off|skip|skipwb|reset|kv]";
        }
    }
    return status();
}

}  // namespace rdrvr::fcensus
