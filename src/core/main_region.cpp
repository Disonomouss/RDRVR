#include "core/main_region.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/fcensus.h"
#include "core/inst_prev_mirror.h"
#include "core/log.h"
#include "core/main_instanced.h"
#include "core/pass2_skip.h"
#include "core/prepass_arrays.h"
#include "core/prevworld_mirror.h"
#include "core/pso.h"
#include "core/state.h"

namespace rdrvr::main_region {
std::atomic<bool> g_armed{false}, g_observing{false}, g_why_armed{false};
std::atomic<bool> g_comparing{false};  // (item 8e) installed and "compare"
namespace {
using Count = std::atomic<uint64_t>;
// one writer a counter (the render thread's, or the playback thread's): load + store; the commands read them
inline void bump(Count& c, uint64_t by = 1) { c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed); }
inline unsigned long long u(const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); }
namespace rg = pso::region;

std::atomic<bool> g_installed{false};
std::atomic<int> g_mode{0};             // 0 off, 1 observe, 2 compare (item 8e: observe plus main_inst's listing, MainFamily=region)
std::atomic<int> g_control{0};          // 0 none, 1 nopw (M2's control: every prevworld twin read as "gate prevworld")
std::atomic<bool> g_reset_req{false};   // "prepass mr reset" while observing: done by the playback thread at its next pass 1 open
bool g_segs = false;                    // fcensus installed: the segment markers come (read-only after install)

// ---- the frame's gates: failed ones are bits (the render thread writes them into the ring before the marker that reads them)
enum Gate : uint32_t {
    kGCascades = 1u << 0, kGRows = 1u << 1, kGUcull = 1u << 2, kGP1vp = 1u << 3, kGControl = 1u << 4, kGAllow = 1u << 5,
    kGAutoE2i = 1u << 6, kGAutoPw = 1u << 7, kGPwLive = 1u << 8, kGIp = 1u << 9, kGFade = 1u << 10, kGCut = 1u << 11
};
constexpr int kGates = 12;
const char* const kGateName[kGates] = {"cascades", "rows", "ucull", "p1vp", "control", "allow", "auto_e2i", "auto_pw", "pw_live", "ip_live",
                                       "fade", "cut"};
// why a frame is not judged (bits)
enum Unj : uint32_t { kUNoE1 = 1, kUOrder = 2, kUOther = 4, kUOver = 8, kUNoP2 = 16, kUMarker = 32 };
constexpr int kUnjs = 6;
const char* const kUnjName[kUnjs] = {"no_e1", "order", "other_thread", "over", "no_p2", "marker"};
// the segments (the playback's cursor, set by the render thread's markers at fcensus's points)
constexpr int kSegs = 5;
const char* const kSegName[kSegs] = {"pre", "b0ent", "b0post", "b1ent", "b1post"};
// a draw's verdict class and its sub-reason (section 3.1's table)
enum Cat : int { kCTwin, kCPso, kCBind, kCNoTwin, kCKind, kCE2i, kCPw, kCIp, kCIndirect, kCats };
const char* const kCatName[kCats] = {"twin", "pso", "bind", "notwin", "kind", "e2i", "pw", "ip", "indirect"};
enum Reason : int {
    kRPsoList, kRPsoSlot, kRBind,
    kRNtStream, kRNtQueued, kRNtReady, kRNtVsRefused, kRNtPsRefused, kRNtHeldWvp, kRNtHeldIp, kRNtHeldE2i, kRNtHeldPw, kRNtHeld,
    kRNtBuildFailed, kRNtShape, kRNtNoRule,
    kRKindWvp,
    kRE2iNoKind, kRE2iNoLive, kRE2iNoGuard, kRE2iNoFresh, kRE2iNoLend, kRE2iUndecided, kRE2iNotLent,
    kRPwFrame, kRPwDraw, kRPwControl,
    kRIpOff, kRIpNoVp2, kRIpNoTaa, kRIpNoRows, kRIpBroken, kRIpNoMark, kRIpBad, kRIpOverK, kRIpCap, kRIpFault, kRIpAsEye1, kRIpNocb, kRIpCtx,
    kRIndirect,
    kReasons
};
struct ReasonInfo {
    const char* name;
    int cat;
};
constexpr ReasonInfo kReason[kReasons] = {
    {"pso_list", kCPso}, {"pso_slot", kCPso}, {"bind", kCBind},
    {"nt_stream", kCNoTwin}, {"nt_queued", kCNoTwin}, {"nt_ready", kCNoTwin}, {"nt_vs_refused", kCNoTwin}, {"nt_ps_refused", kCNoTwin},
    {"nt_held_wvp", kCNoTwin}, {"nt_held_ip", kCNoTwin}, {"nt_held_e2i", kCNoTwin}, {"nt_held_pw", kCNoTwin}, {"nt_held", kCNoTwin},
    {"nt_build_failed", kCNoTwin}, {"nt_shape", kCNoTwin}, {"nt_norule", kCNoTwin},
    {"kind_wvp", kCKind},
    {"e2i_no_kind", kCE2i}, {"e2i_no_live", kCE2i}, {"e2i_no_guard", kCE2i}, {"e2i_no_fresh", kCE2i}, {"e2i_no_lend", kCE2i},
    {"e2i_undecided", kCE2i}, {"e2i_not_lent", kCE2i},
    {"pw_frame", kCPw}, {"pw_draw", kCPw}, {"pw_control", kCPw},
    {"ip_off", kCIp}, {"ip_no_vp2", kCIp}, {"ip_no_taa", kCIp}, {"ip_no_rows", kCIp}, {"ip_broken", kCIp}, {"ip_no_mark", kCIp}, {"ip_bad", kCIp},
    {"ip_over_k", kCIp}, {"ip_cap", kCIp}, {"ip_fault", kCIp}, {"ip_as_eye1", kCIp}, {"ip_nocb", kCIp}, {"ip_ctx", kCIp},
    {"indirect", kCIndirect},
};
static_assert(sizeof(kReason) / sizeof(kReason[0]) == kReasons, "a name for every reason");
enum Twin : int { kTPlain, kTPw, kTE2i, kTPwE2i, kTIp, kTIpE2i, kTOther, kTwins };
const char* const kTwinName[kTwins] = {"plain", "pw", "e2i", "pw_e2i", "ip", "ip_e2i", "other"};
const char* const kStateName[rg::kRsStates] = {"stream", "queued", "ready", "made", "vs_refused", "ps_refused", "held", "build_failed", "shape", "norule"};

// ---- the render thread: the ring of the frame's gates (slot: state::presents & 7), its markers
// (tier 2) kFWhy1/kFWhy2: that pass records its entity markers (its open saw "misses why on"); kFEnt: an entity marker not recorded
enum RingFlag : uint32_t { kSt1 = 1, kStE1 = 2, kSt2 = 4, kFOther = 8, kFMark = 16, kFWhy1 = 32, kFWhy2 = 64, kFEnt = 128 };
struct RingSlot {
    std::atomic<uint64_t> frame{~0ull};
    std::atomic<uint32_t> gates{0}, flags{0};
    std::atomic<uint64_t> ebase[2]{};  // (tier 2) the entity ring's sequence at each pass's open marker: the playback's cursor starts there
};
RingSlot g_ring[8];
thread_local int t_rpass = 0, t_rslot = 0;  // the pass whose markers this DVL call 1 records (dvl_open .. dvl_close), and its slot
uint64_t g_other0 = 0, g_foreign0 = 0;      // (pass 1's open) the other threads' instprev binds and PrevWorld writes so far
Count g_r_opens[3], g_r_open_failed, g_r_e1[3], g_r_e1_failed, g_r_segs, g_r_seg_failed, g_r_unpaired2, g_r_fade_ungated, g_r_fade_unread;

// ---- tier 2 (item8d.md section 3.4, "prepass mr misses why on"): each pass-2 miss's cause. C1 culled in pass 1 (the entity drew no
// e1 draw in pass 1 and at least one in pass 2), split by its bounding sphere against pass 1's planes (eye 1's near plane, the union's
// side planes: near / side / inside all five), or unsplit (no sphere read), or absent (no pass-1 entity call at all); C2 the entity drew
// in both passes with other keys: lod (the miss's pipeline among the entity's pass-1 draws: LOD [I strong]) or geom (absent from
// them: a geometry cull; open: the entity drew more pipelines in pass 1 than kept, none matched); C3 a b0 post / b1 post draw (a
// callback, not the entity loop). Not named: pre (before bucket 0's first entity), no_ent (an entity segment with no entity
// cursor), unattributed (the frame's entity markers incomplete), lost (past the frame's first kWMiss misses)
enum Cause : int { kWC1Near, kWC1Side, kWC1Inside, kWC1Unsplit, kWC1Absent, kWC2Lod, kWC2Geom, kWC2Open, kWC3, kWPre, kWNoEnt, kWUnatt, kWLost, kCauses };
const char* const kCauseName[kCauses] = {"c1_near", "c1_side", "c1_inside", "c1_unsplit", "c1_absent", "c2_lod", "c2_geom", "c2_open", "c3_post",
                                         "pre", "no_ent", "unattributed", "lost"};
inline bool cause_named(int c) { return c <= kWC3; }

// ---- the playback thread: the pair table (VS^PS; the census of what e1 draws)
constexpr unsigned kPairs = 512, kPairProbe = 32;
struct Pair {
    std::atomic<uint64_t> vs{0}, ps{0};
    std::atomic<int> used{0};
    std::atomic<int> exp1{0};       // its row of 8a's census (pso.h e1_pairs) + 1, 0 not in it (zero: the table stays in .bss)
    std::atomic<uint32_t> segs{0};  // the segments pass 1 drew it in (bits 0-4) and pass 2 (bits 8-12)
    std::atomic<uint32_t> st{0};    // the last slot state seen (pso.h region: the state and its kinds)
    std::atomic<int> ord{0};        // (item 8e) its ordinal + 1 (0: none yet; -1: none to give, past 63 or no pipeline), pair_ord's
    Count d1{0}, d2{0}, empty{0}, pred{0}, singles{0}, miss{0};
    Count rep{0};  // (judged frames) pass 1's draws whose key was already counted in this frame (its second and later draws)
    Count cat[kCats]{};
    Count why[kCauses]{};  // (tier 2, judged frames) its misses by cause
    Count hid{0};  // (tier 2, item 8e's bisect step 0) its pass-2 draws past its entity's pass-1 count (an entity-level miss)
};
Pair g_pair[kPairs];  // (static)
Count g_pair_full;
// (item 8e) the session's pair ordinals, 0..63 in order of their first non-empty pass-1 e1 draw (pass 2's and empty draws never spend
// one; written by the playback thread only, kept across "mr reset"; a pair past 63 gets none and is never listed by main_inst, counted)
constexpr int kOrds = 64;
std::atomic<uint64_t> g_ord_vs[kOrds], g_ord_ps[kOrds];
std::atomic<int> g_ord_n{0};
std::atomic<uint32_t> g_ord_st[kOrds];  // (the last pass-1 slot state seen per ordinal: the controls' applicability by kind)
Count g_ord_over;
int ord_assign(uint64_t vs, uint64_t ps) {
    if (!vs && !ps) return -1;  // (no pipeline known)
    const int n = g_ord_n.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (g_ord_vs[i].load(std::memory_order_relaxed) == vs && g_ord_ps[i].load(std::memory_order_relaxed) == ps) return i;
    if (n >= kOrds) {
        bump(g_ord_over);
        return -1;
    }
    g_ord_vs[n].store(vs, std::memory_order_relaxed);
    g_ord_ps[n].store(ps, std::memory_order_relaxed);
    g_ord_n.store(n + 1, std::memory_order_release);  // (last: a reader sees the ordinal whole)
    return n;
}
int pair_of(uint64_t vs, uint64_t ps) {
    unsigned i = static_cast<unsigned>(main_inst::keys::digest(vs ^ ps ^ 0x5bd1e9955bd1e995ull));
    for (unsigned p = 0; p < kPairProbe; ++p, ++i) {
        Pair& e = g_pair[i & (kPairs - 1)];
        if (!e.used.load(std::memory_order_relaxed)) {
            e.vs.store(vs, std::memory_order_relaxed);
            e.ps.store(ps, std::memory_order_relaxed);
            e.exp1.store(vs || ps ? pso::e1_pair(vs, ps) + 1 : 0, std::memory_order_relaxed);
            e.ord.store(0, std::memory_order_relaxed);  // (item 8e: given by pair_ord at its first non-empty pass-1 e1 draw)
            e.used.store(1, std::memory_order_release);
            return static_cast<int>(i & (kPairs - 1));
        }
        if (e.vs.load(std::memory_order_relaxed) == vs && e.ps.load(std::memory_order_relaxed) == ps) return static_cast<int>(i & (kPairs - 1));
    }
    bump(g_pair_full);
    return -1;
}
// (item 8e) a pair's ordinal (-1 none); assign: a non-empty pass-1 e1 draw, which gives the pair its ordinal the first time (a pair past
// 63, or with no pipeline, marked -1 and not tried again until "mr reset" clears the pairs)
int pair_ord(int pair, bool assign) {
    if (pair < 0) return -1;
    Pair& e = g_pair[pair];
    const int o = e.ord.load(std::memory_order_relaxed);
    if (o > 0) return o - 1;
    if (o < 0 || !assign) return -1;
    const int n = ord_assign(e.vs.load(std::memory_order_relaxed), e.ps.load(std::memory_order_relaxed));
    e.ord.store(n >= 0 ? n + 1 : -1, std::memory_order_relaxed);
    return n;
}

// ---- the playback thread: the frame in flight
constexpr int kTop = 8, kMissList = 64, kRepList = 256;
// a draw's key by its vertex buffer slot 0 (tier 1): kDNone the address (6e's key), or in the dynamic vertex ring (below) kDHash
// its own vertices' bytes, kDTag the ring's tag (no bytes read: indexed, over kHashMax, the ring not mapped), kDFault a read faulted
enum Dyn : int { kDNone, kDHash, kDTag, kDFault, kDyns };
const char* const kDynName[kDyns] = {"none", "hash", "tag", "fault"};
struct Rec {  // pass 1's e1 (and pass 2's keyed draws)
    uint32_t draws, empty, pred, indirect, singles, treerock, ip_draws, after_e1;
    uint32_t cats;  // the single classes seen (bits of Cat)
    uint32_t reason[kReasons];
    uint32_t twin[kTwins];
    uint32_t pred_cat[kCats];
    int tp_pair[kTop];  // this frame's singles by pair (the top one named in the ring)
    uint32_t tp_n[kTop];
    int tp_used;
    uint32_t draws2, empty2, indirect2;
    uint32_t miss_seg[kSegs];
    int miss_pair[kMissList];
    uint32_t miss_n, miss_lost;
    int rep_pair[kRepList];  // pass 1's repeated keys by pair (counted into the pair table when the frame is judged)
    uint32_t rep_n, rep_used, rep_lost;
    uint32_t dyn[3][kDyns];  // [pass] keyed draws by Dyn
    bool ip_live;
    // (item 8e) pass 1's non-empty e1 draws by ordinal, the ordinals with a single, the non-empty draws of pairs with no ordinal
    uint16_t pd1[kOrds];
    uint64_t single_ords;
    uint32_t noord;
};

// ---- tier 2's data. An entity's bounding sphere as its draw-time cull reads it (Ghidra, read-only): rmcDrawable's IsVisible
// (vt+0x68, 0x140875030; DrawVisEntity's vt+0xd call in kind 2, and its twin 0x140875190 at vt+0x60) takes (drawable, the rec's
// world matrix rec+0x00, rdrvr_ViewportCurrent) and tests IsSphereVisible 0x140130030 on centre = World x drawable+0x20..+0x28,
// radius drawable+0x90 (near plane +0x3d0, side planes +0x390..+0x3cf: n.c + d + r >= 0 passes each; the near-plane distance
// then picks the LOD from drawable+0x70[3] / +0x50[3]). The drawable is rec+0x80 (kind (rec+0xac >> 13) & 7 == 0: DrawVisEntity
// calls its vt+0x30 0x875370 = SetWorldMtx(rec) + per model grmModel's box culler 0x142b00 on +0x2c0, the side planes only, the
// LOD from rec+0xac bits 10-12, i.e. the shared visibility pass's) or its +0x08 / +0x10 (kind 2: the vt+0xd call itself, its
// LOD per pass). Kind 1 (FUN_140a28d10, the fragment draw): IsSphereVisible on centre = M x bounds+0x50..+0x58, radius bounds
// +0x20, where o = *(rec+0x80)+0x80, M = o+0x20 (rows +0x20/+0x30/+0x40, translation +0x50), bounds = *(*(o+8)+0x18); its LOD per
// pass by the near-plane distance. Margins are n.c + d + r (>= 0: that plane passes the sphere); negative: outside it by that much
struct Sph {
    float r, c[3];
    float nearm;     // the pass's near plane (+0x3d0; never swapped: eye 1's in pass 1, eye 2's in pass 2)
    float side[4];   // the current side planes (+0x390: the union's in a union-culled pass 1, eye 2's in pass 2)
    float eside[4];  // the eye's own side planes (pass 1: main_inst's saved eye-1 planes; pass 2: = side)
};
constexpr int kSphF = 13;  // (the floats of Sph)
static_assert(sizeof(Sph) == kSphF * sizeof(float), "Sph is its floats");
enum SphSrc : uint32_t { kSNone, kSDrawable, kSDrawableSub, kSFrag, kSrcs };
const char* const kSrcName[kSrcs] = {"none", "drawable", "sub", "frag"};
// the render thread's entity ring (sequence-stamped, a seqlock per entry): ring[seq] = {rec, bucket, pass, frame, kind bits, sphere}
constexpr unsigned kERing = 8192;
struct ERing {
    std::atomic<uint64_t> stamp{0};  // seq + 1 once written (0 while written)
    std::atomic<uint64_t> rec{0}, frame{0};
    std::atomic<uint32_t> meta{0};   // bucket | pass << 8 | SphSrc << 16
    std::atomic<uint32_t> ac{0};     // rec+0xac (kind bits 13-15, LOD bits 10-12)
    std::atomic<uint32_t> f[kSphF]{};
};
ERing g_ering[kERing];  // (static: 0.6 MB)
// the playback thread's entities of the frame (both passes; generation-stamped, no clearing), keyed by (rec, bucket)
constexpr unsigned kEntSlots = 2048, kEntProbe = 64, kEntPso = 8, kEntKeys = 4;
struct Ent {
    uint64_t rec;
    uint32_t gen, bucket;
    uint32_t called[2], n[2], empty[2], ac[2], ord[2];  // [pass - 1]: calls, non-empty e1 draws, empty ones, rec+0xac, first ordinal
    uint32_t src[2];
    uint32_t npso, nk;
    bool pso_over, k_over;  // (more distinct pipelines / keys in pass 1 than kept)
    const void* pso[kEntPso];  // pass 1's pipelines (distinct)
    uint64_t k[kEntKeys];      // pass 1's keys without the vertex buffer (distinct, the first kEntKeys)
    Sph s[2];
};
Ent g_et[kEntSlots];  // (static: about 0.5 MB)
uint32_t g_et_gen = 0;
constexpr int kWMiss = 64;  // a frame's misses classified (the rest "lost")
struct WMiss {
    int ent;
    int seg, pair;
    const void* pso;
    uint64_t kv, k;
    d3d::DrawArgs a;
};
constexpr unsigned kWd = 16;  // the first misses since the reset, with their details
struct WDetail {
    uint64_t frame, rec, vs, ps, kv, k;
    int cause, seg, bucket, key1;  // key1: this key (no VB) among the entity's pass-1 keys (1), not (0), not kept (-1)
    uint32_t called[2], n[2], empty[2], ac[2], ord[2], src[2], npso;
    d3d::DrawArgs a;
    Sph s[2];
};
WDetail g_wd[kWd];
std::atomic<uint32_t> g_wd_n{0};

struct Pb {
    int st = 0;     // 0 none (or finalized), 1 pass 1 open, 2 pass 1's e1 reached, 3 pass 1 closed, 4 pass 2 open
    int pass = 0;   // the open region pass's (0: none)
    bool in_e1 = false;
    bool e1_1 = false;  // pass 1 reached its e1
    int seg = 0;
    int slot = 0;
    uint64_t frame = 0;
    DWORD tid = 0;
    uint32_t gates = 0, unj = 0;
    Rec r{};
    main_inst::keys::Frame kf;
    // (tier 2) each pass's entity markers recorded (its open's flag), the frame's markers broken (why: 1 stray, 2 lost, 4 mismatch,
    // 8 a marker not recorded), the cursor into the entity ring, the current entity (the slot; -1 none), the pass's next ordinal
    bool why1 = false, why2 = false;
    uint32_t wbad = 0;
    uint64_t ecur = 0;
    int ent = -1;
    uint32_t eord = 0;
    uint32_t gen = 0;
    WMiss wm[kWMiss];
    uint32_t wm_n = 0, wm_lost = 0;
    // (tier 2, item 8e's bisect step 0) this frame's pass-2 draws past their entity's pass-1 count (key misses too), their pairs (counted
    // per pair only in an attributed frame), and its pass-2 draws whose key (no VB) is not among their entity's kept pass-1 keys
    uint32_t hid = 0, hid_pn = 0, hidk = 0;
    int16_t hid_pair[64];
};
Pb g_pb;
constexpr unsigned kKeySlots = 8192;  // (e1 draws about 1,000-1,500 a pass)
main_inst::keys::Slot g_ktab[2][kKeySlots];
main_inst::keys::Tables g_keys{{g_ktab[0], g_ktab[1]}, kKeySlots, 0};

// ---- the dynamic vertex ring (the diagnosis of 8d's first run: its b0-post miss, PolyFiller 0x6ddbe0, item8d.md section 6). The
// immediate-mode vertices FUN_14012e280 (count, stride) hands out sit in one of DAT_142b01150[n] (n DAT_142b01158: the device's
// frame count), each a 32 MB buffer made by FUN_14012e420 (FUN_140ec1c80(dev, &b, usage 3, 0x2000000): the class FUN_140f8ea60,
// vtable 0x141094cd0, kind +0x30 0 = vertex, usage +0x34 3), advanced by DAT_142c371c8. The buffer's GPU address is +0x40 and
// its size +0x48 (FUN_140f8ea60); the immediate context's SetVertexBuffer (vt+0xc0, FUN_140f7fee0) binds +0x40 + offset with
// SizeInBytes +0x48 - offset (to the ring's end: the view's size differs between the passes too); its Map (vt+0x28, FUN_140f8f520)
// caches the persistent mapping at +0x18 (usage 3: ID3D12Resource::Map once, Unmap vt+0x30 FUN_140f8fd80 a no-op) and returns
// +0x18 + offset. So the GPU address +0x40 + o is the CPU bytes +0x18 + o, the engine's own pairing. The code is checked against
// the analysed build at install (the two RIP-relative references and the three vtable entries); otherwise no draw is in the ring.
constexpr uintptr_t kRvaRingRef = 0x12e316;   // FUN_14012e280: mov rdx, [DAT_142b01150] (48 8b 15 disp32)
constexpr uintptr_t kRvaBufVtRef = 0xf8ea94;  // FUN_140f8ea60: lea rax, [vtable 0x141094cd0] (48 8d 05 disp32)
constexpr uintptr_t kRvaBufMap = 0xf8f520, kRvaBufUnmap = 0xf8fd80, kRvaCtxSetVb = 0xf7fee0;
constexpr uint32_t kRingBytes = 0x2000000;
constexpr unsigned kRingMax = 8, kHashMax = 4096;
struct RingBuf {
    uint64_t va, end;
    uintptr_t cpu;  // (0: not mapped yet; its draws keyed by the tag)
};
RingBuf g_rb[kRingMax];            // (the playback thread's: refreshed at each pass 1 open)
unsigned g_rb_n = 0;
uintptr_t g_ring_slot = 0, g_buf_vt = 0;  // (install) &DAT_142b01150 and the buffer class's vtable; 0: not as analysed
char g_ring_why[120] = "not installed";
std::atomic<uint32_t> g_rb_last{0}, g_rb_mapped{0};  // (the last refresh: buffers found, mapped)
Count g_rb_refresh, g_rb_none, g_rb_bad, g_dyn[3][kDyns], g_k_rep_draws, g_rep_lost;

// totals (the playback thread's; judged frames unless named otherwise)
Count g_f_judged, g_f_clean, g_f_single, g_f_gated, g_f_unjudged, g_unj[kUnjs], g_gate[kGates], g_hist[5], g_unj_draws;
Count g_reason[kReasons], g_twin[kTwins], g_pred_cat[kCats], g_draws, g_empty, g_pred, g_indirect, g_treerock, g_ip_draws, g_after_e1, g_draws2,
    g_empty2, g_indirect2, g_singles;
Count g_k_frames, g_k_within, g_k_within_nvb, g_k_miss, g_k_miss_nvb, g_k_rep, g_k_n1, g_k_n2, g_miss_seg[kSegs], g_miss_lost;
std::atomic<uint64_t> g_k_last_sum1{0}, g_k_last_sum2{0}, g_k_last_miss{0};
Count g_p_opens[3], g_p_e1[3], g_p_e1_lost, g_p_seg[kSegs], g_p_seg_lost, g_p_closes[3], g_p_orphan2;
// the cost: the classification's cycles per pass (rdtsc), calibrated against QPC since the last reset
Count g_tsc[3], g_tsc_n[3];
std::atomic<uint64_t> g_tsc0{0};
std::atomic<int64_t> g_qpc0{0};
// tier 2: the render thread's (wr_), the playback's (wp_), the judged frames' (w_)
std::atomic<int> g_why{0};          // "prepass mr misses why on|off" (test only; armed with observe)
bool g_why_sph = false;             // (install) the sphere's code as analysed: the C1 split; else C1 "cull, unsplit"
char g_why_note[160] = "not installed";
Count g_wr_ents, g_wr_failed, g_wr_tsc, g_wr_src[kSrcs], g_wr_sph_fault, g_wr_sph_bad, g_wr_vp_bad, g_wr_eye_saved;
Count g_wp_ents[3], g_wp_stray, g_wp_lost, g_wp_mismatch, g_wp_full, g_wp_tsc, g_wp_noent_draws;
Count g_w_frames, g_w_frames_bad, g_w_bad[4], g_w_frames_miss, g_wc[kCauses], g_wc_kind[kCauses][8], g_w_c1_plane[4], g_w_c2_lodbits, g_w_key1[3];
// (item 8e's bisect step 0) attributed frames with an entity-level miss (Pb::hid), those the keys called within, their draws; the first
// kWh such frames since the reset (the frame, its draws past their entities' counts, within or not)
Count g_w_hid_frames, g_w_hid_within, g_w_hid_draws, g_w_hidk_frames, g_w_hidk_within, g_w_hidk_draws;
constexpr unsigned kWh = 16;
// (item 8e's bisect step 0b) pass 1's extra draws by entity (n[0] > n[1]) in attributed frames: the entity not called in pass 2, called
// and outside one of eye 2's planes (the union's own extra), its LOD bits differing, inside every eye-2 plane with the same LOD bits
// (dropped by pass 2 for another reason), or no pass-2 sphere; the frames with any "lod" or "in" extra, and the first kWx of those
// ("emptied": pass 2 issued as many draws, some with no instances or vertices)
enum { kXUncalled, kXOut, kXLod, kXIn, kXNoSph, kXEmptied, kXCats };
const char* const kXName[kXCats] = {"uncalled", "out", "lod", "in", "nosph", "emptied"};
Count g_wx_frames, g_wx_li_frames, g_wx[kXCats], g_wx_draws[kXCats];
constexpr unsigned kWx = 16;
struct WX {
    uint64_t frame, rec;
    uint32_t cat, kind, n0, n1, e0, e1, called1, ac0, ac1, vs, ps;
    float r, c[3], m2;  // (m2: the smallest of pass 2's margins, near and sides)
};
WX g_wxd[kWx];
std::atomic<uint32_t> g_wxd_n{0};
std::atomic<uint32_t> g_wxd_emptied{0};  // (the playback thread) the details' emptied ones, at most 4: lod and in keep the rest
// (item 8e's bisect step 0c, r112 U7: the failure needs several pairs twinned) the entities' order: those called in both passes, by
// their first pass-1 ordinal; an entity whose first pass-2 ordinal is below the most seen before it is out of order (pass 2 drew it
// before an entity pass 1 drew first). Counted as the fewest entities moved: m less the longest run in order (pass 2's ordinals by
// pass 1's). The frames with any, their counts, and the first kWo neighbours, one per entity holding the running maximum
Count g_wo_frames, g_wo_inv, g_wo_ents;
constexpr unsigned kWo = 12;
struct WO {
    uint64_t frame, rec[2];
    uint32_t bucket[2], ord0[2], ord1[2], vs[2], ps[2];
    float c[2][3], r[2];
};
WO g_wod[kWo];
std::atomic<uint32_t> g_wod_n{0};
std::atomic<uint64_t> g_wof[32];  // (frame << 16 | out-of-order entities, capped at 65535)
std::atomic<uint32_t> g_wof_n{0};
std::pair<uint32_t, uint32_t> g_wo_sort[kEntSlots];  // (the playback thread) (first pass-1 ordinal, slot)
uint32_t g_wo_tail[kEntSlots];  // (the playback thread) the longest increasing run's tails (patience sorting)
constexpr unsigned kWxf = 32;
std::atomic<uint64_t> g_wxf[kWxf];  // (frame << 16 | lod count << 8 | in count, each capped at 255)
std::atomic<uint32_t> g_wxf_n{0};
std::atomic<uint64_t> g_wh[kWh];
std::atomic<uint32_t> g_wh_n{0};
constexpr const char* kWBadName[4] = {"stray", "lost", "mismatch", "unrecorded"};
// the ring of the last 64 frames that were not clean (1 single, 2 gated, 3 unjudged)
constexpr unsigned kNc = 64;
struct Nc {
    std::atomic<uint64_t> frame{0}, vs{0}, ps{0};
    std::atomic<uint32_t> kind{0}, gates{0}, unj{0}, cats{0}, singles{0}, top_n{0};
};
Nc g_nc[kNc];
std::atomic<uint32_t> g_nc_next{0};

void reset_all() {  // (the playback thread at its pass 1 open, or the command thread while nothing is observed)
    for (Count* c : {&g_rb_refresh, &g_rb_none, &g_rb_bad, &g_k_rep_draws, &g_rep_lost}) c->store(0);
    // (tier 2: the render thread's counters too, as main_region's own r_* are: single writers, a reset may race one bump)
    for (Count* c : {&g_wr_ents, &g_wr_failed, &g_wr_tsc, &g_wr_sph_fault, &g_wr_sph_bad, &g_wr_vp_bad, &g_wr_eye_saved, &g_wp_stray, &g_wp_lost,
                     &g_wp_mismatch, &g_wp_full, &g_wp_tsc, &g_wp_noent_draws, &g_w_frames, &g_w_frames_bad, &g_w_frames_miss, &g_w_c2_lodbits, &g_w_hid_frames,
                     &g_w_hid_within, &g_w_hid_draws, &g_w_hidk_frames, &g_w_hidk_within, &g_w_hidk_draws, &g_wx_frames, &g_wx_li_frames, &g_wo_frames, &g_wo_inv, &g_wo_ents})
        c->store(0);
    for (Count& c : g_wr_src) c.store(0);
    for (Count& c : g_wp_ents) c.store(0);
    for (Count& c : g_w_bad) c.store(0);
    for (Count& c : g_wc) c.store(0);
    for (auto& row : g_wc_kind)
        for (Count& c : row) c.store(0);
    for (Count& c : g_w_c1_plane) c.store(0);
    for (Count& c : g_w_key1) c.store(0);
    g_wd_n.store(0, std::memory_order_release);
    g_wh_n.store(0, std::memory_order_release);
    for (Count& c : g_wx) c.store(0);
    for (Count& c : g_wx_draws) c.store(0);
    g_wxd_n.store(0, std::memory_order_release);
    g_wxd_emptied.store(0, std::memory_order_relaxed);
    g_wxf_n.store(0, std::memory_order_release);
    g_wod_n.store(0, std::memory_order_release);
    g_wof_n.store(0, std::memory_order_release);
    for (auto& row : g_dyn)
        for (Count& c : row) c.store(0);
    for (Count* c : {&g_f_judged, &g_f_clean, &g_f_single, &g_f_gated, &g_f_unjudged, &g_unj_draws, &g_draws, &g_empty, &g_pred, &g_indirect,
                     &g_treerock, &g_ip_draws, &g_after_e1, &g_draws2, &g_empty2, &g_indirect2, &g_singles, &g_k_frames, &g_k_within, &g_k_within_nvb,
                     &g_k_miss, &g_k_miss_nvb, &g_k_rep, &g_k_n1, &g_k_n2, &g_miss_lost, &g_p_e1_lost, &g_p_seg_lost, &g_p_orphan2, &g_pair_full,
                     &g_r_open_failed, &g_r_e1_failed, &g_r_segs, &g_r_seg_failed, &g_r_unpaired2, &g_r_fade_ungated, &g_r_fade_unread})
        c->store(0);
    for (Count& c : g_unj) c.store(0);
    for (Count& c : g_gate) c.store(0);
    for (Count& c : g_hist) c.store(0);
    for (Count& c : g_reason) c.store(0);
    for (Count& c : g_twin) c.store(0);
    for (Count& c : g_pred_cat) c.store(0);
    for (Count& c : g_miss_seg) c.store(0);
    for (Count& c : g_p_seg) c.store(0);
    for (int i = 0; i < 3; ++i)
        for (Count* c : {&g_p_opens[i], &g_p_e1[i], &g_p_closes[i], &g_tsc[i], &g_tsc_n[i], &g_r_opens[i], &g_r_e1[i]}) c->store(0);
    for (Pair& e : g_pair) {  // (single writer: the playback thread, or none at all)
        e.used.store(0, std::memory_order_relaxed);
        e.vs.store(0, std::memory_order_relaxed);
        e.ps.store(0, std::memory_order_relaxed);
        e.exp1.store(0, std::memory_order_relaxed);
        e.segs.store(0, std::memory_order_relaxed);
        e.st.store(0, std::memory_order_relaxed);
        e.ord.store(0, std::memory_order_relaxed);  // (item 8e: the ordinal table itself kept; a re-inserted pair gets its own back)
        for (Count* c : {&e.d1, &e.d2, &e.empty, &e.pred, &e.singles, &e.miss, &e.rep, &e.hid}) c->store(0);
        for (Count& c : e.cat) c.store(0);
        for (Count& c : e.why) c.store(0);
    }
    for (Nc& n : g_nc) n.kind.store(0, std::memory_order_relaxed);
    g_nc_next.store(0, std::memory_order_relaxed);
    g_k_last_sum1.store(0);
    g_k_last_sum2.store(0);
    g_k_last_miss.store(0);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_qpc0.store(q.QuadPart);
    g_tsc0.store(__rdtsc());
}

void nc_push(uint32_t kind, uint32_t gates, uint32_t unj, const Rec& r, uint64_t frame) {
    int top = -1;
    uint32_t tn = 0;
    for (int k = 0; k < r.tp_used; ++k)
        if (r.tp_n[k] > tn) {
            tn = r.tp_n[k];
            top = r.tp_pair[k];
        }
    Nc& n = g_nc[g_nc_next.load(std::memory_order_relaxed) % kNc];
    n.frame.store(frame, std::memory_order_relaxed);
    n.kind.store(kind, std::memory_order_relaxed);
    n.gates.store(gates, std::memory_order_relaxed);
    n.unj.store(unj, std::memory_order_relaxed);
    n.cats.store(r.cats, std::memory_order_relaxed);
    n.singles.store(r.singles, std::memory_order_relaxed);
    n.vs.store(top >= 0 ? g_pair[top].vs.load(std::memory_order_relaxed) : 0, std::memory_order_relaxed);
    n.ps.store(top >= 0 ? g_pair[top].ps.load(std::memory_order_relaxed) : 0, std::memory_order_relaxed);
    n.top_n.store(tn, std::memory_order_relaxed);
    g_nc_next.store(g_nc_next.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// ---- tier 2: a judged frame's misses by cause (the playback thread, at the frame's verdict)
int kind_of(uint32_t ac) { return static_cast<int>((ac >> 13) & 7); }
int classify(const Pb& p, const WMiss& m, int* plane) {
    *plane = -1;
    if (m.seg == 2 || m.seg == 4) return kWC3;  // (b0 post, b1 post: a callback's draw)
    if (m.seg == 0) return kWPre;
    if (m.ent < 0 || g_et[m.ent].gen != p.gen) return kWNoEnt;
    const Ent& e = g_et[m.ent];
    if (e.n[0] == 0) {  // C1: no e1 draw of the entity in pass 1
        if (!e.called[0]) return kWC1Absent;
        if (e.src[0] == kSNone) return kWC1Unsplit;
        const Sph& s = e.s[0];
        if (s.nearm < 0.0f) return kWC1Near;
        int w = 0;
        for (int i = 1; i < 4; ++i)
            if (s.side[i] < s.side[w]) w = i;
        if (s.side[w] < 0.0f) {
            *plane = w;
            return kWC1Side;
        }
        return kWC1Inside;
    }
    for (uint32_t i = 0; i < e.npso; ++i)
        if (e.pso[i] == m.pso) return kWC2Lod;
    return e.pso_over ? kWC2Open : kWC2Geom;
}
void why_frame(Pb& p) {
    if (!p.why1 && !p.why2) return;  // (tier 2 off for this frame: tier 1's names only)
    const bool ok = p.why1 && p.why2 && !p.wbad;
    if (ok) {
        bump(g_w_frames);
    } else {
        bump(g_w_frames_bad);
        for (int b = 0; b < 4; ++b)
            if (p.wbad & (1u << b)) bump(g_w_bad[b]);
    }
    if (p.wm_n || p.wm_lost) bump(g_w_frames_miss);
    if (ok) {  // (step 0b: pass 1's extras by entity, this frame's generation)
        uint32_t nx[kXCats] = {};
        for (const Ent& e : g_et) {
            if (e.gen != p.gen || e.n[0] <= e.n[1]) continue;
            int c = kXIn;
            float m2 = 0.0f;
            if (!e.called[1]) {
                c = kXUncalled;
            } else if (e.n[0] + e.empty[0] <= e.n[1] + e.empty[1]) {
                c = kXEmptied;
            } else if (((e.ac[0] >> 10) & 7) != ((e.ac[1] >> 10) & 7)) {
                c = kXLod;
            } else if (e.src[1] == kSNone) {
                c = kXNoSph;
            } else {
                m2 = e.s[1].nearm;
                for (int q = 0; q < 4; ++q) m2 = e.s[1].side[q] < m2 ? e.s[1].side[q] : m2;
                if (m2 < 0.0f) c = kXOut;
            }
            ++nx[c];
            bump(g_wx[c]);
            bump(g_wx_draws[c], e.n[0] - e.n[1]);
            if (c != kXLod && c != kXIn && c != kXEmptied) continue;
            const uint32_t n = g_wxd_n.load(std::memory_order_relaxed);
            if (n >= kWx || (c == kXEmptied && g_wxd_emptied.load(std::memory_order_relaxed) >= 4)) continue;
            if (c == kXEmptied) g_wxd_emptied.store(g_wxd_emptied.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            WX& x = g_wxd[n];
            x = WX{};
            x.frame = p.frame;
            x.rec = e.rec;
            x.cat = static_cast<uint32_t>(c);
            x.kind = static_cast<uint32_t>(kind_of(e.ac[0]));
            x.n0 = e.n[0];
            x.n1 = e.n[1];
            x.e0 = e.empty[0];
            x.e1 = e.empty[1];
            x.called1 = e.called[1];
            x.ac0 = e.ac[0];
            x.ac1 = e.ac[1];
            pso::RegionInfo ri;
            if (e.npso && e.pso[0] && pso::region_lookup(e.pso[0], &ri)) {
                x.vs = static_cast<uint32_t>(ri.vs >> 32);
                x.ps = static_cast<uint32_t>(ri.ps >> 32);
            }
            const Sph& s = e.s[e.src[1] != kSNone ? 1 : 0];
            x.r = s.r;
            x.c[0] = s.c[0];
            x.c[1] = s.c[1];
            x.c[2] = s.c[2];
            x.m2 = m2;
            g_wxd_n.store(n + 1, std::memory_order_release);
        }
        bool any = false;
        for (uint32_t v : nx) any = any || v;
        if (any) bump(g_wx_frames);
        if (nx[kXLod] || nx[kXIn]) {
            bump(g_wx_li_frames);
            const uint32_t n = g_wxf_n.load(std::memory_order_relaxed);
            g_wxf[n % kWxf].store(p.frame << 16 | (nx[kXLod] < 255 ? nx[kXLod] : 255) << 8 | (nx[kXIn] < 255 ? nx[kXIn] : 255),
                                  std::memory_order_relaxed);
            g_wxf_n.store(n + 1, std::memory_order_release);
        }
    }
    if (ok) {  // (step 0c: the order of the entities both passes drew)
        unsigned m = 0;
        for (unsigned i = 0; i < kEntSlots; ++i) {
            const Ent& e = g_et[i];
            if (e.gen == p.gen && e.n[0] && e.n[1] && e.ord[0] != ~0u && e.ord[1] != ~0u) g_wo_sort[m++] = {e.ord[0], i};
        }
        std::sort(g_wo_sort, g_wo_sort + m);
        uint32_t inv = 0, best = 0, lis = 0;
        unsigned at = 0, noted = ~0u;
        for (unsigned k = 0; k < m; ++k) {
            const Ent& e = g_et[g_wo_sort[k].second];
            const uint32_t pos = static_cast<uint32_t>(std::lower_bound(g_wo_tail, g_wo_tail + lis, e.ord[1]) - g_wo_tail);
            g_wo_tail[pos] = e.ord[1];
            if (pos == lis) ++lis;
            if (k && e.ord[1] < best && at != noted) {
                noted = at;
                const uint32_t n = g_wod_n.load(std::memory_order_relaxed);
                if (n < kWo) {
                    const Ent* q[2] = {&g_et[g_wo_sort[at].second], &e};
                    WO& o = g_wod[n];
                    o = WO{};
                    o.frame = p.frame;
                    for (int s = 0; s < 2; ++s) {
                        o.rec[s] = q[s]->rec;
                        o.bucket[s] = q[s]->bucket;
                        o.ord0[s] = q[s]->ord[0];
                        o.ord1[s] = q[s]->ord[1];
                        pso::RegionInfo ri;
                        if (q[s]->npso && q[s]->pso[0] && pso::region_lookup(q[s]->pso[0], &ri)) {
                            o.vs[s] = static_cast<uint32_t>(ri.vs >> 32);
                            o.ps[s] = static_cast<uint32_t>(ri.ps >> 32);
                        }
                        const Sph& sp = q[s]->s[q[s]->src[1] != kSNone ? 1 : 0];
                        o.r[s] = sp.r;
                        for (int t = 0; t < 3; ++t) o.c[s][t] = sp.c[t];
                    }
                    g_wod_n.store(n + 1, std::memory_order_release);
                }
            }
            if (!k || e.ord[1] > best) {
                best = e.ord[1];
                at = k;
            }
        }
        inv = m - lis;
        bump(g_wo_ents, m);
        if (inv) {
            bump(g_wo_frames);
            bump(g_wo_inv, inv);
            const uint32_t n = g_wof_n.load(std::memory_order_relaxed);
            g_wof[n % 32].store(p.frame << 16 | (inv < 65535 ? inv : 65535), std::memory_order_relaxed);
            g_wof_n.store(n + 1, std::memory_order_release);
        }
    }
    if (ok && p.hidk) {
        bump(g_w_hidk_frames);
        bump(g_w_hidk_draws, p.hidk);
        if (p.kf.miss[0] == 0) bump(g_w_hidk_within);
    }
    if (ok && p.hid) {
        const bool within = p.kf.miss[0] == 0;
        bump(g_w_hid_frames);
        for (uint32_t i = 0; i < p.hid_pn; ++i) bump(g_pair[p.hid_pair[i]].hid);
        bump(g_w_hid_draws, p.hid);
        if (within) bump(g_w_hid_within);
        const uint32_t n = g_wh_n.load(std::memory_order_relaxed);
        if (n < kWh) {  // (frame << 24 | within << 23 | draws)
            g_wh[n].store(p.frame << 24 | (within ? 1ull << 23 : 0) | (p.hid & 0x7fffffu), std::memory_order_relaxed);
            g_wh_n.store(n + 1, std::memory_order_release);
        }
    }
    for (uint32_t i = 0; i < p.wm_n; ++i) {
        const WMiss& m = p.wm[i];
        int plane = -1;
        const int c = ok ? classify(p, m, &plane) : kWUnatt;
        bump(g_wc[c]);
        if (m.pair >= 0) bump(g_pair[m.pair].why[c]);
        if (plane >= 0) bump(g_w_c1_plane[plane]);
        const Ent* e = ok && m.ent >= 0 && g_et[m.ent].gen == p.gen ? &g_et[m.ent] : nullptr;
        if (e) {
            bump(g_wc_kind[c][kind_of(e->called[1] ? e->ac[1] : e->ac[0])]);
            if (c == kWC2Lod || c == kWC2Geom || c == kWC2Open) {
                if (((e->ac[0] >> 10) & 7) != ((e->ac[1] >> 10) & 7)) bump(g_w_c2_lodbits);
            }
        }
        int key1 = -1;  // (this key, no VB, among the entity's kept pass-1 keys)
        if (e) {
            key1 = 0;
            for (uint32_t j = 0; j < e->nk; ++j)
                if (e->k[j] == m.k) key1 = 1;
            if (!key1 && e->k_over) key1 = -1;
            bump(g_w_key1[key1 + 1]);
        }
        const uint32_t n = g_wd_n.load(std::memory_order_relaxed);
        if (n < kWd) {  // (the first kWd since the reset: written once, then published)
            WDetail& d = g_wd[n];
            d = WDetail{};
            d.frame = p.frame;
            d.vs = m.pair >= 0 ? g_pair[m.pair].vs.load(std::memory_order_relaxed) : 0;
            d.ps = m.pair >= 0 ? g_pair[m.pair].ps.load(std::memory_order_relaxed) : 0;
            d.kv = m.kv;
            d.k = m.k;
            d.cause = c;
            d.seg = m.seg;
            d.bucket = -1;
            d.key1 = key1;
            d.a = m.a;
            if (e) {
                d.rec = e->rec;
                d.bucket = static_cast<int>(e->bucket);
                d.npso = e->npso;
                for (int q = 0; q < 2; ++q) {
                    d.called[q] = e->called[q];
                    d.n[q] = e->n[q];
                    d.empty[q] = e->empty[q];
                    d.ac[q] = e->ac[q];
                    d.ord[q] = e->ord[q];
                    d.src[q] = e->src[q];
                    d.s[q] = e->s[q];
                }
            }
            g_wd_n.store(n + 1, std::memory_order_release);
        }
    }
    if (p.wm_lost) bump(g_wc[kWLost], p.wm_lost);
}

// the frame's verdict (pass 2's e1, or a frame left unfinished): judged or not, then clean, single or gated
void finalize() {
    Pb& p = g_pb;
    if (p.st == 0) return;
    const Rec& r = p.r;
    if (p.kf.over) p.unj |= kUOver;
    uint32_t gates = p.gates;
    if (r.ip_draws && !r.ip_live) gates |= kGIp;  // (only a frame that draws an instprev pipeline needs the main InstPrev mirror)
    if (p.unj) {
        bump(g_f_unjudged);
        for (int b = 0; b < kUnjs; ++b)
            if (p.unj & (1u << b)) bump(g_unj[b]);
        bump(g_unj_draws, r.draws);
        nc_push(3, gates, p.unj, r, p.frame);
    } else {
        bump(g_f_judged);
        for (int i = 0; i < kReasons; ++i)
            if (r.reason[i]) bump(g_reason[i], r.reason[i]);
        for (int i = 0; i < kTwins; ++i)
            if (r.twin[i]) bump(g_twin[i], r.twin[i]);
        for (int i = 0; i < kCats; ++i)
            if (r.pred_cat[i]) bump(g_pred_cat[i], r.pred_cat[i]);
        bump(g_draws, r.draws);
        bump(g_empty, r.empty);
        bump(g_pred, r.pred);
        bump(g_indirect, r.indirect);
        bump(g_treerock, r.treerock);
        bump(g_ip_draws, r.ip_draws);
        bump(g_after_e1, r.after_e1);
        bump(g_draws2, r.draws2);
        bump(g_empty2, r.empty2);
        bump(g_indirect2, r.indirect2);
        bump(g_singles, r.singles);
        // (a failed gate makes every e1 draw of the frame a single)
        const uint32_t singles = gates ? r.draws - r.empty : r.singles;
        bump(g_hist[singles == 0 ? 0 : singles == 1 ? 1 : singles <= 4 ? 2 : singles <= 16 ? 3 : 4]);
        if (gates) {
            bump(g_f_gated);
            for (int b = 0; b < kGates; ++b)
                if (gates & (1u << b)) bump(g_gate[b]);
            nc_push(2, gates, 0, r, p.frame);
        } else if (!r.singles) {
            bump(g_f_clean);
        } else {
            bump(g_f_single);
            nc_push(1, 0, 0, r, p.frame);
        }
        // tier 1: pass 2's e1 keys within pass 1's (multisets), by segment and pair
        bump(g_k_frames);
        if (!p.kf.miss[0]) bump(g_k_within);
        if (!p.kf.miss[1]) bump(g_k_within_nvb);
        bump(g_k_miss, p.kf.miss[0]);
        bump(g_k_miss_nvb, p.kf.miss[1]);
        if (p.kf.rep) bump(g_k_rep);
        bump(g_k_n1, p.kf.n1);
        bump(g_k_n2, p.kf.n2);
        g_k_last_sum1.store(p.kf.sum1, std::memory_order_relaxed);
        g_k_last_sum2.store(p.kf.sum2, std::memory_order_relaxed);
        g_k_last_miss.store(p.kf.miss[0], std::memory_order_relaxed);
        for (int s = 0; s < kSegs; ++s)
            if (r.miss_seg[s]) bump(g_miss_seg[s], r.miss_seg[s]);
        for (uint32_t i = 0; i < r.miss_n && i < static_cast<uint32_t>(kMissList); ++i) bump(g_pair[r.miss_pair[i]].miss);
        bump(g_miss_lost, r.miss_lost);
        bump(g_k_rep_draws, r.rep_n);  // (pass 1's repeated keys, by pair)
        for (uint32_t i = 0; i < r.rep_used && i < static_cast<uint32_t>(kRepList); ++i) bump(g_pair[r.rep_pair[i]].rep);
        bump(g_rep_lost, r.rep_lost);
        for (int ps = 1; ps <= 2; ++ps)
            for (int d = 0; d < kDyns; ++d)
                if (r.dyn[ps][d]) bump(g_dyn[ps][d], r.dyn[ps][d]);
        why_frame(p);  // (tier 2: the misses' causes; returns at once when the frame recorded no entity markers)
    }
    if (g_mode.load(std::memory_order_relaxed) == 2 && main_inst::region_family()) {
        // (item 8e) "compare": the verdict to main_inst (it keeps it only inside this frame's pass-2 region: an unfinished frame's
        // verdict, pushed at the next pass 1's open, is counted stale there)
        main_inst::MrVerdict v;
        v.frame = p.frame;  // (S5: main_inst matches it with the frame main_inst::mr_open latched at this frame's pass-1 open)
        v.judged = p.unj == 0;
        v.within = p.kf.miss[0] == 0;
        v.rep = p.kf.rep;
        v.ctl = (gates & kGControl) != 0;
        v.gates = gates & ~kGControl;
        v.unj = p.unj;
        v.cats = r.cats;
        v.singles = r.singles;
        v.noord = r.noord;
        v.single_ords = r.single_ords;
        v.draws = r.pd1;
        main_inst::mr_verdict(v);
    }
    p.st = 0;
    p.in_e1 = false;
}

// ---- the dynamic vertex ring: every read of the game's memory under SEH (no C++ objects in these frames)
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
bool rd_bytes(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
inline uint64_t hmix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}
bool hash_bytes(uintptr_t p, uint32_t n, uint64_t* out) {  // (n <= kHashMax; read in place, nothing copied or allocated)
    uint64_t h = hmix(0x8d0dd1ull, n);
    __try {
        uint32_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t w;
            std::memcpy(&w, reinterpret_cast<const void*>(p + i), 8);
            h = hmix(h, w);
        }
        for (; i < n; ++i) h = hmix(h, *reinterpret_cast<const uint8_t*>(p + i));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *out = h;
    return true;
}
bool ring_install() {  // (install) the analysed build's code: the ring's array and the buffer class, else no draw is in the ring
    const uintptr_t b = anchors::base();
    uint8_t c1[7] = {}, c2[7] = {};
    if (!anchors::exe_matches() || anchors::relocated()) {
        std::snprintf(g_ring_why, sizeof(g_ring_why), "not the analysed build");
        return false;
    }
    if (!rd_bytes(b + kRvaRingRef, c1, sizeof(c1)) || c1[0] != 0x48 || c1[1] != 0x8b || c1[2] != 0x15 || !rd_bytes(b + kRvaBufVtRef, c2, sizeof(c2)) ||
        c2[0] != 0x48 || c2[1] != 0x8d || c2[2] != 0x05) {
        std::snprintf(g_ring_why, sizeof(g_ring_why), "FUN_14012e280 / FUN_140f8ea60 not as analysed");
        return false;
    }
    int32_t d1 = 0, d2 = 0;
    std::memcpy(&d1, c1 + 3, 4);
    std::memcpy(&d2, c2 + 3, 4);
    const uintptr_t slot = b + kRvaRingRef + 7 + static_cast<intptr_t>(d1), vt = b + kRvaBufVtRef + 7 + static_cast<intptr_t>(d2);
    uintptr_t map = 0, unmap = 0, setvb = 0;
    if (!rd(vt + 0x28, &map) || !rd(vt + 0x30, &unmap) || map != b + kRvaBufMap || unmap != b + kRvaBufUnmap) {
        std::snprintf(g_ring_why, sizeof(g_ring_why), "the buffer class's Map/Unmap (vt+0x28/+0x30) not as analysed");
        return false;
    }
    if (!anchors::present(anchors::Id::ImmCtxVtbl) || !rd(anchors::addr(anchors::Id::ImmCtxVtbl) + 0xc0, &setvb) || setvb != b + kRvaCtxSetVb) {
        std::snprintf(g_ring_why, sizeof(g_ring_why), "the immediate context's SetVertexBuffer (vt+0xc0) not as analysed");
        return false;
    }
    g_ring_slot = slot;
    g_buf_vt = vt;
    std::snprintf(g_ring_why, sizeof(g_ring_why), "ok (rva %#llx, vtable rva %#llx)", static_cast<unsigned long long>(slot - b),
                  static_cast<unsigned long long>(vt - b));
    return true;
}
void ring_refresh() {  // (the playback thread, pass 1's open) the ring's buffers as they are now
    g_rb_n = 0;
    if (!g_ring_slot) return;
    bump(g_rb_refresh);
    uintptr_t arr = 0;
    int32_t n = 0;
    if (!rd(g_ring_slot, &arr) || !rd(g_ring_slot + 8, &n) || !arr || n <= 0) {
        bump(g_rb_none);
        g_rb_last.store(0, std::memory_order_relaxed);
        return;
    }
    uint32_t mapped = 0;
    for (int i = 0; i < n && i < static_cast<int>(kRingMax); ++i) {
        uintptr_t w = 0, vt = 0, cpu = 0;
        int32_t kind = -1, usage = -1;
        uint64_t va = 0;
        uint32_t size = 0;
        if (!rd(arr + 8 * static_cast<uintptr_t>(i), &w) || !rd(w, &vt) || vt != g_buf_vt || !rd(w + 0x30, &kind) || !rd(w + 0x34, &usage) ||
            !rd(w + 0x40, &va) || !rd(w + 0x48, &size) || !rd(w + 0x18, &cpu) || kind != 0 || usage != 3 || size != kRingBytes || !va) {
            bump(g_rb_bad);
            continue;
        }
        g_rb[g_rb_n++] = RingBuf{va, va + size, cpu};
        if (cpu) ++mapped;
    }
    g_rb_last.store(g_rb_n, std::memory_order_relaxed);
    g_rb_mapped.store(mapped, std::memory_order_relaxed);
}
// a draw's two keys (main_inst keys::make's), the one with the vertex buffer remade when slot 0 lies in the ring: its address and
// size move between the passes while its bytes do not. Non-indexed draws hash their own vertices ([c, c + a) x stride, at most
// kHashMax bytes: the ring holds no per-instance stream); otherwise the ring's tag with the stride and the draw's span (no address)
int make_key(ID3D12GraphicsCommandList* cl, const void* pso, const d3d::DrawArgs& a, uint64_t* kv, uint64_t* k) {
    main_inst::keys::make(cl, pso, a, kv, k);
    if (!g_rb_n) return kDNone;
    uint64_t loc = 0;
    uint32_t size = 0, stride = 0;
    if (!pso::last_vb(cl, &loc, &size) || !loc) return kDNone;
    const RingBuf* rb = nullptr;
    for (unsigned i = 0; i < g_rb_n; ++i)
        if (loc >= g_rb[i].va && loc < g_rb[i].end) {
            rb = &g_rb[i];
            break;
        }
    if (!rb) return kDNone;
    pso::last_vb_stride(cl, &stride);
    const uint64_t first = a.indexed ? 0 : static_cast<uint64_t>(a.c) * stride, bytes = a.indexed ? 0 : static_cast<uint64_t>(a.a) * stride;
    int kind = kDTag;
    uint64_t h = 0;
    if (!a.indexed && stride && bytes && bytes <= kHashMax && rb->cpu && loc + first + bytes <= rb->end)
        kind = hash_bytes(rb->cpu + static_cast<uintptr_t>(loc - rb->va + first), static_cast<uint32_t>(bytes), &h) ? kDHash : kDFault;
    uint64_t x = hmix(*k, kind == kDHash ? 0xd7a0000000000001ull : 0xd7a0000000000002ull);  // (a fault keys as the tag does)
    x = hmix(x, (static_cast<uint64_t>(stride) << 32) | static_cast<uint32_t>(bytes));
    *kv = kind == kDHash ? hmix(x, h) : x;
    return kind;
}

// ---- tier 2, the render thread (body.cpp's DrawVisEntity hook through fcensus::entity: DVL's entity loop, buckets 0-1, inside e1)
constexpr uintptr_t kRvaIsVisible = 0x875030, kRvaIsVisible2 = 0x875190;  // rmcDrawable vt+0x68 (the vt+0xd call) and vt+0x60 (its twin)
constexpr uintptr_t kRvaDrawableVt = 0x1ceeca8;                            // rmcDrawable's vtable (vt+0x30 0x875370, +0x60, +0x68)
constexpr uintptr_t kRvaDrawableDraw = 0x875370;
constexpr uintptr_t kRvaFragSphereCall = 0xa28dbf;                         // FUN_140a28d10 (kind 1): call IsSphereVisible
constexpr uintptr_t kRvaIsSphereVisible = 0x130030;
uintptr_t g_isvis = 0;    // (install) RDR.exe's IsVisible (0: the sphere not read)
uint64_t g_eseq = 0;      // (the render thread's) the entity markers recorded so far: the ring's sequence
thread_local bool t_why = false;  // this pass's DVL call 1 records entity markers (its open saw "misses why on" and its marker went in)
thread_local int t_vpst = 0;      // this pass's planes: 0 not read yet, 1 read, -1 not readable (no sphere margins this pass)
thread_local float t_pl[3][16];   // [0] the near plane (4 floats), [1] the current side planes, [2] the eye's own side planes
bool why_install() {  // (install) the analysed build's code: rmcDrawable's vtable and IsVisible, the fragment draw's sphere call
    const uintptr_t b = anchors::base();
    if (!anchors::exe_matches() || anchors::relocated()) {
        std::snprintf(g_why_note, sizeof(g_why_note), "not the analysed build: C1 \"cull, unsplit\"");
        return false;
    }
    uintptr_t v30 = 0, v60 = 0, v68 = 0;
    uint8_t c[5] = {};
    int32_t rel = 0;
    if (!rd(b + kRvaDrawableVt + 0x30, &v30) || !rd(b + kRvaDrawableVt + 0x60, &v60) || !rd(b + kRvaDrawableVt + 0x68, &v68) ||
        v30 != b + kRvaDrawableDraw || v60 != b + kRvaIsVisible2 || v68 != b + kRvaIsVisible) {
        std::snprintf(g_why_note, sizeof(g_why_note), "rmcDrawable's vtable (rva %#llx) not as analysed: C1 \"cull, unsplit\"",
                      static_cast<unsigned long long>(kRvaDrawableVt));
        return false;
    }
    if (!rd_bytes(b + kRvaFragSphereCall, c, sizeof(c)) || c[0] != 0xe8) {
        std::snprintf(g_why_note, sizeof(g_why_note), "FUN_140a28d10's sphere call not as analysed: C1 \"cull, unsplit\"");
        return false;
    }
    std::memcpy(&rel, c + 1, 4);
    if (b + kRvaFragSphereCall + 5 + static_cast<intptr_t>(rel) != b + kRvaIsSphereVisible) {
        std::snprintf(g_why_note, sizeof(g_why_note), "FUN_140a28d10 does not call IsSphereVisible at 0x140a28dbf: C1 \"cull, unsplit\"");
        return false;
    }
    g_isvis = b + kRvaIsVisible;
    std::snprintf(g_why_note, sizeof(g_why_note), "ok (IsVisible rva %#llx, rmcDrawable vtable rva %#llx, the fragment draw's sphere)",
                  static_cast<unsigned long long>(kRvaIsVisible), static_cast<unsigned long long>(kRvaDrawableVt));
    return true;
}
bool read_planes(int pass) {  // (the pass's first entity) the current viewport's near and side planes, and the eye's own side planes
    uintptr_t vp = 0;
    if (!anchors::present(anchors::Id::ViewportCurrent) || !rd(anchors::addr(anchors::Id::ViewportCurrent), &vp) || !vp ||
        !rd_bytes(vp + 0x3d0, t_pl[0], 16) || !rd_bytes(vp + 0x390, t_pl[1], 64)) {
        bump(g_wr_vp_bad);
        return false;
    }
    if (pass == 1 && main_inst::uc_eye_planes(vp, t_pl[2])) bump(g_wr_eye_saved);  // (union-culled: the eye's own, saved at the swap)
    else std::memcpy(t_pl[2], t_pl[1], 64);
    return true;
}
// the sphere (r, centre) as the entity's draw-time cull reads it (Sph's comment), every read under SEH (no C++ objects here):
// SphSrc, or -1 a read faulted
int sphere_raw(uintptr_t rec, uintptr_t isvis, float* out, uint32_t* ac) {
    __try {
        *ac = *reinterpret_cast<const uint32_t*>(rec + 0xac);
        const uint32_t kind = (*ac >> 13) & 7;
        const uintptr_t d = *reinterpret_cast<const uintptr_t*>(rec + 0x80);
        if (!d) return kSNone;
        const float* m = nullptr;
        const float* lc = nullptr;
        float r = 0.0f;
        int src = kSNone;
        if (kind == 0 || kind == 2) {
            uintptr_t o = d;
            if (kind == 2) {  // (DrawVisEntity's kind 2: the vt+0xd call on the drawable's +0x08, else its +0x10)
                o = *reinterpret_cast<const uintptr_t*>(d + 8);
                if (!o) o = *reinterpret_cast<const uintptr_t*>(d + 0x10);
                if (!o) return kSNone;
            }
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(o);
            if (!vt || *reinterpret_cast<const uintptr_t*>(vt + 0x68) != isvis) return kSNone;  // (not the layout IsVisible reads)
            lc = reinterpret_cast<const float*>(o + 0x20);
            r = *reinterpret_cast<const float*>(o + 0x90);
            m = reinterpret_cast<const float*>(rec);
            src = kind ? kSDrawableSub : kSDrawable;
        } else if (kind == 1) {  // (FUN_140a28d10: *(rec+0x80)+0x80's matrix and bounds)
            // (the tier-2 review's SF2) the game's own gate first: FUN_140a28d10 returns before this chain when +8 and +0x10 are null
            if (!*reinterpret_cast<const uintptr_t*>(d + 8) && !*reinterpret_cast<const uintptr_t*>(d + 0x10)) return kSNone;
            const uintptr_t o = *reinterpret_cast<const uintptr_t*>(d + 0x80);
            if (!o) return kSNone;
            const uintptr_t a = *reinterpret_cast<const uintptr_t*>(o + 8);
            if (!a) return kSNone;
            const uintptr_t bd = *reinterpret_cast<const uintptr_t*>(a + 0x18);
            if (!bd) return kSNone;
            lc = reinterpret_cast<const float*>(bd + 0x50);
            r = *reinterpret_cast<const float*>(bd + 0x20);
            m = reinterpret_cast<const float*>(o + 0x20);
            src = kSFrag;
        } else {
            return kSNone;
        }
        const float x = lc[0], y = lc[1], z = lc[2];
        out[0] = r;
        for (int k = 0; k < 3; ++k) out[1 + k] = x * m[k] + y * m[4 + k] + z * m[8 + k] + m[12 + k];
        return src;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
inline float margin(const float* pl, const float* c, float r) { return pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] + pl[3] + r; }
inline bool finite_f(float v) { return v == v && v < 3.0e38f && v > -3.0e38f; }
// (the render thread) the entity's sphere against this pass's planes: SphSrc (kSNone: no sphere)
uint32_t sphere(uintptr_t rec, Sph* s, uint32_t* ac) {
    *ac = 0;
    if (!g_isvis || t_vpst <= 0) {
        rd(rec + 0xac, ac);
        return kSNone;
    }
    float o[4] = {};
    const int src = sphere_raw(rec, g_isvis, o, ac);
    if (src < 0) {
        bump(g_wr_sph_fault);
        return kSNone;
    }
    if (src == kSNone) return kSNone;
    if (!(o[0] > 0.0f && o[0] < 1.0e6f) || !finite_f(o[1]) || !finite_f(o[2]) || !finite_f(o[3])) {
        bump(g_wr_sph_bad);
        return kSNone;
    }
    s->r = o[0];
    for (int k = 0; k < 3; ++k) s->c[k] = o[1 + k];
    s->nearm = margin(t_pl[0], s->c, s->r);
    for (int i = 0; i < 4; ++i) {
        s->side[i] = margin(t_pl[1] + 4 * i, s->c, s->r);
        s->eside[i] = margin(t_pl[2] + 4 * i, s->c, s->r);
    }
    return static_cast<uint32_t>(src);
}

// ---- tier 2, the playback thread: the entity markers (the ring's next entry: this draw's entity from here)
int ent_slot(uint64_t rec, uint32_t bucket, uint32_t gen) {
    unsigned i = static_cast<unsigned>(main_inst::keys::digest(rec ^ (static_cast<uint64_t>(bucket) << 56) ^ 0x2545f4914f6cdd1dull));
    for (unsigned q = 0; q < kEntProbe; ++q, ++i) {
        Ent& e = g_et[i & (kEntSlots - 1)];
        if (e.gen != gen) {
            std::memset(&e, 0, sizeof(e));
            e.gen = gen;
            e.rec = rec;
            e.bucket = bucket;
            e.ord[0] = e.ord[1] = ~0u;
            return static_cast<int>(i & (kEntSlots - 1));
        }
        if (e.rec == rec && e.bucket == bucket) return static_cast<int>(i & (kEntSlots - 1));
    }
    return -1;
}
void pb_ent() {
    Pb& p = g_pb;
    const bool why = p.pass == 1 ? p.why1 : p.pass == 2 ? p.why2 : false;
    if (!why || !p.in_e1 || p.st == 0 || GetCurrentThreadId() != p.tid) {  // (a marker outside its region pass: the cursor is not this one's)
        bump(g_wp_stray);
        if (why && p.st != 0) p.wbad |= 1;
        return;
    }
    const uint64_t t0 = __rdtsc();
    const uint64_t idx = p.ecur++;
    const ERing& e = g_ering[idx & (kERing - 1)];
    const uint64_t s1 = e.stamp.load(std::memory_order_acquire);
    const uint64_t rec = e.rec.load(std::memory_order_relaxed), fr = e.frame.load(std::memory_order_relaxed);
    const uint32_t meta = e.meta.load(std::memory_order_relaxed), ac = e.ac.load(std::memory_order_relaxed);
    uint32_t fb[kSphF];
    for (int k = 0; k < kSphF; ++k) fb[k] = e.f[k].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t s2 = e.stamp.load(std::memory_order_relaxed);
    p.ent = -1;
    if (s1 != idx + 1 || s2 != s1) {  // (overwritten, or not yet written: the cursor and the ring disagree from here)
        bump(g_wp_lost);
        p.wbad |= 2;
        return;
    }
    const uint32_t bucket = meta & 0xff, pass = (meta >> 8) & 0xff, src = (meta >> 16) & 0xff;
    if (static_cast<int>(pass) != p.pass || fr != p.frame || bucket > 1) {
        bump(g_wp_mismatch);
        p.wbad |= 4;
        return;
    }
    const int s = ent_slot(rec, bucket, p.gen);
    bump(g_wp_ents[p.pass]);
    if (s < 0) {  // (the table full: this entity's draws unattributed, the frame's other entities still are)
        bump(g_wp_full);
        return;
    }
    Ent& en = g_et[s];
    const int i = p.pass - 1;
    ++en.called[i];
    if (en.ord[i] == ~0u) en.ord[i] = p.eord;
    ++p.eord;
    en.ac[i] = ac;
    en.src[i] = src < kSrcs ? src : kSNone;
    if (en.src[i] != kSNone) std::memcpy(&en.s[i], fb, sizeof(Sph));
    p.ent = s;
    bump(g_wp_tsc, __rdtsc() - t0);
}
void cb_ent(void*) { pb_ent(); }

uint32_t new_gen() {  // (a frame's entity table: a new generation, the table cleared once at its wrap)
    if (++g_et_gen == 0) {
        std::memset(g_et, 0, sizeof(g_et));
        g_et_gen = 1;
    }
    return g_et_gen;
}

// ---- the markers' playback side
void pb_open(int pass, int slot) {
    Pb& p = g_pb;
    const RingSlot& rs = g_ring[slot];
    const uint32_t fl = rs.flags.load(std::memory_order_acquire);
    const uint64_t fr = rs.frame.load(std::memory_order_relaxed);
    bump(g_p_opens[pass]);
    if (pass == 1) {
        if (p.st != 0) {  // (the last frame never finished: no pass 2 open, or its e1 never reached)
            p.unj |= p.st == 4 ? kUNoE1 : kUNoP2;
            finalize();
        }
        if (g_reset_req.exchange(false, std::memory_order_relaxed)) reset_all();
        p.st = 1;
        p.pass = 1;
        p.in_e1 = true;
        p.e1_1 = false;
        p.seg = 0;
        p.slot = slot;
        p.frame = fr;
        p.tid = GetCurrentThreadId();
        p.unj = (fl & kSt1) ? 0u : kUOrder;
        // (item 8e) "compare": this frame's number to main_inst, inside its pass-1 region (its open marker played just before this one)
        if (g_mode.load(std::memory_order_relaxed) == 2 && main_inst::region_family()) main_inst::mr_open(fr);
        p.gates = rs.gates.load(std::memory_order_relaxed);
        if (!main_inst::pb_union_culled()) p.gates |= kGUcull;  // (the union marker, recorded before this one, played in this region)
        pso::LatchView lv;
        pso::latch_view(&lv);  // (main_inst's open latched it just before)
        if (!lv.allow) p.gates |= kGAllow;
        p.r = Rec{};
        p.kf = main_inst::keys::Frame{};
        main_inst::keys::begin(g_keys);
        ring_refresh();  // (the dynamic vertex ring's buffers for this frame's keys)
        // (tier 2) this frame's entities: a new generation (the table cleared once at its wrap), pass 1's cursor
        p.gen = new_gen();
        p.why1 = (fl & kFWhy1) != 0;
        p.why2 = false;
        p.wbad = 0;
        p.ecur = rs.ebase[0].load(std::memory_order_relaxed);
        p.ent = -1;
        p.eord = 0;
        p.wm_n = p.wm_lost = 0;
        p.hid = p.hid_pn = p.hidk = 0;
        g_observing.store(true, std::memory_order_relaxed);
        return;
    }
    if (p.st == 0) {  // (pass 2 without this frame's pass 1: keyed nothing to judge; an orphan, unjudged at its e1)
        bump(g_p_orphan2);
        p.r = Rec{};
        p.kf = main_inst::keys::Frame{};
        p.unj = kUOrder;
        p.gates = 0;
        p.frame = fr;
        p.tid = GetCurrentThreadId();
        p.why1 = false;  // (tier 2: no pass 1 here, nothing to attribute)
        p.wbad = 0;
        p.wm_n = p.wm_lost = 0;
        p.hid = p.hid_pn = p.hidk = 0;
        p.gen = new_gen();
    } else if (p.st == 1 || p.st == 4) {  // (pass 1 never closed, or a second pass 2)
        p.unj |= kUOrder;
    }
    p.why2 = (fl & kFWhy2) != 0;  // (tier 2: pass 2's cursor from its own open)
    p.ecur = rs.ebase[1].load(std::memory_order_relaxed);
    p.ent = -1;
    p.eord = 0;
    if (!p.e1_1) p.unj |= kUNoE1;
    if (p.slot != slot || p.frame != fr || !(fl & kSt2) || p.tid != GetCurrentThreadId() || main_inst::pb_frame_order()) p.unj |= kUOrder;
    p.gates |= rs.gates.load(std::memory_order_relaxed) & kGCut;  // (the walk cut, decided between the passes: read at pass 2's open)
    p.st = 4;
    p.pass = 2;
    p.in_e1 = true;
    p.seg = 0;
    p.slot = slot;
    g_observing.store(true, std::memory_order_relaxed);
}
template <int P, int S>
void cb_open(void*) {
    pb_open(P, S);
}
using Fn = void (*)(void*);
constexpr Fn kOpenCb[2][8] = {{&cb_open<1, 0>, &cb_open<1, 1>, &cb_open<1, 2>, &cb_open<1, 3>, &cb_open<1, 4>, &cb_open<1, 5>, &cb_open<1, 6>, &cb_open<1, 7>},
                              {&cb_open<2, 0>, &cb_open<2, 1>, &cb_open<2, 2>, &cb_open<2, 3>, &cb_open<2, 4>, &cb_open<2, 5>, &cb_open<2, 6>, &cb_open<2, 7>}};
void pb_e1(int pass) {
    Pb& p = g_pb;
    if (p.pass != pass || !p.in_e1 || p.st == 0 || GetCurrentThreadId() != p.tid) {
        bump(g_p_e1_lost);
        if (p.pass == pass && p.st != 0) p.unj |= kUOrder;
        return;
    }
    bump(g_p_e1[pass]);
    p.in_e1 = false;
    // (item 8f) "compare": e1's end to main_inst ([Stereo] MainRegionE1Compare: pass 2's C2 owed; returns at once off an e1f frame)
    if (g_mode.load(std::memory_order_relaxed) == 2 && main_inst::region_family()) main_inst::mr_e1_end(pass);
    const uint32_t fl = g_ring[p.slot].flags.load(std::memory_order_acquire);
    if (pass == 1) {
        p.st = 2;
        p.e1_1 = true;
        if (fl & kFOther) p.unj |= kUOther;  // (an instprev bind or a PrevWorld write on another thread inside the walk)
        p.r.ip_live = inst_prev::main_live();  // (the main walk's open marker played before this region's draws)
        return;
    }
    if (fl & kFMark) p.unj |= kUMarker;
    if (fl & kFEnt) p.wbad |= 8;  // (tier 2: an entity marker of this frame not recorded: its misses unattributed, the frame still judged)
    finalize();  // (pass 2's e1 reached: the frame's verdict)
}
template <int P>
void cb_e1(void*) {
    pb_e1(P);
}
void pb_seg(int k) {
    Pb& p = g_pb;
    if (!p.in_e1 || p.st == 0 || GetCurrentThreadId() != p.tid) {
        bump(g_p_seg_lost);
        return;
    }
    p.seg = k;
    p.ent = -1;  // (tier 2: a post's draws are no entity's; a bucket's entities start with their own marker)
    bump(g_p_seg[k]);
}
template <int K>
void cb_seg(void*) {
    pb_seg(K);
}
constexpr Fn kSegCb[kSegs] = {&cb_seg<0>, &cb_seg<1>, &cb_seg<2>, &cb_seg<3>, &cb_seg<4>};

// ---- the per-draw classification (pass 1's e1)
void single(Rec& r, int reason, int pair, bool pred) {
    const int cat = kReason[reason].cat;
    ++r.singles;
    ++r.reason[reason];
    r.cats |= 1u << cat;
    if (pred) ++r.pred_cat[cat];
    const int ord = pair_ord(pair, false);  // (item 8e: the frame's singles by ordinal)
    if (ord >= 0) r.single_ords |= 1ull << ord;
    if (pair >= 0) {
        bump(g_pair[pair].singles);
        bump(g_pair[pair].cat[cat]);
        for (int k = 0; k < r.tp_used; ++k)
            if (r.tp_pair[k] == pair) {
                ++r.tp_n[k];
                return;
            }
        if (r.tp_used < kTop) {
            r.tp_pair[r.tp_used] = pair;
            r.tp_n[r.tp_used++] = 1;
        }
    }
}
int no_twin_reason(uint32_t st) {
    switch (st & 0xff) {
        case rg::kRsUnknown: return kRNtStream;
        case rg::kRsQueued: return kRNtQueued;
        case rg::kRsReadyNotBuilt: return kRNtReady;
        case rg::kRsVsRefused: return kRNtVsRefused;
        case rg::kRsPsRefused: return kRNtPsRefused;
        case rg::kRsHeld:
            return (st & rg::kKWvp) ? kRNtHeldWvp : (st & rg::kKIp) ? kRNtHeldIp : (st & rg::kKE2i) ? kRNtHeldE2i : (st & rg::kKPw) ? kRNtHeldPw : kRNtHeld;
        case rg::kRsBuildFailed: return kRNtBuildFailed;
        case rg::kRsShape: return kRNtShape;
        default: return kRNtNoRule;
    }
}
int ip_reason() {
    if (!inst_prev::main_live()) {
        switch (inst_prev::main_why()) {
            case 1: return kRIpNoVp2;
            case 2: return kRIpNoTaa;
            case 3: return kRIpNoRows;
            case 4: return kRIpBroken;
            default: return kRIpOff;
        }
    }
    switch (inst_prev::draw_no_why()) {
        case inst_prev::kNoBad: return kRIpBad;
        case inst_prev::kNoOverK: return kRIpOverK;
        case inst_prev::kNoCap: return kRIpCap;
        case inst_prev::kNoFault: return kRIpFault;
        case inst_prev::kNoAsEye1: return kRIpAsEye1;
        case inst_prev::kNoNocb: return kRIpNocb;
        case inst_prev::kNoCtx: return kRIpCtx;
        default: return kRIpNoMark;
    }
}
int twin_kind(uint32_t st) {
    const bool pw = (st & rg::kKPw) != 0, e2i = (st & rg::kKE2i) != 0, ip = (st & rg::kKIp) != 0;
    if (ip && pw) return kTOther;
    if (ip) return e2i ? kTIpE2i : kTIp;
    if (pw) return e2i ? kTPwE2i : kTPw;
    return e2i ? kTE2i : kTPlain;
}
void draw_pass1(Pb& p, ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& a, const void* pso, DrawInfo* out) {
    Rec& r = p.r;
    ++r.draws;
    const bool empty = a.a == 0 || a.b == 0;  // (PrepassCutEmptyDraws' rule: nothing drawn in either eye; exempt)
    pso::RegionInfo ri;
    uint32_t v = 0;
    if (pso) v = pso::region_verdict(pso, cl, &ri);
    const int pair = pair_of(ri.vs, ri.ps);
    const int ord = pair_ord(pair, !empty);  // (item 8e: given at the pair's first non-empty draw here, never in pass 2)
    if (out) *out = DrawInfo{GetCurrentThreadId() == p.tid, empty, ord, ri.flags, ri.fmt};  // (item 8e: main_inst's listing)
    if (pair >= 0) {
        Pair& e = g_pair[pair];
        e.segs.store(e.segs.load(std::memory_order_relaxed) | (1u << p.seg), std::memory_order_relaxed);
        e.st.store(ri.st, std::memory_order_relaxed);
        bump(empty ? e.empty : e.d1);
    }
    Ent* en = p.ent >= 0 ? &g_et[p.ent] : nullptr;  // (tier 2: this draw's entity; null when off or in a post)
    if (empty) {
        ++r.empty;
        if (en) ++en->empty[0];
        return;
    }
    const bool pred = d3d::thread_predicated();
    if (pred) {
        ++r.pred;
        if (pair >= 0) bump(g_pair[pair].pred);
    }
    if (ord >= 0) {  // (item 8e: the frame's draws by ordinal, for main_inst's verdict)
        if (r.pd1[ord] < 0xffff) ++r.pd1[ord];
        g_ord_st[ord].store(ri.st, std::memory_order_relaxed);
    } else {
        ++r.noord;
    }
    if ((ri.vs >> 32) == 0xd420020bull) ++r.treerock;  // (the treerock VS: its near trees through the main InstPrev mirror)
    if ((ri.st & 0xff) == rg::kRsMade && (ri.st & rg::kKIp)) ++r.ip_draws;
    int reason = -1;
    const uint32_t code = v & 0xff;
    if (!pso) {
        reason = kRPsoList;
    } else if (code == rg::kVUnknownPso) {
        reason = kRPsoSlot;
    } else if (!main_inst::region_bind_ok(cl)) {
        reason = kRBind;
    } else {
        switch (code) {
            case rg::kVNoTwin: reason = no_twin_reason(ri.st); break;
            case rg::kVKindWvp: reason = kRKindWvp; break;
            case rg::kVGateE2i: {
                pso::LatchView lv;
                pso::latch_view(&lv);
                const int w = lv.e2i == 1 ? 7 : lv.e2i_why;  // (the record says yes: the images not lent on this list)
                reason = w == 1 ? kRE2iNoKind : w == 2 ? kRE2iNoLive : w == 3 ? kRE2iNoGuard : w == 4 ? kRE2iNoFresh : w == 5 ? kRE2iNoLend
                         : w == 7 ? kRE2iNotLent : kRE2iUndecided;
                break;
            }
            case rg::kVGatePw: {
                pso::LatchView lv;
                pso::latch_view(&lv);
                reason = lv.pw ? kRPwDraw : kRPwFrame;
                break;
            }
            case rg::kVGateIp: reason = ip_reason(); break;
            case rg::kVTwin:
                if (g_control.load(std::memory_order_relaxed) == 1 && (ri.st & rg::kKPw)) reason = kRPwControl;  // (M2's control)
                break;
            default: reason = kRNtNoRule; break;
        }
    }
    if (reason >= 0) {
        single(r, reason, pair, pred);
    } else {
        ++r.twin[twin_kind(ri.st)];
        if (pred) ++r.pred_cat[kCTwin];
        if (pair >= 0) bump(g_pair[pair].cat[kCTwin]);
    }
    uint64_t kv = 0, k = 0;  // (tier 1: the key, counted in; a repeated one named by its pair)
    ++r.dyn[1][make_key(cl, pso, a, &kv, &k)];
    if (main_inst::keys::count(g_keys, p.kf, 1, kv, k) & 4) {
        ++r.rep_n;
        if (pair >= 0 && r.rep_used < static_cast<uint32_t>(kRepList)) r.rep_pair[r.rep_used++] = pair;
        else ++r.rep_lost;
    }
    if (en) {  // (tier 2: the entity's pass-1 draws, its pipelines and keys)
        ++en->n[0];
        uint32_t i = 0;
        while (i < en->npso && en->pso[i] != pso) ++i;
        if (i == en->npso) {
            if (en->npso < kEntPso) en->pso[en->npso++] = pso;
            else en->pso_over = true;
        }
        i = 0;
        while (i < en->nk && en->k[i] != k) ++i;
        if (i == en->nk) {
            if (en->nk < kEntKeys) en->k[en->nk++] = k;
            else en->k_over = true;
        }
    } else if (p.why1 && (p.seg == 1 || p.seg == 3)) {
        bump(g_wp_noent_draws);
    }
}
void draw_pass2(Pb& p, ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& a, const void* pso, DrawInfo* out) {
    Rec& r = p.r;
    ++r.draws2;
    const bool empty = a.a == 0 || a.b == 0;
    pso::RegionInfo ri;
    if (pso) pso::region_lookup(pso, &ri);
    const int pair = pair_of(ri.vs, ri.ps);
    if (out) *out = DrawInfo{GetCurrentThreadId() == p.tid, empty, pair_ord(pair, false), ri.flags, ri.fmt};
    if (pair >= 0) {
        Pair& e = g_pair[pair];
        e.segs.store(e.segs.load(std::memory_order_relaxed) | (0x100u << p.seg), std::memory_order_relaxed);
        if (!empty) bump(e.d2);
    }
    Ent* en = p.ent >= 0 ? &g_et[p.ent] : nullptr;  // (tier 2: this draw's entity)
    if (empty) {
        ++r.empty2;
        if (en) ++en->empty[1];
        return;
    }
    if (en) {
        // (item 8e's bisect step 0) the keys are a multiset: two entities drawing one key hide a miss of either. By entity, pass 1 is
        // complete by now, so a pass-2 draw past its entity's pass-1 count is a miss whatever the keys say
        if (++en->n[1] > en->n[0]) {
            ++p.hid;
            if (pair >= 0 && p.hid_pn < 64) p.hid_pair[p.hid_pn++] = static_cast<int16_t>(pair);
        }
    } else if (p.why2 && (p.seg == 1 || p.seg == 3)) bump(g_wp_noent_draws);
    uint64_t kv = 0, k = 0;  // (tier 1: the key, counted out; a miss named by its segment and pair)
    ++r.dyn[2][make_key(cl, pso, a, &kv, &k)];
    if (en && en->called[0] && !en->k_over) {  // (step 0: a key its own entity never drew in pass 1, another's surplus aside)
        bool kept = false;
        for (uint32_t j = 0; j < en->nk; ++j)
            if (en->k[j] == k) kept = true;
        if (!kept) ++p.hidk;
    }
    if (main_inst::keys::count(g_keys, p.kf, 2, kv, k) & 1) {
        if (out) out->miss = true;  // (item 8g: a pass-2 draw pass 1 did not draw, by count: the handover draws it)
        ++r.miss_seg[p.seg];
        if (pair >= 0 && r.miss_n < static_cast<uint32_t>(kMissList)) r.miss_pair[r.miss_n++] = pair;
        else ++r.miss_lost;
        if (p.why1 || p.why2) {  // (tier 2: classified at the frame's verdict, when the entity's pass-2 draws are all in)
            if (p.wm_n < static_cast<uint32_t>(kWMiss)) p.wm[p.wm_n++] = WMiss{p.ent, p.seg, pair, pso, kv, k, a};
            else ++p.wm_lost;
        }
    }
}

// ---- kv, dump, status (the command thread)
const char* kinds_text(uint32_t k, char* b, size_t n) {
    if (k & pso::kE1Refused) return "refused";
    std::snprintf(b, n, "%s%s%s%s%s", (k & rg::kKIp) ? "ip" : "", (k & rg::kKPw) ? "pw" : "", ((k & (rg::kKIp | rg::kKPw)) && (k & rg::kKE2i)) ? "+" : "",
                  (k & rg::kKE2i) ? "e2i" : "", (k & rg::kKWvp) ? "wvp" : "");
    if (!b[0]) std::snprintf(b, n, "plain");
    return b;
}
const char* state_text(uint32_t st, char* b, size_t n) {
    const uint32_t s = st & 0xff;
    if (s == rg::kRsMade) return kinds_text(st & rg::kKindMask, b, n);
    std::snprintf(b, n, "%s", s < rg::kRsStates ? kStateName[s] : "?");
    return b;
}
int top_cat(const Pair& e) {
    int best = -1;
    uint64_t n = 0;
    for (int c = 0; c < kCats; ++c)
        if (e.cat[c].load(std::memory_order_relaxed) > n) {
            n = e.cat[c].load(std::memory_order_relaxed);
            best = c;
        }
    return best;
}
std::vector<int> pairs_by(bool rep) {  // (the used pairs, most pass-1+2 draws first, or most repeated keys (none: left out); the command thread)
    // (the crash review's MF1) the counts read once into a snapshot, then sorted: the playback thread moves them meanwhile, and a
    // comparator whose values change breaks std::sort's ordering (its unguarded insertion loop can run past the vector)
    std::vector<std::pair<uint64_t, int>> s;
    for (unsigned i = 0; i < kPairs; ++i) {
        if (!g_pair[i].used.load(std::memory_order_acquire)) continue;
        const uint64_t n = rep ? g_pair[i].rep.load() : g_pair[i].d1.load() + g_pair[i].d2.load();
        if (!rep || n) s.push_back({n, static_cast<int>(i)});
    }
    std::sort(s.begin(), s.end(), [](const std::pair<uint64_t, int>& a, const std::pair<uint64_t, int>& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<int> v;
    v.reserve(s.size());
    for (const auto& e : s) v.push_back(e.second);
    return v;
}
std::vector<int> pairs_by_draws() { return pairs_by(false); }
std::vector<int> pairs_by_rep() { return pairs_by(true); }
void cost(int pass, double* cyc, double* ms) {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    const double secs = f.QuadPart ? static_cast<double>(q.QuadPart - g_qpc0.load()) / static_cast<double>(f.QuadPart) : 0.0;
    const double hz = secs > 0.0 ? static_cast<double>(__rdtsc() - g_tsc0.load()) / secs : 0.0;
    const double n = static_cast<double>(g_tsc_n[pass].load()), frames = static_cast<double>(g_p_opens[pass].load());
    *cyc = n > 0.0 ? static_cast<double>(g_tsc[pass].load()) / n : 0.0;
    *ms = hz > 0.0 && frames > 0.0 ? 1000.0 * static_cast<double>(g_tsc[pass].load()) / hz / frames : 0.0;
}
void add(std::string& out, bool& cut, const char* fmt, ...) {
    char b[2400];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= static_cast<int>(sizeof(b))) cut = true;
    out += b;
}
// ---- tier 2's output (the command thread)
double tsc_hz() {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    const double secs = f.QuadPart ? static_cast<double>(q.QuadPart - g_qpc0.load()) / static_cast<double>(f.QuadPart) : 0.0;
    return secs > 0.0 ? static_cast<double>(__rdtsc() - g_tsc0.load()) / secs : 0.0;
}
uint64_t w_named() {
    uint64_t n = 0;
    for (int c = 0; c < kCauses; ++c)
        if (cause_named(c)) n += g_wc[c].load();
    return n;
}
uint64_t w_total() {
    uint64_t n = 0;
    for (const Count& c : g_wc) n += c.load();
    return n;
}
std::vector<int> pairs_by_why() {  // (the pairs with tier-2 misses, most first; a snapshot sorted, as pairs_by)
    std::vector<std::pair<uint64_t, int>> s;
    for (unsigned i = 0; i < kPairs; ++i) {
        if (!g_pair[i].used.load(std::memory_order_acquire)) continue;
        uint64_t n = 0;
        for (const Count& c : g_pair[i].why) n += c.load();
        if (n) s.push_back({n, static_cast<int>(i)});
    }
    std::sort(s.begin(), s.end(), [](const std::pair<uint64_t, int>& a, const std::pair<uint64_t, int>& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<int> v;
    v.reserve(s.size());
    for (const auto& e : s) v.push_back(e.second);
    return v;
}
const char* seg_name(int s) { return s >= 0 && s < kSegs ? kSegName[s] : "?"; }
int ord_of(uint32_t o) { return o == ~0u ? -1 : static_cast<int>(o); }
// one miss's detail: frame:cause:segment:bucket:vs/ps:rec:ordinals p1/p2:calls p1/p2:e1 draws p1/p2:empty p1/p2:rec+0xac p1/p2:the
// sphere's source p1/p2:the key among the entity's pass-1 keys:pass-1 pipelines:the draw's args; then each pass's sphere (radius,
// centre) and its margins (n.c + d + r; < 0: outside that plane by that much): pass 1 near (eye 1), the union's 4 side planes, eye
// 1's own 4 (eye); pass 2 near (eye 2), eye 2's 4 side planes
std::string detail(const WDetail& d) {
    char b[900];
    int n = std::snprintf(b, sizeof(b), "%llu:%s:%s:b%d:%08x/%08x:rec=%llx:ord=%d/%d:calls=%u/%u:draws=%u/%u:empty=%u/%u:ac=%x/%x:src=%s/%s:key1=%d:pso1=%u:"
                          "args=%s,%u,%u,%u,%d",
                          static_cast<unsigned long long>(d.frame), d.cause >= 0 && d.cause < kCauses ? kCauseName[d.cause] : "?", seg_name(d.seg), d.bucket,
                          static_cast<unsigned>(d.vs >> 32), static_cast<unsigned>(d.ps >> 32), static_cast<unsigned long long>(d.rec), ord_of(d.ord[0]),
                          ord_of(d.ord[1]), d.called[0], d.called[1], d.n[0], d.n[1], d.empty[0], d.empty[1], d.ac[0], d.ac[1],
                          kSrcName[d.src[0] < kSrcs ? d.src[0] : 0], kSrcName[d.src[1] < kSrcs ? d.src[1] : 0], d.key1, d.npso, d.a.indexed ? "i" : "n",
                          d.a.a, d.a.b, d.a.c, d.a.d);
    for (int q = 0; q < 2; ++q) {
        if (n < 0 || n >= static_cast<int>(sizeof(b))) break;
        const size_t left = sizeof(b) - static_cast<size_t>(n);
        if (d.src[q] == kSNone) {
            n += std::snprintf(b + n, left, ":p%d=-", q + 1);
            continue;
        }
        const Sph& s = d.s[q];
        n += std::snprintf(b + n, left, ":p%d=r%.3g,c%.4g,%.4g,%.4g,near%.3g,side%.3g,%.3g,%.3g,%.3g", q + 1, s.r, s.c[0], s.c[1], s.c[2], s.nearm,
                           s.side[0], s.side[1], s.side[2], s.side[3]);
        if (q == 0 && n > 0 && n < static_cast<int>(sizeof(b)))
            n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), ",eye%.3g,%.3g,%.3g,%.3g", s.eside[0], s.eside[1], s.eside[2], s.eside[3]);
    }
    return b;
}
std::string why_kv() {  // "prepass mr why": tier 2's counts, by cause and pair, and the first misses' details
    std::string out;
    out.reserve(8000);
    bool cut = false;
    const double hz = tsc_hz(), f1 = static_cast<double>(g_r_opens[1].load()), f2 = static_cast<double>(g_p_opens[1].load());
    const double ents = static_cast<double>(g_wr_ents.load() + g_wr_failed.load());
    const uint64_t tot = w_total(), nam = w_named();
    add(out, cut,
        "why=%d why_armed=%d why_sph=%d mode=%d w_frames=%llu w_frames_bad=%llu w_frames_miss=%llu w_total=%llu w_named=%llu w_unnamed=%llu "
        "k_miss=%llu wr_ents=%llu wr_failed=%llu wr_sph_fault=%llu wr_sph_bad=%llu wr_vp_bad=%llu wr_eye_saved=%llu wp_ents1=%llu wp_ents2=%llu "
        "wp_stray=%llu wp_lost=%llu wp_mismatch=%llu wp_full=%llu wp_noent_draws=%llu c2_lodbits=%llu key1_unkept=%llu key1_no=%llu key1_yes=%llu "
        "cost_wr_cyc_per_ent=%.0f cost_wr_ms_per_frame=%.4f cost_wp_ms_per_frame=%.4f",
        g_why.load(), why_on() ? 1 : 0, g_why_sph ? 1 : 0, g_mode.load(), u(g_w_frames), u(g_w_frames_bad), u(g_w_frames_miss),
        static_cast<unsigned long long>(tot), static_cast<unsigned long long>(nam), static_cast<unsigned long long>(tot - nam), u(g_k_miss), u(g_wr_ents),
        u(g_wr_failed), u(g_wr_sph_fault), u(g_wr_sph_bad), u(g_wr_vp_bad), u(g_wr_eye_saved), u(g_wp_ents[1]), u(g_wp_ents[2]), u(g_wp_stray),
        u(g_wp_lost), u(g_wp_mismatch), u(g_wp_full), u(g_wp_noent_draws), u(g_w_c2_lodbits), u(g_w_key1[0]), u(g_w_key1[1]), u(g_w_key1[2]),
        ents > 0.0 ? static_cast<double>(g_wr_tsc.load()) / ents : 0.0,
        hz > 0.0 && f1 > 0.0 ? 1000.0 * static_cast<double>(g_wr_tsc.load()) / hz / f1 : 0.0,
        hz > 0.0 && f2 > 0.0 ? 1000.0 * static_cast<double>(g_wp_tsc.load()) / hz / f2 : 0.0);
    for (int i = 0; i < kSrcs; ++i) add(out, cut, " wr_src_%s=%llu", kSrcName[i], u(g_wr_src[i]));
    for (int i = 0; i < 4; ++i) add(out, cut, " w_bad_%s=%llu", kWBadName[i], u(g_w_bad[i]));
    for (int c = 0; c < kCauses; ++c) add(out, cut, " w_%s=%llu", kCauseName[c], u(g_wc[c]));
    for (int i = 0; i < 4; ++i) add(out, cut, " c1_plane%d=%llu", i, u(g_w_c1_plane[i]));
    for (int c = 0; c < kCauses; ++c) {  // (the entity kinds, (rec+0xac >> 13) & 7, per cause: k0:k1:...:k7)
        uint64_t t = 0;
        for (const Count& x : g_wc_kind[c]) t += x.load();
        if (!t) continue;
        add(out, cut, " wk_%s=", kCauseName[c]);
        for (int k = 0; k < 8; ++k) add(out, cut, "%s%llu", k ? ":" : "", u(g_wc_kind[c][k]));
    }
    const std::vector<int> v = pairs_by_why();  // (vs:ps, then cause=count for each of its causes; the top 16)
    add(out, cut, " wpairs=%zu", v.size());
    for (size_t k = 0; k < v.size() && k < 16; ++k) {
        const Pair& e = g_pair[v[k]];
        add(out, cut, " wp%02zu=%08x:%08x", k, static_cast<unsigned>(e.vs.load() >> 32), static_cast<unsigned>(e.ps.load() >> 32));
        for (int c = 0; c < kCauses; ++c)
            if (e.why[c].load()) add(out, cut, ":%s=%llu", kCauseName[c], u(e.why[c]));
    }
    add(out, cut, " w_hid_frames=%llu w_hid_within=%llu w_hid_draws=%llu w_hidk_frames=%llu w_hidk_within=%llu w_hidk_draws=%llu", u(g_w_hid_frames),
        u(g_w_hid_within), u(g_w_hid_draws), u(g_w_hidk_frames), u(g_w_hidk_within), u(g_w_hidk_draws));
    {
        int nh = 0;
        for (unsigned i = 0; i < kPairs; ++i) {
            const Pair& e = g_pair[i];
            if (!e.hid.load(std::memory_order_relaxed)) continue;
            if (nh++ >= 16) continue;
            add(out, cut, " whp%02d=%08x:%08x:%llu", nh - 1, static_cast<unsigned>(e.vs.load() >> 32), static_cast<unsigned>(e.ps.load() >> 32), u(e.hid));
        }
        add(out, cut, " whp_n=%d", nh);
        add(out, cut, " wo_frames=%llu wo_inv=%llu wo_ents=%llu wof=", u(g_wo_frames), u(g_wo_inv), u(g_wo_ents));
        {
            const uint32_t nf = g_wof_n.load(std::memory_order_acquire), f0 = nf > 32 ? nf - 32 : 0;
            for (uint32_t i = f0; i < nf; ++i) {
                const uint64_t w = g_wof[i % 32].load(std::memory_order_relaxed);
                add(out, cut, "%s%llu:%u", i > f0 ? "," : "", static_cast<unsigned long long>(w >> 16), static_cast<unsigned>(w & 0xffff));
            }
            if (!nf) add(out, cut, "-");
            const uint32_t no = g_wod_n.load(std::memory_order_acquire);
            for (uint32_t i = 0; i < no && i < kWo; ++i) {
                if (out.size() > 6000) {
                    add(out, cut, " wod_shown=%u", i);
                    break;
                }
                const WO& o = g_wod[i];
                add(out, cut, " wod%02u=%llu", i, static_cast<unsigned long long>(o.frame));
                for (int s = 0; s < 2; ++s)
                    add(out, cut, "%sb%u:%08x:%08x:o%u/%u:r%.3g:c%.1f,%.1f,%.1f", s ? "|" : ":", o.bucket[s], o.vs[s], o.ps[s], o.ord0[s], o.ord1[s],
                        o.r[s], o.c[s][0], o.c[s][1], o.c[s][2]);
            }
        }
        add(out, cut, " wx_frames=%llu wx_li_frames=%llu", u(g_wx_frames), u(g_wx_li_frames));
        for (int c = 0; c < kXCats; ++c) add(out, cut, " wx_%s=%llu wx_%s_draws=%llu", kXName[c], u(g_wx[c]), kXName[c], u(g_wx_draws[c]));
        const uint32_t nf = g_wxf_n.load(std::memory_order_acquire);  // (the last kWxf, oldest first: frame:lod:in)
        add(out, cut, " wxf=");
        const uint32_t f0 = nf > kWxf ? nf - kWxf : 0;
        for (uint32_t i = f0; i < nf; ++i) {
            const uint64_t w = g_wxf[i % kWxf].load(std::memory_order_relaxed);
            add(out, cut, "%s%llu:%u:%u", i > f0 ? "," : "", static_cast<unsigned long long>(w >> 16), static_cast<unsigned>(w >> 8 & 0xff),
                static_cast<unsigned>(w & 0xff));
        }
        if (!nf) add(out, cut, "-");
        const uint32_t nd = g_wxd_n.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < nd && i < kWx; ++i) {
            if (out.size() > 7600) {  // (the wd details after these keep their room)
                add(out, cut, " wxd_shown=%u", i);
                break;
            }
            const WX& x = g_wxd[i];
            add(out, cut, " wxd%02u=%llu:%s:k%u:%08x:%08x:n%u/%u:e%u/%u:c%u:ac%x/%x:r%.3g:c%.1f,%.1f,%.1f:m%.3g", i, static_cast<unsigned long long>(x.frame),
                kXName[x.cat < kXCats ? x.cat : 0], x.kind, x.vs, x.ps, x.n0, x.n1, x.e0, x.e1, x.called1, x.ac0, x.ac1, x.r, x.c[0], x.c[1], x.c[2], x.m2);
        }
        const uint32_t n = g_wh_n.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n && i < kWh; ++i) {
            const uint64_t h = g_wh[i].load(std::memory_order_relaxed);
            add(out, cut, " whf%02u=%llu:%d:%u", i, static_cast<unsigned long long>(h >> 24), static_cast<int>(h >> 23 & 1), static_cast<unsigned>(h & 0x7fffff));
        }
    }
    const uint32_t nd = g_wd_n.load(std::memory_order_acquire);
    add(out, cut, " wd_n=%u", nd);
    for (uint32_t i = 0; i < nd && i < kWd; ++i) {  // (the reply kept under the test's 10 KB: the rest in dump, each named)
        const std::string x = detail(g_wd[i]);
        if (out.size() + x.size() + 48 > 9900) {
            add(out, cut, " wd_shown=%u", i);
            break;
        }
        add(out, cut, " wd%02u=%s", i, x.c_str());
    }
    if (cut) out += " kv_truncated=1";
    return out + " kv_end=1";
}
void why_dump(std::string& out, bool& cut) {  // (dump's tier-2 lines, each under the log's 2 KB)
    const uint64_t tot = w_total(), nam = w_named();
    add(out, cut, "[mr] tier 2 (misses why %s; the sphere: %s): frames %llu attributed, %llu not (stray %llu, lost %llu, mismatch %llu, unrecorded "
                  "%llu); misses classified %llu: named %llu, unnamed %llu; entity markers recorded %llu (failed %llu), played %llu / %llu (stray %llu, "
                  "lost %llu, mismatch %llu, table full %llu)\n",
        g_why.load() ? "on" : "off", g_why_note, u(g_w_frames), u(g_w_frames_bad), u(g_w_bad[0]), u(g_w_bad[1]), u(g_w_bad[2]), u(g_w_bad[3]),
        static_cast<unsigned long long>(tot), static_cast<unsigned long long>(nam), static_cast<unsigned long long>(tot - nam), u(g_wr_ents),
        u(g_wr_failed), u(g_wp_ents[1]), u(g_wp_ents[2]), u(g_wp_stray), u(g_wp_lost), u(g_wp_mismatch), u(g_wp_full));
    add(out, cut, "[mr] tier 2 by cause:");
    for (int c = 0; c < kCauses; ++c)
        if (g_wc[c].load()) add(out, cut, " %s %llu", kCauseName[c], u(g_wc[c]));
    add(out, cut, " | c1_side by plane %llu %llu %llu %llu | c2 with rec+0xac's LOD bits differing %llu\n", u(g_w_c1_plane[0]), u(g_w_c1_plane[1]),
        u(g_w_c1_plane[2]), u(g_w_c1_plane[3]), u(g_w_c2_lodbits));
    for (int i : pairs_by_why()) {
        const Pair& e = g_pair[i];
        add(out, cut, "[mr] tier 2 pair %016llx %016llx:", static_cast<unsigned long long>(e.vs.load()), static_cast<unsigned long long>(e.ps.load()));
        for (int c = 0; c < kCauses; ++c)
            if (e.why[c].load()) add(out, cut, " %s %llu", kCauseName[c], u(e.why[c]));
        add(out, cut, "\n");
    }
    const uint32_t nd = g_wd_n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < nd && i < kWd; ++i) add(out, cut, "[mr] tier 2 miss %u: %s\n", i, detail(g_wd[i]).c_str());
}
std::string kv() {
    std::string out;
    out.reserve(9000);
    bool cut = false;
    const uint64_t judged = g_f_judged.load();
    add(out, cut,
        "mode=%d control=%d segs=%d region_table=%d frames_judged=%llu frames_clean=%llu frames_single=%llu frames_gated=%llu frames_unjudged=%llu "
        "clean_pct=%.4f unj_draws=%llu hist0=%llu hist1=%llu hist2_4=%llu hist5_16=%llu hist17=%llu draws=%llu empty=%llu predicated=%llu "
        "indirect=%llu singles=%llu after_e1=%llu treerock=%llu ip_draws=%llu draws2=%llu empty2=%llu indirect2=%llu",
        g_mode.load(), g_control.load(), g_segs ? 1 : 0, pso::region_on() ? 1 : 0, u(g_f_judged), u(g_f_clean), u(g_f_single), u(g_f_gated),
        u(g_f_unjudged), judged ? 100.0 * static_cast<double>(g_f_clean.load()) / static_cast<double>(judged) : 0.0, u(g_unj_draws), u(g_hist[0]),
        u(g_hist[1]), u(g_hist[2]), u(g_hist[3]), u(g_hist[4]), u(g_draws), u(g_empty), u(g_pred), u(g_indirect), u(g_singles), u(g_after_e1),
        u(g_treerock), u(g_ip_draws), u(g_draws2), u(g_empty2), u(g_indirect2));
    for (int i = 0; i < kUnjs; ++i) add(out, cut, " unj_%s=%llu", kUnjName[i], u(g_unj[i]));
    for (int i = 0; i < kGates; ++i) add(out, cut, " gate_%s=%llu", kGateName[i], u(g_gate[i]));
    for (int i = 0; i < kReasons; ++i) add(out, cut, " s_%s=%llu", kReason[i].name, u(g_reason[i]));
    for (int i = 0; i < kTwins; ++i) add(out, cut, " t_%s=%llu", kTwinName[i], u(g_twin[i]));
    for (int i = 0; i < kCats; ++i) add(out, cut, " pred_%s=%llu", kCatName[i], u(g_pred_cat[i]));
    add(out, cut,
        " k_frames=%llu k_within=%llu k_within_nvb=%llu k_miss=%llu k_miss_nvb=%llu k_rep=%llu k_n1=%llu k_n2=%llu k_last_sum1=%llu "
        "k_last_sum2=%llu k_last_miss=%llu miss_lost=%llu",
        u(g_k_frames), u(g_k_within), u(g_k_within_nvb), u(g_k_miss), u(g_k_miss_nvb), u(g_k_rep), u(g_k_n1), u(g_k_n2),
        static_cast<unsigned long long>(g_k_last_sum1.load()), static_cast<unsigned long long>(g_k_last_sum2.load()),
        static_cast<unsigned long long>(g_k_last_miss.load()), u(g_miss_lost));
    for (int s = 0; s < kSegs; ++s) add(out, cut, " miss_%s=%llu seg_%s=%llu", kSegName[s], u(g_miss_seg[s]), kSegName[s], u(g_p_seg[s]));
    // the dynamic vertex ring's keys (judged frames' keyed draws by pass and Dyn), the ring as last read, pass 1's repeated keys
    add(out, cut, " ring_ok=%d ring_bufs=%u ring_mapped=%u ring_refresh=%llu ring_none=%llu ring_bad=%llu k_rep_draws=%llu rep_lost=%llu",
        g_ring_slot ? 1 : 0, g_rb_last.load(), g_rb_mapped.load(), u(g_rb_refresh), u(g_rb_none), u(g_rb_bad), u(g_k_rep_draws), u(g_rep_lost));
    for (int ps = 1; ps <= 2; ++ps)
        for (int d = 1; d < kDyns; ++d) add(out, cut, " k_dyn_%s%d=%llu", kDynName[d], ps, u(g_dyn[ps][d]));
    double c1 = 0, m1 = 0, c2 = 0, m2 = 0;
    cost(1, &c1, &m1);
    cost(2, &c2, &m2);
    add(out, cut,
        " r_opens1=%llu r_opens2=%llu r_open_failed=%llu r_e1_1=%llu r_e1_2=%llu r_e1_failed=%llu r_segs=%llu r_seg_failed=%llu r_unpaired2=%llu "
        "fade_ungated=%llu fade_unread=%llu p_opens1=%llu p_opens2=%llu p_e1_1=%llu p_e1_2=%llu p_e1_lost=%llu p_seg_lost=%llu p_closes1=%llu "
        "p_closes2=%llu p_orphan2=%llu pairs_full=%llu cost_p1_cyc_per_draw=%.0f cost_p2_cyc_per_draw=%.0f cost_p1_ms_per_frame=%.4f "
        "cost_p2_ms_per_frame=%.4f",
        u(g_r_opens[1]), u(g_r_opens[2]), u(g_r_open_failed), u(g_r_e1[1]), u(g_r_e1[2]), u(g_r_e1_failed), u(g_r_segs), u(g_r_seg_failed),
        u(g_r_unpaired2), u(g_r_fade_ungated), u(g_r_fade_unread), u(g_p_opens[1]), u(g_p_opens[2]), u(g_p_e1[1]), u(g_p_e1[2]), u(g_p_e1_lost),
        u(g_p_seg_lost), u(g_p_closes[1]), u(g_p_closes[2]), u(g_p_orphan2), u(g_pair_full), c1, c2, m1, m2);
    // the pair table: counts, then the top 24 (vs:ps:pass-1 draws:pass-2 draws:segments:8a's kind:the slot's:the top class:singles:misses:
    // repeated keys)
    const std::vector<int> v = pairs_by_draws();
    const pso::E1Pair* e1 = nullptr;
    const int ne1 = pso::e1_pairs(&e1);
    std::vector<bool> drawn(static_cast<size_t>(ne1), false);
    int not8a = 0;
    for (int i : v) {
        const int x = g_pair[i].exp1.load() - 1;
        if (x >= 0 && x < ne1 && g_pair[i].d1.load()) drawn[static_cast<size_t>(x)] = true;
        if (x < 0 && g_pair[i].d1.load()) ++not8a;
    }
    int n8a = 0;
    for (bool b : drawn) n8a += b ? 1 : 0;
    add(out, cut, " pairs_used=%zu pairs_not_in_8a=%d pairs_8a_drawn=%d pairs_8a_not_drawn=%d", v.size(), not8a, n8a, ne1 - n8a);
    for (size_t k = 0; k < v.size() && k < 24; ++k) {  // (the top 24: the reply under the test's 10 KB with main_* appended; dump has all)
        const Pair& e = g_pair[v[k]];
        const int x = e.exp1.load() - 1;
        char eb[24], sb[24];
        const int tc = top_cat(e);
        add(out, cut, " pair%02zu=%08x:%08x:%llu:%llu:%x:%s:%s:%s:%llu:%llu:%llu", k, static_cast<unsigned>(e.vs.load() >> 32), static_cast<unsigned>(e.ps.load() >> 32),
            u(e.d1), u(e.d2), e.segs.load(), x >= 0 && x < ne1 ? kinds_text(e1[x].kind, eb, sizeof(eb)) : "none", state_text(e.st.load(), sb, sizeof(sb)),
            tc >= 0 ? kCatName[tc] : "-", u(e.singles), u(e.miss), u(e.rep));
    }
    // the pairs with the most repeated pass-1 keys (vs:ps:repeated:pass-1 draws), the top 6
    const std::vector<int> rv = pairs_by_rep();
    for (size_t k = 0; k < rv.size() && k < 6; ++k) {
        const Pair& e = g_pair[rv[k]];
        add(out, cut, " rep_top%zu=%08x:%08x:%llu:%llu", k, static_cast<unsigned>(e.vs.load() >> 32), static_cast<unsigned>(e.ps.load() >> 32), u(e.rep),
            u(e.d1));
    }
    // the last 8 frames that were not clean (frame:1 single/2 gated/3 unjudged:gates:unjudged:classes:singles:top pair:its singles)
    const uint32_t nx = g_nc_next.load();
    for (uint32_t k = 0; k < 8 && k < nx && k < kNc; ++k) {
        const Nc& n = g_nc[(nx - 1 - k) % kNc];
        add(out, cut, " nc%u=%llu:%u:%x:%x:%x:%u:%08x:%08x:%u", k, static_cast<unsigned long long>(n.frame.load()), n.kind.load(), n.gates.load(),
            n.unj.load(), n.cats.load(), n.singles.load(), static_cast<unsigned>(n.vs.load() >> 32), static_cast<unsigned>(n.ps.load() >> 32),
            n.top_n.load());
    }
    // (tier 2's totals; "prepass mr why" has the rest)
    add(out, cut, " why=%d w_frames=%llu w_frames_bad=%llu w_total=%llu w_named=%llu", g_why.load(), u(g_w_frames), u(g_w_frames_bad),
        static_cast<unsigned long long>(w_total()), static_cast<unsigned long long>(w_named()));
    out += inst_prev::main_kv();  // (the main InstPrev mirror's main_*: empty fields when not installed)
    if (cut) out += " kv_truncated=1";
    return out + " kv_end=1";
}
std::string dump() {
    std::string out;
    bool cut = false;
    const uint64_t judged = g_f_judged.load();
    add(out, cut, "[mr] frames judged %llu: clean %llu (%.4f%%), single %llu, gated %llu; unjudged %llu:", u(g_f_judged), u(g_f_clean),
        judged ? 100.0 * static_cast<double>(g_f_clean.load()) / static_cast<double>(judged) : 0.0, u(g_f_single), u(g_f_gated), u(g_f_unjudged));
    for (int i = 0; i < kUnjs; ++i) add(out, cut, " %s %llu", kUnjName[i], u(g_unj[i]));
    add(out, cut, "\n[mr] gates failed (frames):");
    for (int i = 0; i < kGates; ++i) add(out, cut, " %s %llu", kGateName[i], u(g_gate[i]));
    add(out, cut, "\n[mr] singles by reason (judged frames' draws):");
    for (int i = 0; i < kReasons; ++i)
        if (g_reason[i].load()) add(out, cut, " %s %llu", kReason[i].name, u(g_reason[i]));
    add(out, cut, "\n[mr] twins by kind:");
    for (int i = 0; i < kTwins; ++i) add(out, cut, " %s %llu", kTwinName[i], u(g_twin[i]));
    add(out, cut, " | draws %llu, empty %llu, predicated %llu, indirect %llu, treerock %llu | misses %llu (frames within %llu of %llu)\n", u(g_draws),
        u(g_empty), u(g_pred), u(g_indirect), u(g_treerock), u(g_k_miss), u(g_k_within), u(g_k_frames));
    add(out, cut, "[mr] the dynamic vertex ring: %s; buffers %u (mapped %u) at the last pass 1 open, refreshes %llu (none %llu, bad %llu); keyed by "
                  "its bytes pass 1 %llu / pass 2 %llu, by its tag %llu / %llu, a read faulted %llu / %llu\n",
        g_ring_why, g_rb_last.load(), g_rb_mapped.load(), u(g_rb_refresh), u(g_rb_none), u(g_rb_bad), u(g_dyn[1][kDHash]), u(g_dyn[2][kDHash]),
        u(g_dyn[1][kDTag]), u(g_dyn[2][kDTag]), u(g_dyn[1][kDFault]), u(g_dyn[2][kDFault]));
    add(out, cut, "[mr] pass 1's repeated keys (judged frames): %llu draws in %llu frames (not named %llu); by pair:", u(g_k_rep_draws), u(g_k_rep),
        u(g_rep_lost));
    const std::vector<int> rv = pairs_by_rep();  // (the top 40: the line under the log's 2 KB; each pair's own line below has its count)
    for (size_t k = 0; k < rv.size() && k < 40; ++k)
        add(out, cut, " %08x/%08x %llu;", static_cast<unsigned>(g_pair[rv[k]].vs.load() >> 32), static_cast<unsigned>(g_pair[rv[k]].ps.load() >> 32),
            u(g_pair[rv[k]].rep));
    add(out, cut, "%s\n", rv.size() > 40 ? " (more: the pairs below)" : "");
    const pso::E1Pair* e1 = nullptr;
    const int ne1 = pso::e1_pairs(&e1);
    std::vector<bool> drawn(static_cast<size_t>(ne1), false);
    add(out, cut, "[mr] pairs (vs ps: pass 1 draws / pass 2 draws, segments p1|p2, 8a's kind, the slot's, by class, singles, misses):\n");
    for (int i : pairs_by_draws()) {
        const Pair& e = g_pair[i];
        const int x = e.exp1.load() - 1;
        if (x >= 0 && x < ne1 && e.d1.load()) drawn[static_cast<size_t>(x)] = true;
        char eb[24], sb[24], segs[96] = "";
        size_t sn = 0;
        const uint32_t sg = e.segs.load();
        for (int half = 0; half < 2; ++half) {  // (pass 1's segments | pass 2's)
            for (int s = 0; s < kSegs; ++s)
                if (sg & (1u << (s + 8 * half)))
                    sn += static_cast<size_t>(std::snprintf(segs + sn, sizeof(segs) - sn, "%s%s", sn && segs[sn - 1] != '|' ? "," : "", kSegName[s]));
            if (!half) sn += static_cast<size_t>(std::snprintf(segs + sn, sizeof(segs) - sn, "|"));
        }
        add(out, cut, "  %016llx %016llx %llu / %llu %s 8a %s%s%s slot %s:", static_cast<unsigned long long>(e.vs.load()),
            static_cast<unsigned long long>(e.ps.load()), u(e.d1), u(e.d2), segs, x >= 0 && x < ne1 ? kinds_text(e1[x].kind, eb, sizeof(eb)) : "NOT IN 8a",
            x >= 0 && x < ne1 ? " " : "", x >= 0 && x < ne1 ? e1[x].name : "", state_text(e.st.load(), sb, sizeof(sb)));
        for (int c = 0; c < kCats; ++c)
            if (e.cat[c].load()) add(out, cut, " %s %llu", kCatName[c], u(e.cat[c]));
        add(out, cut, "; singles %llu, misses %llu, repeated keys %llu, empty %llu, predicated %llu\n", u(e.singles), u(e.miss), u(e.rep), u(e.empty),
            u(e.pred));
    }
    {  // (item 8e: the session's pair ordinals, moved here from kv; the line under the log's 2 KB: 64 x 21 bytes)
        const int no = g_ord_n.load(std::memory_order_acquire);
        add(out, cut, "[mr] item 8e's pair ordinals (at the first non-empty pass-1 e1 draw): %d given of %d, %llu refused past 63:", no, kOrds,
            u(g_ord_over));
        for (int i = 0; i < no && i < kOrds; ++i)
            add(out, cut, " %d %08x/%08x;", i, static_cast<unsigned>(g_ord_vs[i].load() >> 32), static_cast<unsigned>(g_ord_ps[i].load() >> 32));
        add(out, cut, "\n");
    }
    add(out, cut, "[mr] 8a's pairs NOT MEASURED (never drawn in pass 1's e1 since the reset):");
    for (int i = 0; i < ne1; ++i)
        if (!drawn[static_cast<size_t>(i)]) add(out, cut, " %08x/%08x %s;", e1[i].vs, e1[i].ps, e1[i].name);
    add(out, cut, "\n[mr] the last %u frames not clean (frame, kind, gates, unjudged, classes, singles, top pair):\n", kNc);
    const uint32_t nx = g_nc_next.load();
    for (uint32_t k = 0; k < kNc && k < nx; ++k) {
        const Nc& n = g_nc[(nx - 1 - k) % kNc];
        const uint32_t kind = n.kind.load(), g = n.gates.load(), uj = n.unj.load(), cats = n.cats.load();
        add(out, cut, "  %llu %s", static_cast<unsigned long long>(n.frame.load()), kind == 1 ? "single" : kind == 2 ? "gated" : "unjudged");
        for (int b = 0; b < kGates; ++b)
            if (g & (1u << b)) add(out, cut, " gate:%s", kGateName[b]);
        for (int b = 0; b < kUnjs; ++b)
            if (uj & (1u << b)) add(out, cut, " unj:%s", kUnjName[b]);
        for (int b = 0; b < kCats; ++b)
            if (cats & (1u << b)) add(out, cut, " %s", kCatName[b]);
        add(out, cut, " singles %u, top %016llx %016llx (%u)\n", n.singles.load(), static_cast<unsigned long long>(n.vs.load()),
            static_cast<unsigned long long>(n.ps.load()), n.top_n.load());
    }
    why_dump(out, cut);
    add(out, cut, "[mr] %s\n[mr] instprev%s", pso::region_status().c_str(), inst_prev::main_kv().c_str());
    if (cut) out += "\n[mr] (a line was cut)";
    return out;
}
std::string status() {
    const uint64_t judged = g_f_judged.load();
    char b[1200];
    std::snprintf(b, sizeof(b),
                  "mr %s%s%s: frames judged %llu, clean %llu (%.3f%%), single %llu, gated %llu, unjudged %llu | draws %llu (empty %llu, predicated "
                  "%llu, indirect %llu), singles %llu, twins plain %llu pw %llu e2i %llu pw_e2i %llu ip %llu ip_e2i %llu | misses %llu in %llu frames "
                  "(within %llu) | markers: opens %llu / %llu (failed %llu), e1 %llu / %llu, segments %llu (failed %llu)%s | misses why %s: "
                  "classified %llu, named %llu, frames attributed %llu (not %llu)",
                  g_mode.load() == 2 ? "compare" : g_mode.load() ? "observe" : "off", g_control.load() ? " (control nopw)" : "", g_observing.load() ? " (a region open)" : "", u(g_f_judged),
                  u(g_f_clean), judged ? 100.0 * static_cast<double>(g_f_clean.load()) / static_cast<double>(judged) : 0.0, u(g_f_single), u(g_f_gated),
                  u(g_f_unjudged), u(g_draws), u(g_empty), u(g_pred), u(g_indirect), u(g_singles), u(g_twin[kTPlain]), u(g_twin[kTPw]), u(g_twin[kTE2i]),
                  u(g_twin[kTPwE2i]), u(g_twin[kTIp]), u(g_twin[kTIpE2i]), u(g_k_miss), u(g_k_frames), u(g_k_within), u(g_r_opens[1]), u(g_r_opens[2]),
                  u(g_r_open_failed), u(g_r_e1[1]), u(g_r_e1[2]), u(g_r_segs), u(g_r_seg_failed),
                  g_segs ? "" : " | no segment tags ([Stereo] RouteFCensus=1 not installed)", g_why.load() ? "on" : "off",
                  static_cast<unsigned long long>(w_total()), static_cast<unsigned long long>(w_named()), u(g_w_frames), u(g_w_frames_bad));
    return b;
}
void set_mode(int mode) {  // 0 off, 1 observe, 2 compare (item 8e: observe's markers plus main_inst's listing)
    const bool observe = mode != 0;
    g_mode.store(mode, std::memory_order_relaxed);
    g_armed.store(g_installed.load() && observe, std::memory_order_relaxed);
    g_comparing.store(g_installed.load() && mode == 2, std::memory_order_relaxed);
    g_why_armed.store(g_installed.load() && observe && g_why.load() != 0, std::memory_order_relaxed);
    fcensus::set_segments(observe && g_segs);  // (fcensus's points run with its census and probe off)
    log::info("[mr] %s (test only: from the next DVL call 1)", mode == 2 ? "compare" : observe ? "observe" : "off");
}
}  // namespace

// ---- the render thread
void dvl_open(int pass, uint32_t mi) {
    t_rpass = 0;
    t_why = false;
    t_vpst = 0;
    if (!armed() || (pass != 1 && pass != 2)) return;
    const uint64_t f = state::presents.load(std::memory_order_relaxed);
    const int slot = static_cast<int>(f & 7);
    RingSlot& r = g_ring[slot];
    const bool why = why_on();  // (tier 2: latched for this pass; its cursor's start written before the open marker that reads it)
    r.ebase[pass - 1].store(g_eseq, std::memory_order_relaxed);
    if (pass == 1) {
        uint32_t g = 0;
        if (!(mi & kMiCascades)) g |= kGCascades;
        if (!(mi & kMiRows)) g |= kGRows;
        if (!(mi & kMiUc)) g |= kGUcull;
        if (!(mi & kMiP1vp)) g |= kGP1vp;
        if (mi & kMiCtl) g |= kGControl;
        bool e2i = false, pw = false;
        pso::auto_kinds(&e2i, &pw);
        if (!e2i) g |= kGAutoE2i;
        if (!pw) g |= kGAutoPw;
        if (!prevworld::gate_live()) g |= kGPwLive;
        const int fade = p2skip::fade_branch();  // (-2: Pass2FrontSkip not installed, its globals not read: not gated, counted)
        if (fade == -2) {
            bump(g_r_fade_ungated);
        } else if (fade != 0) {
            g |= kGFade;
            if (fade < 0) bump(g_r_fade_unread);
        }
        r.frame.store(f, std::memory_order_relaxed);
        r.gates.store(g, std::memory_order_relaxed);
        r.flags.store(kSt1 | (why ? kFWhy1 : 0u), std::memory_order_release);
        g_other0 = inst_prev::main_other();
        g_foreign0 = prevworld::foreign_walks();
    } else {
        uint32_t g = r.gates.load(std::memory_order_relaxed), fl = r.flags.load(std::memory_order_relaxed);
        if (r.frame.load(std::memory_order_relaxed) != f || !(fl & kSt1)) {  // (no pass 1 open recorded this frame)
            bump(g_r_unpaired2);
            r.frame.store(f, std::memory_order_relaxed);
            g = 0;
            fl = 0;
        }
        if (!prepass::walk_was_cut()) g |= kGCut;
        r.gates.store(g, std::memory_order_relaxed);
        r.flags.store((fl & ~kFWhy2) | kSt2 | (why ? kFWhy2 : 0u), std::memory_order_release);
    }
    if (!prepass::stream_marker(kOpenCb[pass - 1][slot])) {
        bump(g_r_open_failed);
        return;
    }
    bump(g_r_opens[pass]);
    t_rpass = pass;
    t_rslot = slot;
    t_why = why;
}
void dvl_close(int pass) {
    if (t_rpass == pass) t_rpass = 0;
}
void e1_end(int pass) {
    inst_prev::main_close(false);  // (the main InstPrev walk ends with e1: the composite and debris kind 3 stay out of it)
    if (t_rpass != pass || (pass != 1 && pass != 2)) return;
    RingSlot& r = g_ring[t_rslot];
    uint32_t fl = r.flags.load(std::memory_order_relaxed) | kStE1;
    if (pass == 1 && (inst_prev::main_other() != g_other0 || prevworld::foreign_walks() != g_foreign0)) fl |= kFOther;
    r.flags.store(fl, std::memory_order_release);
    if (!prepass::stream_marker(pass == 1 ? &cb_e1<1> : &cb_e1<2>)) {  // (the playback then never sees e1's end: unjudged, no_e1)
        bump(g_r_e1_failed);
        return;
    }
    bump(g_r_e1[pass]);
}
void segment(int seg) {
    if (!t_rpass || seg < 1 || seg >= kSegs) return;
    if (prepass::stream_marker(kSegCb[seg])) {
        bump(g_r_segs);
        return;
    }
    bump(g_r_seg_failed);
    RingSlot& r = g_ring[t_rslot];  // (the playback's cursor is wrong from here: the frame not judged)
    r.flags.store(r.flags.load(std::memory_order_relaxed) | kFMark, std::memory_order_release);
}
void entity(uintptr_t rec, uint32_t bucket, int pass) {
    if (!t_why || t_rpass != pass || bucket > 1 || !rec) return;
    const uint64_t t0 = __rdtsc();
    if (!t_vpst) t_vpst = read_planes(pass) ? 1 : -1;
    Sph s{};
    uint32_t ac = 0;
    const uint32_t src = sphere(rec, &s, &ac);
    bump(g_wr_src[src]);
    const uint64_t seq = g_eseq;
    ERing& e = g_ering[seq & (kERing - 1)];
    e.stamp.store(0, std::memory_order_relaxed);  // (the seqlock: the entry invalid while written)
    std::atomic_thread_fence(std::memory_order_release);
    e.rec.store(rec, std::memory_order_relaxed);
    e.frame.store(g_ring[t_rslot].frame.load(std::memory_order_relaxed), std::memory_order_relaxed);  // (the frame this region's open latched)
    e.meta.store(bucket | static_cast<uint32_t>(pass) << 8 | src << 16, std::memory_order_relaxed);
    e.ac.store(ac, std::memory_order_relaxed);
    uint32_t fb[kSphF];
    std::memcpy(fb, &s, sizeof(fb));
    for (int k = 0; k < kSphF; ++k) e.f[k].store(fb[k], std::memory_order_relaxed);
    e.stamp.store(seq + 1, std::memory_order_release);
    if (prepass::stream_marker(&cb_ent)) {
        g_eseq = seq + 1;
        bump(g_wr_ents);
    } else {  // (the entry is rewritten by the next one; the playback's cursor never moves for it, so the frame is marked)
        bump(g_wr_failed);
        RingSlot& r = g_ring[t_rslot];
        r.flags.store(r.flags.load(std::memory_order_relaxed) | kFEnt, std::memory_order_release);
    }
    bump(g_wr_tsc, __rdtsc() - t0);
}

// ---- the playback thread
void pb_close(int pass) {
    g_observing.store(false, std::memory_order_relaxed);
    Pb& p = g_pb;
    if (p.pass != pass) return;
    bump(g_p_closes[pass]);
    p.pass = 0;
    p.in_e1 = false;
    if (pass == 1) {
        if (p.st == 1) p.st = 3;  // (closed before its e1: pass 2's open marks the frame no_e1)
        else if (p.st == 2) p.st = 3;
        return;
    }
    if (p.st == 4) {  // (pass 2 closed before its e1)
        p.unj |= kUNoE1;
        finalize();
    }
}
void draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args, const void* pso, DrawInfo* out) {
    if (out) *out = DrawInfo{};
    Pb& p = g_pb;
    if (!p.in_e1 || p.st == 0) {
        if (p.pass == 1 && p.st) ++p.r.after_e1;  // (the region's draws past e1: not classified)
        return;
    }
    const uint64_t t0 = __rdtsc();
    if (p.pass == 1) draw_pass1(p, cl, args, pso, out);
    else draw_pass2(p, cl, args, pso, out);
    bump(g_tsc[p.pass], __rdtsc() - t0);
    bump(g_tsc_n[p.pass]);
}
int ord_find(uint32_t vs32, uint32_t ps32) {  // (item 8e; any thread)
    const int n = g_ord_n.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kOrds; ++i)
        if (static_cast<uint32_t>(g_ord_vs[i].load(std::memory_order_relaxed) >> 32) == vs32 &&
            static_cast<uint32_t>(g_ord_ps[i].load(std::memory_order_relaxed) >> 32) == ps32)
            return i;
    return -1;
}
bool ord_pair(int ord, uint64_t* vs, uint64_t* ps) {
    *vs = *ps = 0;
    if (ord < 0 || ord >= kOrds || ord >= g_ord_n.load(std::memory_order_acquire)) return false;
    *vs = g_ord_vs[ord].load(std::memory_order_relaxed);
    *ps = g_ord_ps[ord].load(std::memory_order_relaxed);
    return true;
}
int ord_count() { return g_ord_n.load(std::memory_order_acquire); }
bool pass1_clean(const char** why) {  // (item 8g; main_region.h)
    const Pb& p = g_pb;
    const char* w = nullptr;
    if (p.st != 4 || p.pass != 2 || !p.in_e1 || GetCurrentThreadId() != p.tid) {
        w = "8d: not inside pass 2's e1";
    } else {
        uint32_t gates = p.gates;
        if (p.r.ip_draws && !p.r.ip_live) gates |= kGIp;  // (finalize's rule)
        if (p.unj) w = "8d: the frame not judgeable";
        else if (gates & ~kGControl) w = "8d: a failed frame gate";
        else if (p.r.singles) w = "8d: a single in pass 1's e1";
        else if (p.r.noord) w = "8d: a pass-1 e1 draw of a pair without an ordinal";
        else if (p.kf.over) w = "8d: a key table full";
    }
    if (why) *why = w;
    return w == nullptr;
}
uint32_t ord_state(int ord) { return ord >= 0 && ord < kOrds ? g_ord_st[ord].load(std::memory_order_relaxed) : 0u; }
void indirect(ID3D12GraphicsCommandList* cl) {
    (void)cl;
    Pb& p = g_pb;
    if (!p.in_e1 || p.st == 0 || GetCurrentThreadId() != p.tid) return;
    if (p.pass == 1) {  // (never twinned: prepass's before_indirect rule)
        ++p.r.draws;
        ++p.r.indirect;
        single(p.r, kRIndirect, -1, d3d::thread_predicated());
    } else {
        ++p.r.indirect2;
    }
}

void install() {
    const int want = config::get_int("Stereo", "MainRegion", 0);
    if (want <= 0) return;
    if (want != 1) {
        log::warn("[mr] MainRegion=%d is not 1: off", want);
        return;
    }
    if (!main_inst::noting()) {
        log::warn("[mr] MainRegion needs [Stereo] MainInstanced=1 installed (its region's markers): off");
        return;
    }
    if (!main_inst::ucull_installed()) {
        log::warn("[mr] MainRegion needs [Stereo] MainUnionCull=1 (the draw keys' vertex buffers, the union cull's marker): off");
        return;
    }
    if (!config::get_bool("Stereo", "InstancedShadersAuto", false)) {
        log::warn("[mr] MainRegion needs [Stereo] InstancedShadersAuto=1 (the region table's twin states): off");
        return;
    }
    g_segs = fcensus::installed();
    if (!g_segs) log::warn("[mr] MainRegion: no segment tags without [Stereo] RouteFCensus=1 (its points; the census itself may be off): every draw \"pre\"");
    if (!ring_install())
        log::warn("[mr] MainRegion: the dynamic vertex ring not identified (%s): its draws keyed by their vertex buffer's address", g_ring_why);
    else
        log::info("[mr] MainRegion: the dynamic vertex ring %s: its draws keyed by their own vertices' bytes", g_ring_why);
    if (g_segs) {  // (tier 2 needs fcensus's entity point; the sphere needs the analysed code, else C1 reads "cull, unsplit")
        g_why_sph = why_install();
        log::info("[mr] MainRegion: tier 2 (\"prepass mr misses why on\"): the bounding sphere %s", g_why_note);
    } else {
        std::snprintf(g_why_note, sizeof(g_why_note), "no entity markers without [Stereo] RouteFCensus=1");
    }
    reset_all();
    g_installed = true;
    log::info("[mr] [Stereo] MainRegion on (test only, observe only): \"prepass mr observe\" classifies pass 1's e1 draws by 8f's would-be verdict "
              "(pso's region table, the frame latch, the per-draw gates), keys both passes' e1 (the misses census) and judges each frame; off until "
              "then; segment tags %s",
              g_segs ? "from fcensus's points" : "off");
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load())
        return "mr: not installed (start with [Stereo] MainRegion=1, MainInstanced=1, MainUnionCull=1 and InstancedShadersAuto=1; test only)";
    if (t.size() >= 3) {
        const std::string& w = t[2];
        if (w == "kv") return kv();
        if (w == "why") return why_kv();  // (tier 2's counts and the first misses' details)
        if (w == "dump") {
            const std::string d = dump();
            size_t a = 0;  // (the crash review's SF3: line by line, the log's line buffer is 2 KB)
            while (a < d.size()) {
                const size_t e = d.find('\n', a);
                const size_t n = (e == std::string::npos ? d.size() : e) - a;
                log::info("[mr] dump: %.*s", static_cast<int>(n), d.c_str() + a);
                a += n + 1;
            }
            return d;
        }
        if (w == "off" || w == "observe") {
            set_mode(w == "observe" ? 1 : 0);
        } else if (w == "compare") {  // (item 8e, test only: main_inst lists e1's draws from this classifier, MainFamily=region)
            if (!main_inst::region_family()) return "ERROR mr compare needs [Stereo] MainFamily=region installed (main_inst's region listing)";
            set_mode(2);
        } else if (w == "reset") {
            if (g_mode.load() || g_observing.load()) g_reset_req = true;  // (the playback thread's, at its next pass 1 open)
            else reset_all();
        } else if (w == "misses" && t.size() >= 5 && t[3] == "why" && (t[4] == "on" || t[4] == "off")) {
            // (tier 2, test only: the entity markers from the next DVL call 1; armed with observe)
            if (t[4] == "on" && !g_segs) return "ERROR refused: misses why needs fcensus's entity point ([Stereo] RouteFCensus=1 installed)";
            g_why = t[4] == "on" ? 1 : 0;
            g_why_armed.store(g_installed.load() && g_mode.load() != 0 && g_why.load() != 0, std::memory_order_relaxed);
            log::info("[mr] misses why %s (test only; the sphere: %s)", t[4].c_str(), g_why_note);
        } else if (w == "control" && t.size() >= 4 && (t[3] == "none" || t[3] == "nopw")) {
            g_control = t[3] == "nopw" ? 1 : 0;
            log::info("[mr] control %s (test only)", t[3].c_str());
        } else {
            return "ERROR usage: prepass mr off|observe|compare|reset|kv|dump|why|control none|nopw|misses why on|off";
        }
    }
    return status();
}

}  // namespace rdrvr::main_region
