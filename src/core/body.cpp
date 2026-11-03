#include "core/body.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>

#include "core/actions.h"
#include "core/aim.h"
#include "core/gestures.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/dual.h"
#include "core/dual_pass.h"
#include "core/fcensus.h"
#include "core/gpu_stages.h"
#include "core/hands.h"
#include "core/held_prop.h"
#include "core/xr.h"
#include "core/holster.h"
#include "core/round_draw.h"
#include "core/hooks.h"
#include "core/image_mem.h"
#include "core/log.h"
#include "core/main_instanced.h"
#include "core/pose.h"
#include "core/prepass_arrays.h"
#include "core/pso.h"
#include "core/reload.h"
#include "core/zone_rings.h"

namespace rdrvr::body {
namespace {

// 2026-10-29 (round 42: a simulator cycle fell to 4 Hz for good, the render thread 98% in VirtualQuery): a range inside
// RDR.exe's image, where the game keeps its objects, is first asked of the working set, page by page (image_mem.h:
// VirtualQuery's RegionSize walks the 1.4 GB .data's pages, about 10 ms a call once the game's writes join the runs).
// Pages not present, or anything else, as before.
bool readable(uintptr_t a, size_t n) {
    if (image_mem::fast_readable(a, n, PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
        return true;  // (the same protections as below)
    MEMORY_BASIC_INFORMATION mi{};
    return a && image_mem::query(a, &mi, image_mem::kBody) && mi.State == MEM_COMMIT &&
           (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
           !(mi.Protect & PAGE_GUARD) && a + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

// Checked read (VirtualQuery): for the once-a-frame lookups and the test commands, never in the draw hook.
template <class T>
bool rd(uintptr_t a, T* out) {
    if (!readable(a, sizeof(T))) return false;
    *out = *reinterpret_cast<const T*>(a);
    return true;
}

// A game-owned pointer the draw itself dereferences, read without VirtualQuery (a system call: called at every entity
// draw it cut first person to under 5 fps, headset round 5), with SEH as the backstop.
bool raw(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The player's object from its pool handle, as FUN_140230700 does; 0 when stale.
uintptr_t player_object(uint32_t* handle_out) {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.object) return 0;
    uint32_t h = static_cast<uint32_t>(st.object);
    if (handle_out) *handle_out = h;
    uintptr_t pool = 0;
    if (!rd(anchors::addr(anchors::Id::ObjectsPool), &pool) || !pool) return 0;
    uintptr_t slot = pool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    uint16_t gen = 0;
    uintptr_t obj = 0;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16) || !rd(slot, &obj)) return 0;
    return obj;
}

// The player's skeleton, by reads only, as FUN_140266bd0 then the code at 0x6bc0a0 find it: the actor slot (object
// +0xb0 handle in the actor pool), actor +0x50 -> +0x88 = x; when x +0x1b0 is set and x has a drawable (x +0x78 with
// flags 0x30000000 at +0xc, else x +0x98), the skeleton is *(x +0x88) +0x1a0.
uintptr_t player_skeleton() {
    uintptr_t obj = player_object(nullptr);
    if (!obj) return 0;
    uint16_t idx = 0, gen = 0, sgen = 0;
    uintptr_t pool = 0, actor = 0, comp = 0, x = 0, r9 = 0, r8 = 0, draw = 0, skel = 0;
    uint8_t flag = 0;
    uint32_t bits = 0;
    if (!rd(obj + 0xb0, &idx) || !rd(obj + 0xb2, &gen) || !rd(anchors::addr(anchors::Id::ActorPool), &pool) || !pool) return 0;
    if (!rd(pool + idx * 0x10 + 8, &sgen) || sgen != gen || !rd(pool + idx * 0x10, &actor) || !actor) return 0;
    if (!rd(actor + 0x50, &comp) || !comp || !rd(comp + 0x88, &x) || !x) return 0;
    if (!rd(x + 0x1b0, &flag) || !flag || !rd(x + 0x88, &r9) || !r9 || !rd(x + 0x78, &r8)) return 0;
    if (r8) {
        if (!rd(r8 + 0xc, &bits) || (bits & 0x30000000) != 0x30000000 || !rd(r8, &draw)) return 0;
    } else if (!rd(x + 0x98, &draw)) {
        return 0;
    }
    if (!draw || !rd(r9 + 0x1a0, &skel)) return 0;
    return skel;
}

// ---- settings ([Body]; the menu's Comfort tab)
std::mutex g_cfg_mutex;
bool g_cfg_hide = true;
float g_cfg_back = 0.04f;
int g_cfg_stance = 1;  // 0 the game's animation, 1 upper body upright, 2 the forward/back lean removed, 3 still
float g_cfg_height_tau = 0.35f;  // [Body] HeightSmoothing (the camera's eye height's): the still body's height follows it
bool g_cfg_still_feet = false;   // [Body] StillFeet (run 10 item 9 stage 2): the still body's feet where the game planted them
bool g_cfg_still_belt = true;    // [Body] StillBelt (run 10 item 9b): the belt and holster still with the body
bool g_cfg_follow = false;
bool g_cfg_lock = true;     // [Body] LockFacing: the drawn body faces the camera's heading
HandCfg g_hand_cfg;
float g_cfg_pitch = 2.0f;       // [Body] TorsoPitch: degrees added to the upright torso (positive leans it forward)
bool g_cfg_lock_torso = true;  // [Body] LockTorso: the upper body kept over the hips as at rest (horizontally)
bool g_cfg_auto_show = true;   // [Body] AutoShow: forearms and hands while crouching, in cover, riding or driving
std::atomic<uint32_t> g_actor_flags{0};  // the latest RdrvrActorState flags seen (status)
std::string g_cfg_up_bone = "spine00";  // the lowest spine bone: its subtree is the whole upper body (legs hang off pelvis)
float g_cfg_fwd_sign = 1.0f;            // 1: the model faces the root bone's -z (measured); -1 flips it
std::atomic<int> g_override{0};
std::atomic<bool> g_ik_test{false};  // "skel body iktest on": the right wrist's target 0.3 m in front of the head bone  // "skel body on|off|auto": 1 on, 2 off, 0 with the camera anchor
std::atomic<bool> g_active{false};

// ---- the rig: one skeleton data's bones, cached (render thread). Subtrees by the bone records' parent pointers.
struct Rig {
    uintptr_t data = 0;
    int count = 0;
    int head = -1, up = -1;
    std::vector<int> head_set, up_set;  // each bone and every bone under it, the bone first
    std::vector<int> attach;            // the bones props hang on (pistol, melee, rifle, thrower, rope, *_Attachment)
    int holster[body::kHolsterBones] = {-1, -1, -1, -1, -1, -1, -1};  // pistol (right hip), rifle (back), thrower, melee, the chest, the lower back, the lasso
    int piece[body::kPieces] = {-1, -1, -1, -1, -1};  // run 12 item 2: pistol, melee, thrower, rope, rifle
    std::vector<int> piece_set[body::kPieces];       // each piece bone's subtree (the bone first)
    int chain[4] = {-1, -1, -1, -1};    // spine00..spine03 (the upright turn's pivots)
    int arm[2] = {-1, -1}, elbow[2] = {-1, -1}, wrist[2] = {-1, -1};  // the IK chains (left, right)
    int armroll[2] = {-1, -1}, wristroll[2] = {-1, -1};               // the twist bones (upper arm, forearm)
    std::vector<int> clav_set[2];               // each clavicle's subtree (the shoulder and the whole arm)
    std::vector<uint8_t> in_elbow, in_wrist;    // in either side's elbow / wrist subtree
    std::vector<int8_t> side;                   // -1, or 0/1: in that arm's subtree (below the clavicle)
    std::vector<int> arm_set[2], elbow_set[2], wrist_set[2];
    // [side][0 the upper arm (arm_set less elbow_set), 1 the forearm (elbow_set less wrist_set)]: the deforming bones
    // (lengthened by the arm stretch) and the attachment bones (only moved, so the props on them stay rigid)
    std::vector<int> seg_def[2][2], seg_att[2][2];
    int att_wrist[2] = {-1, -1};  // wrist_l_Attachment, wrist_r_Attachment: where the game hangs a held item
    int finger[2][15] = {};       // thumb_01..03, finger_11..43 of each hand (-1: absent)
    bool mirror_ok = false;       // the rest skeleton is left/right symmetric across a plane (n . x = c), within 5 mm
    float mir_n[3] = {1, 0, 0}, mir_c = 0, mir_res = 1e9f;
    int pelvis = -1;                    // straightened on its own; its subtree less the legs (hip_l, hip_r) goes with it
    std::vector<int> pelvis_set;
    std::vector<int> chain_set[4];      // each one's subtree
    std::vector<uint8_t> in_head, in_up;
    // [Body] Stance=still (2026-10-11): each bone's parent (-1 the root's), and the pinned bones: the torso (the pelvis
    // and spine00's subtrees) less the arms below the clavicles, the legs and the attachment bones; each pinned bone is
    // drawn in its bind pose under one frame, the rest hang from their nearest pinned ancestor
    std::vector<int> parent;
    std::vector<uint8_t> pin;
    // run 10 item 9b ([Body] StillBelt): the belt's attachment bones pinned with the torso. `skel belt scan` (cycle D7): the
    // outfit's gun belt and holster is one geometry weighted pelvis 65%, pistol 31%, hip_r 4%; the holster's leather
    // follows the `pistol` attachment bone, which hangs from the leg (so it swung with the legs); the pouch is `thrower`'s
    std::vector<uint8_t> belt_pin;
    int clav[2] = {-1, -1};
    int foot[2] = {-1, -1};  // the lowest leg joints found (foot_l/r, else ankle_l/r): the still diagnostic's feet
    int knee[2] = {-1, -1}, hipj[2] = {-1, -1};  // run 10 item 8b's measure: the knee (the foot's leg parent) and hip joints
    std::vector<int> leg_set[2][3];  // run 10 item 9 stage 2 ([Body] StillFeet): the hip's, the knee's and the foot's subtrees
    int max_index = -1;
    std::string up_name;
    // each joint's rest position in the model (bind) space: the skeleton data's default local matrices (+0x20, 0x40
    // each: axis rows, then the translation, relative to the parent) chained from the root, which sits 1.015 m up
    // (round 6: assuming the root's rest pose at the origin put every pivot a metre low). A joint is drawn at
    // M_b j_b + t_b in the set's space.
    std::vector<float> jbind;  // 3 per bone
};
Rig g_rig;
std::atomic<uint32_t> g_rig_gen{0};  // build_rig's count: a filter entry is for one rig

int bone_index(uintptr_t bones, int count, const char* name) {
    size_t n = std::strlen(name);
    for (int i = 0; i < count; ++i) {
        uintptr_t p = 0;
        char s[40] = {};
        if (!rd(bones + i * 0x110, &p) || !p || !readable(p, n + 1)) continue;
        std::memcpy(s, reinterpret_cast<const void*>(p), n + 1);
        if (std::strcmp(s, name) == 0) return i;
    }
    return -1;
}

std::vector<int> subtree(uintptr_t bones, int count, int root) {
    std::vector<int> out{root};
    uintptr_t root_rec = bones + static_cast<uintptr_t>(root) * 0x110;
    for (int i = 0; i < count; ++i) {
        if (i == root) continue;
        uintptr_t p = 0;
        rd(bones + i * 0x110 + 0x20, &p);
        for (int depth = 0; p && depth < 64; ++depth) {
            if (p == root_rec) {
                out.push_back(i);
                break;
            }
            if (!rd(p + 0x20, &p)) break;
        }
    }
    return out;
}

void mul3(const float* a, const float* b, float* o);
bool build_rig(uintptr_t data, const std::string& up_name) {
    if (data == g_rig.data && up_name == g_rig.up_name) return g_rig.head >= 0;
    g_rig = Rig{};
    g_rig_gen.fetch_add(1, std::memory_order_relaxed);  // the filter's entries are for one rig
    g_rig.data = data;
    g_rig.up_name = up_name;
    uintptr_t bones = 0;
    uint16_t count = 0;
    if (!rd(data, &bones) || !rd(data + 0x30, &count) || !bones || count == 0 || count > 512) return false;
    g_rig.count = count;
    {
        uintptr_t locals = 0;
        g_rig.jbind.assign(static_cast<size_t>(count) * 3, 0.0f);
        std::vector<float> grot(static_cast<size_t>(count) * 9, 0.0f);
        if (rd(data + 0x20, &locals) && locals && readable(locals, static_cast<size_t>(count) * 0x40)) {
            const float* L = reinterpret_cast<const float*>(locals);
            for (int i = 0; i < count; ++i) {
                const float* l = L + i * 16;
                float rl[9];  // column form: the transpose of the stored axis rows
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) rl[r * 3 + c] = l[c * 4 + r];
                uintptr_t par = 0;
                rd(bones + i * 0x110 + 0x20, &par);
                int pi = par ? static_cast<int>((par - bones) / 0x110) : -1;
                float* gr = &grot[static_cast<size_t>(i) * 9];
                float* gt = &g_rig.jbind[static_cast<size_t>(i) * 3];
                if (pi < 0 || pi >= i) {
                    std::memcpy(gr, rl, sizeof(rl));
                    for (int k = 0; k < 3; ++k) gt[k] = l[12 + k];
                } else {
                    const float* pr = &grot[static_cast<size_t>(pi) * 9];
                    const float* pt = &g_rig.jbind[static_cast<size_t>(pi) * 3];
                    mul3(pr, rl, gr);
                    for (int k = 0; k < 3; ++k) gt[k] = pr[k * 3] * l[12] + pr[k * 3 + 1] * l[13] + pr[k * 3 + 2] * l[14] + pt[k];
                }
            }
        } else {
            log::warn("[body] no default local matrices: joints from the skeleton only");
        }
    }
    g_rig.head = bone_index(bones, count, "head");
    if (g_rig.head < 0) return false;
    g_rig.head_set = subtree(bones, count, g_rig.head);
    for (int h = 0; h < 2; ++h) {
        const char* sfx = h ? "_r" : "_l";
        g_rig.arm[h] = bone_index(bones, count, (std::string("arm") + sfx).c_str());
        g_rig.elbow[h] = bone_index(bones, count, (std::string("elbow") + sfx).c_str());
        g_rig.wrist[h] = bone_index(bones, count, (std::string("wrist") + sfx).c_str());
        g_rig.armroll[h] = bone_index(bones, count, (std::string("armroll") + sfx).c_str());
        g_rig.wristroll[h] = bone_index(bones, count, (std::string("wristroll") + sfx).c_str());
        if (g_rig.arm[h] >= 0) g_rig.arm_set[h] = subtree(bones, count, g_rig.arm[h]);
        if (g_rig.elbow[h] >= 0) g_rig.elbow_set[h] = subtree(bones, count, g_rig.elbow[h]);
        if (g_rig.wrist[h] >= 0) g_rig.wrist_set[h] = subtree(bones, count, g_rig.wrist[h]);
        g_rig.att_wrist[h] = bone_index(bones, count, (std::string("wrist") + sfx + "_Attachment").c_str());
        int cl = bone_index(bones, count, (std::string("clavicle") + sfx).c_str());
        if (cl >= 0) g_rig.clav_set[h] = subtree(bones, count, cl);
    }
    {
        const char* const names[4] = {"pistol", "rifle", "thrower", "melee"};
        for (int i = 0; i < 4; ++i) g_rig.holster[i] = bone_index(bones, count, names[i]);
        g_rig.holster[6] = bone_index(bones, count, "rope");  // the Lasso holster (2026-10-13)
        const char* const pieces[body::kPieces] = {"pistol", "melee", "thrower", "rope", "rifle"};  // run 12 item 2
        for (int i = 0; i < body::kPieces; ++i) {
            g_rig.piece[i] = bone_index(bones, count, pieces[i]);
            g_rig.piece_set[i] = g_rig.piece[i] > 0 ? subtree(bones, count, g_rig.piece[i]) : std::vector<int>{};
        }
        // 2026-10-13 (the user: "When satchel moves, lasso moves. Not the same vice versa"): a piece's bone under another
        // piece's (the rope under the thrower) is that piece's alone: each moved by its own move only
        std::vector<int> own_set[body::kPieces];
        for (int i = 0; i < body::kPieces; ++i) {
            own_set[i] = g_rig.piece_set[i];
            for (int j = 0; j < body::kPieces; ++j) {
                if (j == i || g_rig.piece[j] <= 0 || g_rig.piece[j] == g_rig.piece[i]) continue;
                bool under = false;
                for (int x : g_rig.piece_set[i]) under = under || x == g_rig.piece[j];
                if (!under) continue;
                log::info("[body] the belt's piece %d (bone %d) hangs under piece %d (bone %d): moved by its own move only", j, g_rig.piece[j], i,
                          g_rig.piece[i]);
                std::vector<int> kept;
                for (int x : own_set[i]) {
                    bool theirs = false;
                    for (int y : g_rig.piece_set[j]) theirs = theirs || x == y;
                    if (!theirs) kept.push_back(x);
                }
                own_set[i].swap(kept);
            }
        }
        for (int i = 0; i < body::kPieces; ++i) g_rig.piece_set[i].swap(own_set[i]);
    }
    g_rig.in_elbow.assign(count, 0);
    g_rig.in_wrist.assign(count, 0);
    g_rig.side.assign(count, -1);
    for (int h = 0; h < 2; ++h) {
        for (int b : g_rig.elbow_set[h]) g_rig.in_elbow[b] = 1;
        for (int b : g_rig.wrist_set[h]) g_rig.in_wrist[b] = 1;
        // the side group: the clavicle's subtree. The torso collapses to the midpoint of the two cut points, so every
        // hidden point lies on the one line between the cuts and the upper chest (both clavicles and the spine) has no
        // area (round 6 had the torso at the root, and a V-shaped sliver until the clavicles went with the torso; round
        // 6c: with the clavicles on the torso, the sleeve's shoulder seam drew a needle from the elbow to the middle).
        for (int b : g_rig.clav_set[h]) g_rig.side[b] = static_cast<int8_t>(h);
    }
    g_rig.pelvis = bone_index(bones, count, "pelvis");
    if (g_rig.pelvis >= 0) {
        std::vector<int> all = subtree(bones, count, g_rig.pelvis), legs;
        for (const char* leg : {"hip_l", "hip_r"}) {
            int h = bone_index(bones, count, leg);
            if (h >= 0) {
                std::vector<int> t = subtree(bones, count, h);
                legs.insert(legs.end(), t.begin(), t.end());
            }
        }
        for (int b : all)
            if (std::find(legs.begin(), legs.end(), b) == legs.end()) g_rig.pelvis_set.push_back(b);
    }
    g_rig.up = bone_index(bones, count, up_name.c_str());
    if (g_rig.up >= 0) g_rig.up_set = subtree(bones, count, g_rig.up);
    // the chain from the upright bone up the spine (spine00 -> spine03 by default): each level takes a share of the
    // turn, so the waist (pelvis -> spine00, partly weighted) does not stretch (headset round 5b)
    {
        int n = 0;
        int cur = g_rig.up;
        while (cur >= 0 && n < 4) {
            g_rig.chain[n] = cur;
            g_rig.chain_set[n] = subtree(bones, count, cur);
            ++n;
            uintptr_t rec = bones + static_cast<uintptr_t>(cur) * 0x110;
            int nx = -1;
            for (int i = 0; i < count; ++i) {  // the spine child: a bone whose parent is cur and whose name starts "spine"
                uintptr_t par = 0, nmp = 0;
                char nm[8] = {};
                if (!rd(bones + i * 0x110 + 0x20, &par) || par != rec) continue;
                if (!rd(bones + i * 0x110, &nmp) || !nmp) continue;
                for (int k = 0; k < 7; ++k)
                    if (!rd(nmp + k, &nm[k]) || !nm[k]) break;
                if (std::strncmp(nm, "spine", 5) == 0 && !std::strstr(nm, "_")) {
                    nx = i;
                    break;
                }
            }
            cur = nx;
        }
    }
    g_rig.holster[4] = g_rig.chain[3] >= 0 ? g_rig.chain[3] : g_rig.chain[2];  // the chest (the ammo holster)
    g_rig.holster[5] = g_rig.pelvis >= 0 ? g_rig.pelvis : g_rig.chain[0];       // the lower back (the knife and lasso)
    g_rig.in_head.assign(count, 0);
    g_rig.in_up.assign(count, 0);
    for (int b : g_rig.head_set) g_rig.in_head[b] = 1;
    for (int b : g_rig.up_set) g_rig.in_up[b] = 1;
    for (int i = 0; i < count; ++i) {
        uintptr_t p = 0;
        char nm[40] = {};
        if (!rd(bones + i * 0x110, &p) || !p) continue;
        for (int k = 0; k < 39; ++k)
            if (!rd(p + k, &nm[k]) || !nm[k]) break;
        if (std::strstr(nm, "Attachment") || !std::strcmp(nm, "pistol") || !std::strcmp(nm, "melee") || !std::strcmp(nm, "rifle") ||
            !std::strcmp(nm, "thrower") || !std::strcmp(nm, "rope"))
            g_rig.attach.push_back(i);
    }
    {
        std::vector<uint8_t> is_att(count, 0);
        for (int b : g_rig.attach) is_att[b] = 1;
        for (int h = 0; h < 2; ++h) {
            for (int b : g_rig.arm_set[h])
                if (!g_rig.in_elbow[b]) (is_att[b] ? g_rig.seg_att[h][0] : g_rig.seg_def[h][0]).push_back(b);
            for (int b : g_rig.elbow_set[h])
                if (!g_rig.in_wrist[b]) (is_att[b] ? g_rig.seg_att[h][1] : g_rig.seg_def[h][1]).push_back(b);
        }
        log::info("[body] arm segments (stretch): left %zu+%zu / %zu+%zu, right %zu+%zu / %zu+%zu (deforming+attachment, upper / fore)",
                  g_rig.seg_def[0][0].size(), g_rig.seg_att[0][0].size(), g_rig.seg_def[0][1].size(), g_rig.seg_att[0][1].size(),
                  g_rig.seg_def[1][0].size(), g_rig.seg_att[1][0].size(), g_rig.seg_def[1][1].size(), g_rig.seg_att[1][1].size());
    }
    {  // the fingers, and the rest pose's middle plane from the left/right joint pairs (the finger mirror)
        static const char* const kFinger[15] = {"thumb_01",  "thumb_02",  "thumb_03",  "finger_11", "finger_12", "finger_13", "finger_21", "finger_22",
                                                "finger_23", "finger_31", "finger_32", "finger_33", "finger_41", "finger_42", "finger_43"};
        for (int h = 0; h < 2; ++h)
            for (int i = 0; i < 15; ++i) g_rig.finger[h][i] = bone_index(bones, count, (std::string(kFinger[i]) + (h ? "_r" : "_l")).c_str());
        std::vector<std::pair<int, int>> pairs;
        for (int i = 0; i < 15; ++i)
            if (g_rig.finger[0][i] >= 0 && g_rig.finger[1][i] >= 0) pairs.push_back({g_rig.finger[0][i], g_rig.finger[1][i]});
        const int jl[4] = {g_rig.wrist[0], g_rig.elbow[0], g_rig.arm[0], g_rig.att_wrist[0]}, jr[4] = {g_rig.wrist[1], g_rig.elbow[1], g_rig.arm[1], g_rig.att_wrist[1]};
        for (int i = 0; i < 4; ++i)
            if (jl[i] >= 0 && jr[i] >= 0) pairs.push_back({jl[i], jr[i]});
        float sum[3] = {0, 0, 0};
        for (auto [l, r] : pairs)
            for (int k = 0; k < 3; ++k) sum[k] += g_rig.jbind[static_cast<size_t>(l) * 3 + k] - g_rig.jbind[static_cast<size_t>(r) * 3 + k];
        const float sl = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        if (sl > 1e-3f && pairs.size() >= 19) {
            for (int k = 0; k < 3; ++k) g_rig.mir_n[k] = sum[k] / sl;
            const float* nn = g_rig.mir_n;
            float c = 0;
            for (auto [l, r] : pairs) {
                const float* a = &g_rig.jbind[static_cast<size_t>(l) * 3];
                const float* b = &g_rig.jbind[static_cast<size_t>(r) * 3];
                c += nn[0] * (a[0] + b[0]) * 0.5f + nn[1] * (a[1] + b[1]) * 0.5f + nn[2] * (a[2] + b[2]) * 0.5f;
            }
            g_rig.mir_c = c / static_cast<float>(pairs.size());
            float res = 0;
            for (auto [l, r] : pairs) {
                const float* a = &g_rig.jbind[static_cast<size_t>(l) * 3];
                const float* b = &g_rig.jbind[static_cast<size_t>(r) * 3];
                float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
                const float dn = d[0] * nn[0] + d[1] * nn[1] + d[2] * nn[2];
                for (int k = 0; k < 3; ++k) d[k] -= dn * nn[k];
                res = std::fmax(res, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
                const float mid = nn[0] * (a[0] + b[0]) * 0.5f + nn[1] * (a[1] + b[1]) * 0.5f + nn[2] * (a[2] + b[2]) * 0.5f;
                res = std::fmax(res, std::fabs(mid - g_rig.mir_c));
            }
            g_rig.mir_res = res;
            g_rig.mirror_ok = res < 0.005f;
        }
        log::info("[body] hands: attachments %d/%d, %zu left/right pairs, the rest pose's middle plane n (%.3f %.3f %.3f) c %.3f, off by %.4f m: "
                  "the finger mirror %s",
                  g_rig.att_wrist[0], g_rig.att_wrist[1], pairs.size(), g_rig.mir_n[0], g_rig.mir_n[1], g_rig.mir_n[2], g_rig.mir_c, g_rig.mir_res,
                  g_rig.mirror_ok ? "on" : "off");
    }
    {  // the still stance's parents and pinned bones
        g_rig.parent.assign(count, -1);
        for (int i = 0; i < count; ++i) {
            uintptr_t par = 0;
            if (rd(bones + i * 0x110 + 0x20, &par) && par >= bones && (par - bones) % 0x110 == 0) {
                const int pi = static_cast<int>((par - bones) / 0x110);
                if (pi >= 0 && pi < i) g_rig.parent[i] = pi;
            }
        }
        g_rig.pin.assign(count, 0);
        std::vector<uint8_t> skip(count, 0);
        for (int h = 0; h < 2; ++h) {
            for (int b : g_rig.arm_set[h]) skip[b] = 1;
            g_rig.clav[h] = bone_index(bones, count, h ? "clavicle_r" : "clavicle_l");
            g_rig.foot[h] = bone_index(bones, count, h ? "foot_r" : "foot_l");
            if (g_rig.foot[h] < 0) g_rig.foot[h] = bone_index(bones, count, h ? "ankle_r" : "ankle_l");
            g_rig.hipj[h] = bone_index(bones, count, h ? "hip_r" : "hip_l");
            if (g_rig.hipj[h] >= 0) g_rig.leg_set[h][0] = subtree(bones, count, g_rig.hipj[h]);
            if (g_rig.foot[h] >= 0) g_rig.leg_set[h][2] = subtree(bones, count, g_rig.foot[h]);
            // the knee: up the foot's parents to the one whose parent is the hip's child (hip -> knee -> ... -> foot)
            int k = g_rig.foot[h];
            for (int d = 0; k >= 0 && d < 6; ++d) {
                const int pk = g_rig.parent[k];
                if (pk < 0) break;
                if (g_rig.parent[pk] == g_rig.hipj[h] || pk == g_rig.hipj[h]) {
                    g_rig.knee[h] = pk == g_rig.hipj[h] ? k : pk;
                    g_rig.leg_set[h][1] = subtree(bones, count, g_rig.knee[h]);
                    break;
                }
                k = pk;
            }
        }
        for (int b : g_rig.attach) skip[b] = 1;
        g_rig.belt_pin.assign(count, 0);
        for (const char* nm : {"pistol", "thrower", "melee"}) {
            const int bi = bone_index(bones, count, nm);
            if (bi > 0 && bi < count && !(bi < static_cast<int>(g_rig.side.size()) && g_rig.side[bi] >= 0)) g_rig.belt_pin[bi] = 1;  // never an arm's
        }
        auto pin_all = [&](const std::vector<int>& v) {
            for (int b : v)
                if (b > 0 && b < count && !skip[b]) g_rig.pin[b] = 1;
        };
        pin_all(g_rig.pelvis_set);
        pin_all(g_rig.chain_set[0]);
        int np = 0;
        for (uint8_t p : g_rig.pin) np += p;
        log::info("[body] the still stance: %d bones pinned (the torso less the arms, the legs and the attachments); feet %d %d", np,
                  g_rig.foot[0], g_rig.foot[1]);
    }
    // the highest bone anything here touches: a set is transformed only when it holds it (the draw's count test)
    auto cover = [](int b) { g_rig.max_index = b > g_rig.max_index ? b : g_rig.max_index; };
    auto cover_all = [&](const std::vector<int>& v) {
        for (int b : v) cover(b);
    };
    cover(g_rig.head);
    cover(g_rig.up);
    cover(g_rig.pelvis);
    cover_all(g_rig.head_set);
    cover_all(g_rig.up_set);
    cover_all(g_rig.attach);
    cover_all(g_rig.pelvis_set);
    for (int b : g_rig.holster) cover(b);
    for (int k = 0; k < 4; ++k) {
        cover(g_rig.chain[k]);
        cover_all(g_rig.chain_set[k]);
    }
    for (int h = 0; h < 2; ++h) {
        cover(g_rig.arm[h]);
        cover(g_rig.elbow[h]);
        cover(g_rig.wrist[h]);
        cover(g_rig.att_wrist[h]);
        cover_all(g_rig.clav_set[h]);
        cover_all(g_rig.arm_set[h]);
        cover_all(g_rig.elbow_set[h]);
        cover_all(g_rig.wrist_set[h]);
        for (int k = 0; k < 2; ++k) {
            cover_all(g_rig.seg_def[h][k]);
            cover_all(g_rig.seg_att[h][k]);
        }
        for (int i = 0; i < 15; ++i) cover(g_rig.finger[h][i]);
        cover(g_rig.clav[h]);
    }
    log::info("[body] rig: %u bones (the highest used %d); head %d (+%zu under it); upright bone \"%s\" %d (+%zu under it)", count,
              g_rig.max_index, g_rig.head, g_rig.head_set.size() - 1, up_name.c_str(), g_rig.up, g_rig.up_set.empty() ? 0 : g_rig.up_set.size() - 1);
    log::info("[body] %zu attachment bones (props are moved or hidden with the body); pelvis %d (+%zu, legs excluded)", g_rig.attach.size(), g_rig.pelvis, g_rig.pelvis_set.empty() ? 0 : g_rig.pelvis_set.size() - 1);
    return true;
}

// ---- this frame's job (before_scene, once a frame; read by the draws of the same scene)
// the item in hand's game matrix, sampled at a visibility build (see g_held_ring)
struct HeldSample {
    bool valid = false;
    bool left = false;
    float m[16] = {};
    // placed at the drawn hand ([Body] HeldPropAtHand): the hand's world correction it was placed with; the draws then
    // take only what the hand moved since (T_now o T_used^-1)
    bool placed = false;
    float A[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, a[3] = {};
    double ad[3] = {};               // a in double (round 8: the gun shook slightly in the hand; at |z| 2400 m a float step is 0.24 mm)
    float game[16] = {};             // the game's own matrix (W +0x80), placed or not
    float ik[3] = {}, ik_hold[3] = {};  // the weapon's support-hand offsets (aim.cpp)
    bool aiming = false;
    uintptr_t W = 0;                 // the weapon object (another gun: the learned foregrip is dropped)
    float mo[3] = {};                // the weapon's MuzzleOffset in the gun's axes (the transplant's pin)
    bool mo_ok = false;
    uint64_t build = 0;              // the visibility build it was taken at (1, 2, ...; 0: never written)
    bool sec = false;                // [Hands] DualWield: the second gun's (placed at John's hand `john`)
    int john = -1;
    // [Hands] CarryByForegrip: the item in hand is the carried long gun in its put-away, placed at John's hand follow_j's
    // target (pair_t, pair_r: that target as the placement read it; A, ad that hand's correction): its draws take that
    // target's move since, not the bone it hangs on (-1: none)
    int follow_j = -1;
    float mshift[3] = {};            // [Hands] DualWieldOwnModel: the copy's shot moved by the two models' muzzles (C's axes)
    bool pair_ok = false;
    float pair_t[3] = {}, pair_r[9] = {};
    bool put = false;  // (a second gun's sample: the carried gun placed while the game puts it away, not yet holstered)
};
constexpr int kHeldRing = 4;
// The latest visibility build's sample, if the item was in hand then (else null). The ring is written in turn, so its
// array order is not its age (the last valid slot was 0-3 builds old).
const HeldSample* newest_held(const HeldSample (&held)[kHeldRing]) {
    const HeldSample* n = nullptr;
    for (const HeldSample& x : held)
        if (x.build && (!n || x.build > n->build)) n = &x;
    return n && n->valid ? n : nullptr;
}

struct Frame {
    bool valid = false;
    uintptr_t skel = 0;
    bool hide = false;
    int stance = 0;
    float head_local[3] = {}, up_local[3] = {}, head_world[3] = {};
    float shift_bind[3] = {};  // the body's shift in the root bone's frame
    float shift_world[3] = {};
    bool shift = false;
    float up_world[3] = {};
    float w_root[16] = {};
    float chain_local[4][3] = {};
    float pelvis_local[3] = {};
    int show = 0;
    bool lock_torso = false;
    bool actor_valid = false;
    float actor_world[3] = {};
    bool ik[2] = {false, false};
    float ik_t[2][3] = {};      // the wrist targets (world)
    float ik_r[2][9] = {};      // the controllers' orientations (world, the calibration turn applied)
    float ik_pole[2][3] = {};   // the elbows' bend hints (world)
    float arm_local[2][3] = {}, elbow_local[2][3] = {}, wrist_local[2][3] = {};
    float pitch = 0;      // radians, the torso pitch offset
    float stretch_max = 1.0f;  // the arm stretch's cap ([Hands] ArmStretch; 1 = off)
    float collapse = 1e-7f;    // [Body] CollapseScale: a hidden bone's matrix scale (its vertices onto its target)
    bool collapse_root = true;  // the root bone (0) hidden with the rest in the show modes
    bool still_cloth = true;   // [Body] HiddenNoFlutter: no cloth flutter on the player's draws while parts are hidden
    bool no_flutter = true;    // [Body] NoFlutter: none on every player draw (2026-11-02, after optimumbox's)
    float yaw = 0;        // radians about the world's up through the root: the facing lock
    bool lock = false;
    // [Body] Stance=still: where the bind pose's origin is drawn (world), this frame's and the last frame's (the
    // previous set's): the actor's position across, and up so the bind head joint sits at the head bone's height over
    // the actor low-passed as the camera's eye height is ([Body] HeightSmoothing)
    bool still = false;
    bool still_feet = false;  // [Body] StillFeet
    bool still_belt = true;   // [Body] StillBelt (run 10 item 9b)
    bool ride_keep = false;   // [Horse] BodyWithCamera and mounted: the still body pinned while the animation is within 1.5 m
    float still_t[3] = {}, still_t_prev[3] = {};
    float view_h = 0;  // the view's heading (radians; faces (-sin h, -cos h)): the still diagnostic's axes
    std::vector<float> attach_pos;  // the attachment bones' world positions (3 each, the order of g_rig.attach)
    HeldSample held[kHeldRing];     // the item in hand's game matrices, the last builds
    HeldSample sec[kHeldRing];      // [Hands] DualWield: the second gun's placed matrices, the last builds
    HeldSample copy[kHeldRing];     // [Hands] DualWieldCopy: the copy's placed matrices (C), the last builds
    bool sec_any = false;
    int sec_new = -1;               // the newest of them (an index into sec)
    uint64_t sec_build = 0;         // the newest visibility build's number (a sample of it was placed this update)
    float sec_box[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    bool held_any = false;
    bool held_fix = true;           // [Body] HeldPropFix: the draws matched to the held item
    int held_new = -1;              // the latest build's sample (its index in held), valid or not
    // min xyz, max xyz: the held samples' origins (2 mm, held_match's), the attachment bones (0.5 m, the props' keep)
    float held_box[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    float attach_box[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    float cam[16] = {};  // the frame's game camera (the body points carry it)
    bool cam_ok = false;
    bool long_gun = false;     // a long gun in hand: the foregrip is found on it
    int weapon = -1;           // the eWeapon in hand (the actor state), for the grips learned per weapon
    int gun_j = 1;             // John's gun hand (0: left-handed with GunInGunHand)
    int ctrl[2] = {0, 1};      // the controller each of John's hands follows
    int item_side = -1;        // the game's hand that holds the item in hand (0 left, 1 right), from the sample
    bool xfer = false;         // the item is drawn in John's other hand (the transplant)
    int fingers = 0;           // TransplantFingers while transplanting (0 when the mirror is off)
    bool copy_grip = false;    // [Hands] CopyGrip: the copy or a second gun is out, the hand holding it takes the gun hand's grip
    bool pin = true;           // the transplant's attachment point moved across the hand's plane (into the fist)
    bool mirror = true;        // [Hands] TransplantMirror: the transplant as the exact mirror of the right hand's hold
    float two_blend = 0.0f;    // holster.cpp's two-handed weight
    float barrel_blend = 0.0f; // [Reload] BarrelHandSnap: the off hand's weight on the Double-barrel's open barrels
    float part_blend = 0.0f;   // [Reload] PartHandSnap: the off hand's weight on the part it grips
    float part_p[3] = {};      // that part's handle in the drawn gun's frame
    float part_turn = 0.0f;    // (2026-10-28) its turn about the gun's z (deg: the bolt's lift), the hand turned with it
    bool part_bolt = false;    // the part a bolt's handle: the hand held on it (the other parts' at the controller's turn)
    // (2026-10-28) [Reload] CarriedBolt's bolt in the free gun hand: the weight, the handle and the carried gun's axes in the
    // carrying hand's target frame, the handle's turn, John's carrying hand (actions::carried_part_snap)
    float cpart_blend = 0.0f;
    float cpart_p[3] = {}, cpart_R[9] = {};
    float cpart_turn = 0.0f;
    int cpart_j = -1;
    // [Reload] ThumbCock (2026-10-28): the single action in hand with RevolverControl=cock (-1: none), the thumb's weight on
    // its hammer's spur, the drawn hammer (0 down - 1 cocked)
    int thumb_w = -1;
    float thumb_wt = 0.0f, thumb_hammer = 1.0f;
    int sthumb_w = -1, sthumb_j = -1;  // (C3) the second gun's (or the copy's) thumb cock: its eWeapon, John's hand holding it
    float sthumb_wt = 0.0f;
    float barrel_open = 0.0f;  // the barrels' drawn opening (0 shut - 1 open), for the hand's turn with them
    float pump = 0.0f;         // [Reload] PumpParts: the fore-end's drawn travel (0 - 1 of kPumpSlide): the front hand's grip with it
    bool snap = false;         // [Reload] TwoHandedSnap
    float fore_off[3] = {};    // [Reload] ForegripOffset: right, up, forward in the gun's axes
    int carry_j = -1;          // [Hands] CarryByForegrip: John's hand carrying a long gun (-1: none)
    // and the long gun in hand is the carried one, its put-away under way (not taken back): drawn at the carrying hand
    // (hk_obj_set_matrix), so no two-handed turn of the gun hand, no LeverControl carry, no foregrip on it
    bool carry_put = false;
    int round_hand = -1;       // the hand holding a round (round_draw::held_hand), read once a draw: both sets agree
    // (2026-10-27, [Hands] DualWieldCrossReload) John's hand holding the second gun or the copy (-1: none, or a carry): it
    // may hold the gun in hand's rounds now, and keeps the gun's grip (no round pinch put on it)
    int sec_j = -1;
    // [Hands] GunKick (2026-10-27, off; kick_frame): this frame's kick of each spring, 0 the gun in hand, 1 the second gun or
    // the copy: on, its turn and slide as a move in the gun's own axes (kick_l: [R | t], the rows; x right, y up, z back),
    // John's hand holding that gun (-1: none, or the carry), the spring's value and its last shot (log::now_ms). kick_any:
    // a kick to put on a draw or the trace to keep (off: the draws' one test). kick_aim: the aim's barrel in the gun hand's
    // target (aim::barrel_in_target), for the trace only
    bool kick_any = false;
    bool kick_on[2] = {false, false};
    float kick_l[2][12] = {};
    int kick_j[2] = {-1, -1};
    int kick_key[2] = {-1, -1};  // the gun each spring kicks (its eWeapon; the second gun's slot too, or the copy): another resets it
    float kick_s[2] = {};
    double kick_shot_ms[2] = {-1e12, -1e12};
    bool kick_aim_ok = false;
    float kick_aim[3] = {};
    // [Hands] GunCollision (2026-10-29, off; coll_frame): each spring's gun moved off what it touches, in its own axes (m: added
    // to its kick's slide, kick_frame), and an empty hand moved off a gun held still (world, m: added to its wrist's IK
    // target only; the published hand stays the controller's)
    bool coll_on[2] = {false, false};
    float coll_l[2][3] = {};
    bool coll_hand_on[2] = {false, false};
    float coll_hand[2][3] = {};
};
Frame g_frame;
// [Hands] CarryByForegrip: the front hand's snapped hold in its controller's frame (the snapped wrist target and turn as
// the controller's target and turn times these), learned while fully two-handed; the carrying hand is drawn there, so
// neither it nor the gun on it moves at the hand-over and both then move rigidly with the controller
float g_carry_prel[2][3] = {};
float g_carry_orel[2][9] = {};
bool g_carry_rel_ok[2] = {false, false};
uint64_t g_carry_learns = 0, g_carry_holds = 0, g_carry_clears = 0;  // "skel carry" (the draw mutex's; read racily, a print)

// ---- the player's grmMatrixSet. It holds `count` skinning matrices (3 rows of 4 floats: M (3x3) maps a bind-space
// vertex, the 4th column is the translation) at +0xd4, then the previous frame's set. In the bind pose every bone's
// skinning matrix equals the root's (the root's bind pose is the identity), so:
//  - a joint j (in the root bone's frame, from the skeleton's world matrices W: local = W_root * (world - t_root)) is
//    at M_root * j + t_root in the set's space;
//  - "upright" turns the upright bone's subtree about its joint by R = M_root * M_up^-1: the torso takes its bind
//    orientation over the hips, the arms keep their motion relative to it;
//  - "lean" turns it about the body's sideways axis (the root's x) by the angle between the spine's up and the root's;
//  - the head bones collapse to the (corrected) head joint: p + 1e-4 * M_b;
//  - the shift moves every bone but the root by M_root * shift_bind.
std::atomic<uint64_t> g_sets{0}, g_hat_hides{0}, g_props{0}, g_rigid_props{0}, g_stretches{0};

std::mutex g_draw_mutex;

void mul3(const float* a, const float* b, float* o) {  // o = a * b (3x3 row-major)
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
}

void norm(float* v) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void cross(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

// The 3x3 of a set matrix with orthonormal columns (Gram-Schmidt; the third column from the first two).
void ortho_cols(const float* s, float* o) {
    float c0[3] = {s[0], s[4], s[8]}, c1[3] = {s[1], s[5], s[9]}, c2[3];
    norm(c0);
    float d = c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2];
    for (int k = 0; k < 3; ++k) c1[k] -= d * c0[k];
    norm(c1);
    cross(c0, c1, c2);
    for (int r = 0; r < 3; ++r) {
        o[r * 3 + 0] = c0[r];
        o[r * 3 + 1] = c1[r];
        o[r * 3 + 2] = c2[r];
    }
}

void axis_angle(const float* a, float ang, float* o) {
    float c = std::cos(ang), s = std::sin(ang), t = 1 - c, x = a[0], y = a[1], z = a[2];
    const float m[9] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y, t * x * y + s * z, t * y * y + c,
                        t * y * z - s * x, t * x * z - s * y, t * y * z + s * x, t * z * z + c};
    std::memcpy(o, m, sizeof(m));
}

// Per-bone corrections in the set's space, T_b(y) = A_b y + a_b, built per set: the spine chain's shares of the
// upright turn, the facing lock (all bones about the root joint), the shift. Applied as M' = A M, t' = A t + a.
// The head bones then collapse to their corrected head joint. For slot 0 the world versions are kept for the props.
struct Corr {
    float A[9];
    float a[3];
};
// run 5 (held_prop): the world correction that moves a draw now at (Rc: its axes as columns, pc) onto the wanted pose
// (12 floats: the axes X, Y, Z, then the position): A = Rt Rc^T, a = pt - A pc
// run 5 item 3 ([Reload] OpenCylinder) and run 6 item 9a: the gun in hand's parts drawn (the table below)
// Run 6 item 9a: each gun's moving parts in one table. A part is a hinge (deg about the model's x, y or z at a pivot)
// and a slide (a vector, metres), both scaled by its driver's amount (actions.cpp: the opening, the lever, the hammer),
// applied on the bone's own matrix (kOwn, M' = M Q_p: the game's own motion of that bone kept, as a revolver's cylinder
// spinning with the shots) or rebuilt from the reference bone's (kRef, M' = M_ref Q_p: the game's own cycling of the
// part replaced, the lever guns), plus a constant offset t0 (where the clip's rest pose differs from the model's). The
// model's flags: its fire clip stopped, and the gun held still in the hand through John's fire clip. Q_p(v) = Q (v - p)
// + p + s, in the gun's own axes (x right, y up, z back), the bone's matrix after it (a skinned set's matrix carries the
// gun's world placement; a record's set is identity at rest: the same).
using actions::kDrvOpen, actions::kDrvLever, actions::kDrvHammer, actions::kDrvIndex, actions::kDrvPump, actions::kDrvBoltLift, actions::kDrvBoltSlide,
    actions::kDrvBreech, actions::kDrvRod, actions::kDrvCount;  // the drivers
// kHinged: the part rides on the bone the model's last kOwn open part turned (the top-breaks' cylinder on the hinge):
// moved by that bone's change in the world, so its own pose (the game's fire clip spins it) is kept as it tips
enum PartBase : uint8_t { kOwn, kRef, kHinged };
struct GunPart {
    int8_t bone, axis;  // the bone in the set; the hinge's axis (0 x, 1 y, 2 z)
    uint8_t drv, base;  // its driver (actions::Driver), its base (PartBase)
    float deg;          // the hinge's turn at the driver's full amount
    float pivot[3];     // the hinge's point (m)
    float slide[3];     // the slide at the full amount (m)
    float t0[3];        // a constant offset (m)
};
constexpr uint8_t kStopFireClip = 1, kSteady = 2;
constexpr float kPumpSlide = 0.0823f;  // the Pump-action's fore-end at its rear stop (m, +z: back): its row, John's hand on it
struct GunModel {
    int weapon, count, ref;
    uint8_t flags;
    int n;
    GunPart part[4];
};
const GunModel kGunModels[] = {
    // [Reload] OpenCylinder: the top-breaks (the Schofield, the LeMat): the hinge (bone 3) and the cylinder (4, its own
    // spin kept) 70 degrees down about x at the hinge (their own reloads, "skel parts": within 0.1 mm; run 6 item 2)
    // run 6 item 9b: each round put in by hand turns the cylinder one chamber (60 degrees, as the game's own fire clip
    // turns the Schofield's and the LeMat's) about its own axis (z) at its centre (research\run6\clip-bones.md: the
    // bones' bind origins, checked against 13 of the game's measured pivots); the turn listed after the hinge that
    // carries the cylinder (on one bone the later part acts first: the cylinder turns in its shut place, then tips)
    // 2026-10-26 ([Reload] RevolverControl=cock): the single actions' hammers drawn from the player's flick, rebuilt from
    // the root (the game's own re-cock in the fire clip stopped): 40 degrees back about x at the hammer's bind origin
    // (research\run6\clip-bones.md; the bones re-read from fragments.rpf: the Schofield's and the LeMat's hammer 2, the
    // Cattleman's 3)
    {5, 6, 0, 0, 4, {{3, 0, kDrvOpen, kOwn, -70.0f, {0.0157f, 0.0272f, -0.0585f}, {}, {}}, {4, 0, kDrvOpen, kHinged, -70.0f, {0.0157f, 0.0272f, -0.0585f}, {}, {}},
                     {4, 2, kDrvIndex, kOwn, -60.0f, {0.0f, 0.04092f, -0.01445f}, {}, {}}, {2, 0, kDrvHammer, kRef, 40.0f, {0.0f, 0.04084f, 0.03053f}, {}, {}}}},
    {7, 6, 0, 0, 4, {{3, 0, kDrvOpen, kOwn, -70.0f, {0.0063f, 0.0192f, -0.0554f}, {}, {}}, {4, 0, kDrvOpen, kHinged, -70.0f, {0.0063f, 0.0192f, -0.0554f}, {}, {}},
                     {4, 2, kDrvIndex, kOwn, -60.0f, {0.0f, 0.04120f, -0.02906f}, {}, {}}, {2, 0, kDrvHammer, kRef, 40.0f, {0.0f, 0.02664f, 0.01354f}, {}, {}}}},
    // the Double-action: its cylinder (bone 2) swung out to the left on a crane beside it (the mod's own, approved by the
    // user), turned a chamber a round about its own centre: the Cattleman's (its frame is the Cattleman's: the flap's and
    // the trigger's pivots within 0.1 mm). Not bone 2's origin (-6.4, 22.1) mm: that is the crane, about which the game's
    // own reload swings the cylinder out 60 degrees (round 13: turned about it, the cylinder went round the gun)
    {6, 6, 0, 0, 2, {{2, 2, kDrvOpen, kOwn, 80.0f, {0.0056f, 0.0101f, -0.0478f}, {}, {}}, {2, 2, kDrvIndex, kOwn, -60.0f, {0.0f, 0.04826f, 0.0f}, {}, {}}}},
    // the Cattleman: its loading gate (bone 4) 50 degrees about the barrel's axis at (15.7, 35.3) mm (a hinge within
    // 0.04 mm in its own reload); its cylinder (bone 2, the Double-action's place in the same skeleton; it never turns in
    // the game's clips) a chamber a round
    {4, 6, 0, 0, 3, {{4, 2, kDrvOpen, kOwn, -50.0f, {0.0157f, 0.0353f, 0.0f}, {}, {}}, {2, 2, kDrvIndex, kOwn, -60.0f, {0.0f, 0.04826f, 0.0f}, {}, {}},
                     {3, 0, kDrvHammer, kRef, 40.0f, {0.0f, 0.02744f, 0.02093f}, {}, {}}}},
    // [Reload] ManualBreak: the Double-barrel's barrels (bone 2) 45 degrees down (the game's 31; round 13: further) about x at (y 40.3, z -62.9) mm (its
    // own reload, a hinge within 0.01 mm; it swings to 33.5 and settles at 31)
    {16, 6, 0, 0, 1, {{2, 0, kDrvOpen, kOwn, -actions::kBreakOpenDeg, {0.0f, 0.0403f, -0.0629f}, {}, {}}}},
    // run 6 item 9d: the Sawed-off's barrels, the same (its skeleton's hinge is the Double-barrel's, bind origin (0, 40.4,
    // -62.9) mm; its own reload, cycle J: 33.4 degrees at the swing's end, held at 31)
    {15, 6, 0, 0, 1, {{2, 0, kDrvOpen, kOwn, -actions::kBreakOpenDeg, {0.0f, 0.0403f, -0.0629f}, {}, {}}}},
    // [Reload] LeverParts: the Carbine (repeater_carbine01x), fitted to its fire clip's frames (within 0.1 mm): the lever
    // (2) 50.1 degrees down and forward about (y 25.5, z -82.2 mm), closed 11.2 mm above the model's rest pose as the clip
    // holds it; the slide (3) 53.8 mm back with it; the hammer (5) 36.9 degrees back about (43.6, -16.2 mm) (cocked; the
    // model rests with it down). The trigger is left to the game.
    {8, 6, 0, kStopFireClip | kSteady, 3,
     {{2, 0, kDrvLever, kRef, 50.1f, {0.0f, 0.0255f, -0.0822f}, {}, {0.0f, 0.0112f, 0.0029f}},
      {3, 0, kDrvLever, kRef, 0.0f, {}, {0.0f, 0.0f, 0.0538f}, {}},
      {5, 0, kDrvHammer, kRef, 36.9f, {0.0f, 0.04357f, -0.01623f}, {}, {}}}},
    // run 6 item 9c: the other lever guns on the Carbine's design, each on its own skeleton (research\run6\clip-bones.md:
    // the bones' bind origins; their own fire clips and reloads, "skel parts", fitted within 0.5 mm). The Winchester (9)
    // and the Henry (10): the lever (2) 50.8 degrees about its bind origin with no offset (their shared clip, Rifle_lvr,
    // was made on the Winchester's skeleton), the slide (3) 54.4 mm back, the hammer (5) 36.9 degrees. The Evans (11):
    // the Carbine's lever offset (the clip holds it so: its idle lever is the model's rest, the fire clip's is 11.2 mm
    // up), its hammer turned about a point 5.5 cm ahead of the trigger as the game turns it. The Volcanic (0, a pistol):
    // the lever (5) 35 degrees, the hammer (3) 38.3; its slide (2) never moves in the game's clips and is left so.
    {9, 6, 0, kStopFireClip | kSteady, 3,
     {{2, 0, kDrvLever, kRef, 50.8f, {0.0f, 0.02271f, -0.02903f}, {}, {}},
      {3, 0, kDrvLever, kRef, 0.0f, {}, {0.0f, 0.0f, 0.0544f}, {}},
      {5, 0, kDrvHammer, kRef, 36.9f, {0.0f, 0.04525f, 0.01057f}, {}, {}}}},
    {10, 6, 0, kStopFireClip | kSteady, 3,
     {{2, 0, kDrvLever, kRef, 50.8f, {0.0f, 0.02271f, -0.02903f}, {}, {}},
      {3, 0, kDrvLever, kRef, 0.0f, {}, {0.0f, 0.0f, 0.0544f}, {}},
      {5, 0, kDrvHammer, kRef, 36.9f, {0.0f, 0.04968f, 0.00110f}, {}, {}}}},
    {11, 6, 0, kStopFireClip | kSteady, 3,
     {{2, 0, kDrvLever, kRef, 50.8f, {0.0f, 0.03750f, -0.04740f}, {}, {0.0f, 0.0112f, 0.0029f}},
      {3, 0, kDrvLever, kRef, 0.0f, {}, {0.0f, 0.0f, 0.0544f}, {}},
      {5, 0, kDrvHammer, kRef, 36.9f, {0.0f, 0.03076f, -0.06668f}, {}, {}}}},
    {0, 6, 0, kStopFireClip | kSteady, 2,
     {{5, 0, kDrvLever, kRef, 35.0f, {0.0f, 0.01905f, -0.02366f}, {}, {}}, {3, 0, kDrvHammer, kRef, 38.3f, {0.0f, 0.03263f, 0.02326f}, {}, {}}}},
    // run 6 item 9e: [Reload] PumpParts: the Pump-action's fore-end (2, "sliding") 82.3 mm back at the rear stop (its own
    // fire clip, cycle J; 86.7 in its reload), rebuilt from the root (the game's own pumping replaced). Its hammer (4)
    // only flicks at the shot in the game's clip (0-40-0 within 0.1 s) and is left at the model's rest.
    {17, 5, 0, kStopFireClip | kSteady, 1, {{2, 0, kDrvPump, kRef, 0.0f, {}, {0.0f, 0.0f, kPumpSlide}, {}}}},
    // run 6 item 9f: [Reload] BoltParts: the bolt (2, "sliding") turned up about its own axis (z, through its bind origin's
    // height) by 59.7 degrees, then slid back 82.5 mm (the Bolt Action's own reload, cycle J: the turn about (0, 76.3) mm
    // within 0.1 mm, the slide 82.5). The turn and the slide share the axis, so their order does not matter. The
    // Carcano's bolt cannot turn in the game (no rotation channel: it only slides, and its clips hold it 31 mm back and 10
    // mm down even shut, another skeleton's pose); drawn as the Bolt Action's about its own bolt origin's axis (99.3 mm),
    // from the model's rest.
    {13, 5, 0, kStopFireClip | kSteady, 2,
     {{2, 2, kDrvBoltLift, kRef, 59.7f, {0.0f, 0.07632f, 0.0f}, {}, {}}, {2, 2, kDrvBoltSlide, kOwn, 0.0f, {}, {0.0f, 0.0f, 0.0825f}, {}}}},
    {20, 5, 0, kStopFireClip | kSteady, 2,
     {{2, 2, kDrvBoltLift, kRef, 59.7f, {0.0f, 0.09926f, 0.0f}, {}, {}}, {2, 2, kDrvBoltSlide, kOwn, 0.0f, {}, {0.0f, 0.0f, 0.0825f}, {}}}},
    // (2026-11-01, the off-hand build step 8, R14) the Explosive Rifle: the Bolt Action's bolt on its own model (bone 2,
    // "sliding", bind (0 0.07632 0.00651): the same turn axis, a turn and a slide channel; fragments.rpf
    // rifle_explosive01x.wft), drawn as the Bolt Action's [I: its handle's lift and travel the Bolt Action's]
    // (step 8, R14) the Blunderbuss: no part drawn (its rod, bone 4, not yet), its fire clip's own reload (the ramrod) stopped
    // for the mod's ram (actions.cpp's muzzle-loader); 5 bones (fragments.rpf rifle_blunderbuss01x.wft)
    // (2026-11-01) its rod (bone 4, "rod": free, bind (0 0.024 -0.460)) drawn back along the barrel with the ram's stroke
    // (actions' kDrvPump for this gun: 0 home, 1 the stroke's 6 cm) [I: the rod stays in its pipe under the barrel; the
    // headset to judge]
    // (2026-11-01, GOAL-offhand-changes C15b; round 65 check 15, the user: the ramrod did not appear) [Reload] RamrodAtChest:
    // the ramrod held, its rod drawn in that hand: its head moved to the hand, its body as it lies (back along the barrel: at
    // the muzzle into it; the review: turned half round it stuck out ahead in the ram) (actions::rod_place; kDrvRod's slide,
    // not the row's) [I: the rod's mesh runs back from its head]
    {34, 5, 0, kStopFireClip | kSteady, 2,
     {{4, 0, kDrvPump, kRef, 0.0f, {}, {0.0f, 0.0f, 0.06f}, {}}, {4, 1, kDrvRod, kRef, 0.0f, {0.0f, 0.024f, -0.460f}, {}, {}}}},
    {31, 5, 0, kStopFireClip | kSteady, 2,
     {{2, 2, kDrvBoltLift, kRef, 59.7f, {0.0f, 0.07632f, 0.0f}, {}, {}}, {2, 2, kDrvBoltSlide, kOwn, 0.0f, {}, {0.0f, 0.0f, 0.0825f}, {}}}},
    // run 6 item 9g: [Reload] BreechParts. The Springfield's trapdoor (2) and the release on it (3) 90 degrees forward
    // about the trapdoor's hinge (its own fire clip, cycle J: 94.9 at the swing's end, held at 90), its hammer (5) 35
    // degrees cocked; the Rolling Block's breech block (4, "latch") rolled 75 degrees back about its pivot, its hammer
    // (3) 36.7; the Buffalo's falling block (2, "sliding") 37 degrees down at the rear (its prop clip's: the game never
    // turns it, its bone has no rotation channel), its hammer (4) 30. Rebuilt from the root: the games' own working gone.
    {12, 6, 0, kStopFireClip | kSteady, 3,
     {{2, 0, kDrvBreech, kRef, -90.0f, {0.0f, 0.06671f, -0.11778f}, {}, {}}, {3, 0, kDrvBreech, kRef, -90.0f, {0.0f, 0.06671f, -0.11778f}, {}, {}},
      {5, 0, kDrvHammer, kRef, 35.0f, {0.0f, 0.04377f, -0.02194f}, {}, {}}}},
    {19, 5, 0, kStopFireClip | kSteady, 2,
     {{4, 0, kDrvBreech, kRef, 75.0f, {0.0f, 0.05265f, 0.02453f}, {}, {}}, {3, 0, kDrvHammer, kRef, 36.7f, {0.0f, 0.04262f, 0.05544f}, {}, {}}}},
    {14, 5, 0, kStopFireClip | kSteady, 2,
     {{2, 0, kDrvBreech, kRef, 37.0f, {0.0f, 0.03182f, -0.05969f}, {}, {}}, {4, 0, kDrvHammer, kRef, 30.0f, {0.0f, 0.05270f, -0.02644f}, {}, {}}}},
    // run 6 item 9h: [Reload] SemiAutoParts: the Semi-Auto Shotgun's bolt (2, "sliding") racked 65 mm back (the mod's:
    // no clip of the game's moves it); its fire clip and its cycling between shots are the game's (no flags)
    {18, 5, 0, 0, 1, {{2, 0, kDrvBoltSlide, kRef, 0.0f, {}, {0.0f, 0.0f, 0.065f}, {}}}},
    // 2026-10-28, [Reload] PistolSlides (GOAL-weapons-handling.md item 3; research\run28\magazines.md): the pistols' slides
    // drawn from the player's rack, rebuilt from the root, their own fire clips stopped (not steadied: John's recoil kept).
    // The bones from the models (pistol_highpower01x: body 0, slide 1, trigger 2; pistol_semiauto01x: body 0, slide 1 the
    // toggle's rear link hinged at (0, 66.9, 112.5) mm, slide02 2 its front link from the knee (0, 71.0, 83.5) mm, trigger
    // 3; pistol_mauser01x: body 0, sliding 1 the bolt, hammer 2, trigger 3), the travels the game's fire clips' as
    // optimumbox measured them (MIT, v0.8.11.10) [I]. The front link: carried by the knee (its rise, its move back) and
    // turned about the knee's rest point (on its own bone the later part acts first: turned, then carried)
    {2, 3, 0, kStopFireClip, 1, {{1, 0, kDrvBoltSlide, kRef, 0.0f, {}, {0.0f, 0.0f, actions::kSlideTravelHP}, {}}}},
    {1, 4, 0, kStopFireClip, 4,
     {{1, 0, kDrvBoltLift, kRef, actions::kToggleDeg, {actions::kToggleHinge[0], actions::kToggleHinge[1], actions::kToggleHinge[2]}, {}, {}},
      {2, 0, kDrvPump, kRef, 0.0f, {}, {0.0f, actions::kToggleDy, 0.0f}, {}},
      {2, 0, kDrvBoltSlide, kOwn, 0.0f, {}, {0.0f, 0.0f, actions::kToggleDz}, {}},
      {2, 0, kDrvBreech, kOwn, actions::kToggleFrontDeg, {actions::kToggleKnee[0], actions::kToggleKnee[1], actions::kToggleKnee[2]}, {}, {}}}},
    {3, 4, 0, kStopFireClip, 1, {{1, 0, kDrvBoltSlide, kRef, 0.0f, {}, {0.0f, 0.0f, actions::kBoltTravelMauser}, {}}}},
};
const GunModel* gun_model(int weapon, int count = -1) {
    for (const GunModel& g : kGunModels)
        if (g.weapon == weapon && (count < 0 || g.count == count)) return &g;
    return nullptr;
}
uint8_t gun_flags(int weapon) {
    const GunModel* g = gun_model(weapon);
    return g ? g->flags : 0;
}
// m (a set matrix, 3x4 rows) = B Q_p: B the base (3x4; it may be m itself), Q a turn of deg about the axis at p, then s
void part_apply(const float* B, int axis, float deg, const float* p, const float* s, float* m) {
    const float th = deg * 0.0174532925f, c = std::cos(th), sn = std::sin(th);
    float Q[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (axis == 0) Q[4] = c, Q[5] = -sn, Q[7] = sn, Q[8] = c;       // about x
    else if (axis == 1) Q[0] = c, Q[2] = sn, Q[6] = -sn, Q[8] = c;  // about y
    else Q[0] = c, Q[1] = -sn, Q[3] = sn, Q[4] = c;                 // about z
    float qp[3], out[12];
    for (int i = 0; i < 3; ++i) qp[i] = p[i] - (Q[i * 3] * p[0] + Q[i * 3 + 1] * p[1] + Q[i * 3 + 2] * p[2]) + s[i];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = B[i * 4] * Q[j] + B[i * 4 + 1] * Q[3 + j] + B[i * 4 + 2] * Q[6 + j];
        out[i * 4 + 3] = B[i * 4 + 3] + B[i * 4] * qp[0] + B[i * 4 + 1] * qp[1] + B[i * 4 + 2] * qp[2];
    }
    std::memcpy(m, out, sizeof(out));
}
// G = N O^-1 (3x4 rows, affine): the change that takes the matrix O to N
void change_of(const float* O, const float* N, float* G) {
    const float a = O[0], b = O[1], c = O[2], d = O[4], e = O[5], f = O[6], g = O[8], h = O[9], i = O[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) return;
    const float inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det, (f * g - d * i) / det, (a * i - c * g) / det,
                          (c * d - a * f) / det, (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    const float ot[3] = {O[3], O[7], O[11]};
    for (int r = 0; r < 3; ++r) {
        float t = N[r * 4 + 3];
        for (int col = 0; col < 3; ++col) {
            const float v = N[r * 4] * inv[col] + N[r * 4 + 1] * inv[3 + col] + N[r * 4 + 2] * inv[6 + col];
            G[r * 4 + col] = v;
            t -= v * ot[col];
        }
        G[r * 4 + 3] = t;
    }
}
// The gun model's parts whose drivers are in `mask` (1 << PartDrv), by the amounts `drv`, on both halves of the set (the
// current and the previous); the parts turned (of the current half)
float g_open_last[12] = {};  // the last opened bone's matrix (under g_draw_mutex; "skel open")
float g_drawn_set[8][12] = {};  // "skel drawn": the current half as the parts left it (under g_draw_mutex)
int g_drawn_count = 0, g_drawn_weapon = -1;
unsigned gun_parts(float* sm, int count, int weapon, const float* drv, unsigned mask) {
    const GunModel* g = gun_model(weapon, count);
    if (!g || g->ref >= count) return 0;
    unsigned n = 0;
    for (int half = 0; half < 2; ++half) {
        float* base = sm + static_cast<size_t>(half) * count * 12;
        float ref[12];
        std::memcpy(ref, base + g->ref * 12, sizeof(ref));
        float G[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // the last kOwn open part's bone: its change (new old^-1)
        for (int k = 0; k < g->n; ++k) {
            const GunPart& P = g->part[k];
            if (!(mask >> P.drv & 1u) || P.bone < 0 || P.bone >= count) continue;
            const float a = drv[P.drv];
            float sl[3] = {P.slide[0] * a + P.t0[0], P.slide[1] * a + P.t0[1], P.slide[2] * a + P.t0[2]};
            float rq[9];
            if (P.drv == kDrvRod && !actions::rod_place(rq, sl)) continue;  // (C15b: the hand's place, this frame's)
            float* m = base + P.bone * 12;
            if (P.base == kHinged) {
                float out[12];
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) out[i * 4 + j] = G[i * 4] * m[j] + G[i * 4 + 1] * m[4 + j] + G[i * 4 + 2] * m[8 + j];
                    out[i * 4 + 3] = G[i * 4] * m[3] + G[i * 4 + 1] * m[7] + G[i * 4 + 2] * m[11] + G[i * 4 + 3];
                }
                std::memcpy(m, out, sizeof(out));
            } else if (P.drv == kDrvRod) {  // (C15c) its own turn Q about its head, then the move: m = B [Q | p - Q p + s]
                const float* B = ref;
                float qp[3], out[12];
                for (int i = 0; i < 3; ++i) qp[i] = P.pivot[i] - (rq[i * 3] * P.pivot[0] + rq[i * 3 + 1] * P.pivot[1] + rq[i * 3 + 2] * P.pivot[2]) + sl[i];
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) out[i * 4 + j] = B[i * 4] * rq[j] + B[i * 4 + 1] * rq[3 + j] + B[i * 4 + 2] * rq[6 + j];
                    out[i * 4 + 3] = B[i * 4 + 3] + B[i * 4] * qp[0] + B[i * 4 + 1] * qp[1] + B[i * 4 + 2] * qp[2];
                }
                std::memcpy(m, out, sizeof(out));
            } else {
                float old[12];
                std::memcpy(old, m, sizeof(old));
                part_apply(P.base == kRef ? ref : m, P.axis, P.deg * a, P.pivot, sl, m);
                if (P.drv == kDrvOpen && P.base == kOwn) change_of(old, m, G);
            }
            if (half == 0) {
                if (P.drv == kDrvOpen && !(n >> kDrvOpen & 1u)) std::memcpy(g_open_last, m, sizeof(g_open_last));
                n |= 1u << P.drv;
            }
        }
    }
    if (n && count <= 8) {
        std::memcpy(g_drawn_set, sm, sizeof(float) * 12 * count);
        g_drawn_count = count;
        g_drawn_weapon = weapon;
    }
    return n;  // a bit for each driver whose parts were drawn
}
// [Reload] OpenCylinder / ManualBreak: the gun in hand drawn open by `amount` (0 shut - 1 open); its parts drawn
int open_gun(float* sm, int count, int weapon, float amount) {
    float d[kDrvCount] = {};
    d[kDrvOpen] = amount;
    return gun_parts(sm, count, weapon, d, 1u << kDrvOpen) != 0;
}
std::atomic<uint64_t> g_open_draws{0};
std::atomic<uint64_t> g_sec_open_draws{0};  // [Hands] DualWieldOpen: the second gun's (or the copy's prop's) draws drawn open
// run 6 item 9: the gun in hand whose action the player works (actions::parts_weapon: the lever guns with LeverParts):
// its model's flags (its own fire clip stopped, the gun steadied through John's fire clip); 0 for none
uint8_t worked_flags() {
    const int w = actions::parts_weapon();
    if (w >= 0) return gun_flags(w);
    // [Reload] RevolverControl=cock: the single action's own fire clip stopped (its re-cock and REVOLVER_COCK_FIRE; the
    // player's flick cocks it). Not steadied: the revolver keeps John's recoil
    return actions::hammer_weapon(nullptr) >= 0 ? kStopFireClip : 0;
}
// John's fire clip, for the steadying: the gun controller's fire phase (G +0x24), or within 2 s of the shot (cycle L2: the
// Bolt Action's bolt cycle swung the drawn gun 97 degrees past that phase; the steadying only puts the drawn gun at the
// controller with its rest offset, so a longer hold is harmless)
constexpr double kShotHoldMs = 2000.0;
bool in_fire_clip() { return holster::fire_clip_phase() >= 0.0f || actions::since_shot_ms() < kShotHoldMs; }
std::atomic<uint64_t> g_lever_draws{0}, g_steady_draws{0}, g_steady_misses{0}, g_clip_stops{0};
std::atomic<uint64_t> g_steady_hand[2] = {}, g_steady_xf{0};  // (C14) the steadied draws by John's hand, and transplanted ("skel parts lever")
float g_clip_done = 2.0f;  // a fire clip phase past its end (the gun's own fire clip stopped): writable, set each use
// The rifle held still through John's fire clip ([Reload] LeverParts): the clip swings the gun in the hand (48 degrees
// for the Carbine's shot, "skel stale"), and the drawn gun would swing in the player's hand. Per hand, after every
// change: the drawn gun's root in that hand's controller target T (the IK target: the controller's pose, whatever the
// hand solve and the two-handed aim did with the game's swinging gun), G = T^-1 F_root, learned while no fire clip
// plays; during one, every bone of the drawn gun goes through X = T G_rest F_root^-1 (the player set's space), and for
// 150 ms after it X eases back to none (the hands may have moved meanwhile). Under g_draw_mutex.
struct Steady {
    bool wrist_ok = false, rest_ok = false, on = false;
    float Sw[12];  // the drawn wrist's skinning matrix, this pass (the "skel parts post" check)
    float G[12];   // the drawn gun's root in the target, at rest
    double clip_end_ms = 0;
};
Steady g_steady[2];
// a general 3x4 affine (rows: the 3x3, then the translation column): inverse, product
bool inv34(const float* m, float* o) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-9f)) return false;
    const float k = 1.0f / det;
    const float r[9] = {(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k, (f * g - d * i) * k, (a * i - c * g) * k,
                        (c * d - a * f) * k, (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) o[row * 4 + col] = r[row * 3 + col];
        o[row * 4 + 3] = -(r[row * 3] * m[3] + r[row * 3 + 1] * m[7] + r[row * 3 + 2] * m[11]);
    }
    return true;
}
void mul34(const float* A, const float* B, float* o) {  // o = A B (B first)
    float t[12];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) t[i * 4 + j] = A[i * 4] * B[j] + A[i * 4 + 1] * B[4 + j] + A[i * 4 + 2] * B[8 + j];
        t[i * 4 + 3] = A[i * 4] * B[3] + A[i * 4 + 1] * B[7] + A[i * 4 + 2] * B[11] + A[i * 4 + 3];
    }
    std::memcpy(o, t, sizeof(t));
}
void to_axis_angle(const float* R, float* axis, float* ang);
void axis_angle(const float* a, float ang, float* o);
// The steadying's turn for a gun frame F (3x4) held at T G (T the target, G the frame in it at rest), eased back to none
// as fade goes 0 -> 1 (about F's origin). False when F is singular.
bool steady_x(const float* T, const float* G, const float* F, float fade, float* X) {
    float target[12], fi[12];
    mul34(T, G, target);
    if (!inv34(F, fi)) return false;
    mul34(target, fi, X);
    if (fade > 0.0f) {
        const float Rx[9] = {X[0], X[1], X[2], X[4], X[5], X[6], X[8], X[9], X[10]};
        float ax[3], an = 0, Rf[9];
        to_axis_angle(Rx, ax, &an);
        axis_angle(ax, an * (1.0f - fade), Rf);
        // x' = Rf (x - r) + r + (1 - fade) (X r - r): F's origin r moved by the same share
        const float r[3] = {F[3], F[7], F[11]};
        float xr[3];
        for (int k = 0; k < 3; ++k) xr[k] = X[k * 4] * r[0] + X[k * 4 + 1] * r[1] + X[k * 4 + 2] * r[2] + X[k * 4 + 3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) X[i * 4 + j] = Rf[i * 3 + j];
            X[i * 4 + 3] = r[i] - (Rf[i * 3] * r[0] + Rf[i * 3 + 1] * r[1] + Rf[i * 3 + 2] * r[2]) + (1.0f - fade) * (xr[i] - r[i]);
        }
    }
    return true;
}
// "skel parts arm|<n>" (the moving parts' study): the held gun's set as the game animated it, one draw a frame, before any
// change (under g_draw_mutex)
struct PartsSample {
    double ms;
    float phase;  // the gun controller's fire phase then (below 0: none), and 2 when within kShotHoldMs of a shot
    int count;
    float m[8][12];
    bool post_ok;
    float post[12];  // the reference bone as drawn (after every change), the world less the player's offset
    float left[12];  // the drawn left wrist then (the player's corrected set)
    bool snap_ok;
    float snap[12];  // the front hand's snap: its frame of the gun (the published gun_frame), the same space as post
    // the game's own, before any change (the same space): the gun's object matrix (the record's, as 3x4: its axes as
    // columns), its set's root, and the gun hand's attachment bone (the player's set as animated, its last draw)
    bool game_ok;
    float obj[12], root[12], att[12];
};
float g_att_game[12];  // the gun hand's attachment bone as animated, the player's last set (the world less its offset)
bool g_att_game_ok = false;
constexpr int kPartsMax = 300;
PartsSample g_parts[kPartsMax];
int g_parts_n = 0;
std::atomic<bool> g_parts_rec{false};
uint64_t g_parts_frame = ~0ull;
extern float g_player_offset[3];
void record_parts(const float* sm, int count, uint64_t frame, const float* rc, const float* po) {
    if (count < 1 || count > 8 || g_parts_n >= kPartsMax || g_parts_frame == frame) return;
    g_parts_frame = frame;
    PartsSample& p = g_parts[g_parts_n++];
    p.game_ok = rc && po && g_att_game_ok;
    if (p.game_ok) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) p.obj[i * 4 + j] = rc[j * 4 + i];
            p.obj[i * 4 + 3] = rc[12 + i] - g_player_offset[i];
        }
        std::memcpy(p.root, sm, sizeof(p.root));
        for (int k = 0; k < 3; ++k) p.root[k * 4 + 3] += po[k] - g_player_offset[k];
        std::memcpy(p.att, g_att_game, sizeof(p.att));
    }
    p.ms = log::now_ms();
    p.phase = holster::fire_clip_phase();
    if (p.phase < 0.0f && actions::since_shot_ms() < kShotHoldMs) p.phase = 2.0f;
    p.count = count;
    p.post_ok = false;
    std::memcpy(p.m, sm, sizeof(float) * 12 * count);
    if (g_parts_n >= kPartsMax) g_parts_rec = false;
}
// [Debug] TwoHandShotLog (run 9 item 3, on): a shot while two-handed arms the recording above by itself (when no test
// has it), and its end logs the front wrist's move on the drawn gun (parts_post), with the two-handed blend and the
// front hand's pose at the shot and at the end (the user's "Foregrip hand moves after firing": not reproduced in the
// simulator, within 2 mm with every grip switch)
std::atomic<bool> g_shotlog_cfg{true};
bool g_parts_auto = false;
bool g_parts_manual = false;  // a test's recording ("skel parts arm") not yet read ("post", "dump"): never overwritten
float g_parts_auto_blend = 0.0f;
int g_parts_auto_pose = -1, g_parts_auto_weapon = -1;
std::atomic<uint64_t> g_shotlogs{0};
// D = A^-1 B for two set matrices (3x4 rows: the 3x3, then the translation column), A rigid: R_A^T R_B, R_A^T (t_B - t_A)
void rel_set(const float* A, const float* B, float* D) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) D[i * 4 + j] = A[0 * 4 + i] * B[0 * 4 + j] + A[1 * 4 + i] * B[1 * 4 + j] + A[2 * 4 + i] * B[2 * 4 + j];
        D[i * 4 + 3] = A[0 * 4 + i] * (B[3] - A[3]) + A[1 * 4 + i] * (B[7] - A[7]) + A[2 * 4 + i] * (B[11] - A[11]);
    }
}
std::atomic<int> g_held_set_count{-1}, g_held_set_rm{-1};  // the held item's last set draw: its matrices, its record-matrix flag

Corr pose_corr(const float* Rc, const float* pc, const float* pose) {
    Corr c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float v = 0;
            for (int k = 0; k < 3; ++k) v += pose[k * 3 + i] * Rc[j * 3 + k];  // Rt[i][k] Rc^T[k][j]
            c.A[i * 3 + j] = v;
        }
    for (int i = 0; i < 3; ++i) c.a[i] = pose[9 + i] - (c.A[i * 3] * pc[0] + c.A[i * 3 + 1] * pc[1] + c.A[i * 3 + 2] * pc[2]);
    return c;
}
std::atomic<uint64_t> g_hprop_draws{0}, g_hprop_skinned{0}, g_hprop_rigid{0}, g_copy_muzzle_moved{0}, g_copy_own_skips{0};
std::atomic<uint64_t> g_hprop_hidden{0};  // 2026-10-27: the shells' pool's spares' draws hidden ("skel hprop")
struct HpDiag {  // the last held prop draw ("skel hprop"): its path (1 record matrix, 2 skinned set, 3 rigid), flags, before/target/after
    int path = 0, count = 0;
    uint32_t flags = 0;
    float before[3] = {}, target[3] = {}, after[3] = {}, xb[3] = {}, xa[3] = {};
};
HpDiag g_hp_diag;  // under g_draw_mutex
std::vector<Corr> g_corr;
std::vector<Corr> g_corr_world;  // the latest current set's, in the world (props on the player)
// props by their drawable (record [0x10]): once matched to a bone, a prop keeps it while it stays within 0.5 m of that
// bone, so a fast player (the skeleton read may be a frame ahead of the record) does not lose it (round 5c: the hat
// flickered while sprinting)
struct PropBone {
    uintptr_t drawable;
    int bone;
    uint64_t frame;
    bool head;  // matched within 5 cm of a head bone (the hat): from then on it stays a head prop
    // 2026-10-13 (the user: "In fist fights, npc hats pop in and out"): the draw's instance (draw_instance) the prop was
    // first matched with: another copy of the same model is never this prop (0: none; John's props have none, r13_propowner)
    uintptr_t inst;
    // and a head prop's facing (its matrix's first axis) at its last match: John's hat turns with his head a few degrees
    // a frame, an NPC's hat of the same model in a fist fight faces him; by the model alone it was hidden as his
    float ax[3] = {0.0f, 0.0f, 0.0f};
};
// The item in hand, known by its game matrix (round 7: "the gun in hand turns invisible for a bit now and then"; the
// nearest attachment bone in the game's pose was often a holster, the hip or the head, hidden with the body in the
// forearms mode). Sampled at each visibility build (the update thread) into a ring behind a sequence counter; the
// render thread copies it once a frame. A prop draw whose record matrix is one of them is the held item: it takes
// the hand's correction and is never hidden. [Body] HeldPropFix.
HeldSample g_held_ring[kHeldRing];
uint32_t g_held_next = 0;  // the update thread's
std::atomic<uint32_t> g_held_seq{0};
std::atomic<bool> g_held_fix_cfg{true};
// left-handed: the gun in John's left hand ([Hands] GunInGunHand, TransplantFingers, TransplantPin)
std::atomic<bool> g_gun_hand_cfg{true}, g_pin_cfg{true};
// [Hands] TransplantMirror (round 9: long guns in the left hand sat beside it): the gun drawn in John's left hand is
// the exact mirror of the game's right-hand hold (reflected across its own middle plane, then across the hand's), for
// any cant or yaw of the gun in the hand; off: TransplantPin's shift along the hand's x (exact only for a gun lying
// square in the hand)
std::atomic<bool> g_mirror_cfg{true};
float g_xfer_grip_err = -1.0f;  // the drawn left wrist against the mirrored game right wrist, in the gun's frame (m), the last
// the same with the knuckles (finger_11..41's base joints' centroid), John's drawn hand against the game's gripping
// hand mirrored: neither is what the transplant is built from (the wrist check above is exact by construction in
// mirror mode; review 1), so this one can fail (m, the last; -1 none)
float g_xfer_knuckle_err = -1.0f;
// [Hands] ArmTwist: the arm's roll from the IK's bend plane, not the game's animation; ForearmTwistShare: the forearm's
// roll bone's share of the hand's twist about the forearm
std::atomic<bool> g_twist_cfg{true};
std::atomic<float> g_twist_share{0.5f};
std::atomic<uint64_t> g_twists{0};
float g_fore_roll[2][3] = {{0, 1e9f, -1e9f}, {0, 1e9f, -1e9f}};  // per arm: the forearm's roll correction (deg), last, min, max
std::atomic<int> g_fingers_cfg{1};
// [Hands] CopyGrip (run 6 item 1b, on): while the copy of the sidearm (either model) is out, the free hand holding it
// takes the gun hand's grip, mirrored as the transplant's fingers are (round 12: that hand stayed open round its gun)
std::atomic<bool> g_copy_grip_cfg{true};
std::atomic<float> g_curl[2] = {-1.0f, -1.0f};  // "skel fingers": each hand's fingers' mean turn from its wrist, as drawn
// what the game holds in its hand g, as drawn (the wrist's correction or the transplant), from the latest current set:
// world and the set's space for the props (under g_draw_mutex), and for the aim behind g_hand_seq
Corr g_item_world[2] = {{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}}, {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}}};
Corr g_item_set[2] = {{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}}, {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}}};
bool g_item_xfer = false;  // g_item_* differ from the wrists' (a transplant this frame)
std::atomic<uint64_t> g_xfers{0}, g_xfer_draws{0};
float g_xfer_shift = 0.0f;  // the pin's shift (m), the last
int g_xfer_pin_mode = 0;    // where it was measured: 1 the muzzle (the gun's middle plane), 2 the fist's centre, 3 the attachment
// [Body] HeldPropAtHand: the game culls each object at its own place (the culler tests every entity's box), and it
// places the held gun's prop at its animated hand (FUN_1402fe6d0 -> FUN_1402191c0(prop, W +0x80)): when the head looks
// away from where the game holds it, the gun is not drawn at all (the simulator: none of its draws with the head 20 deg
// off the body's facing). The placement is given the drawn hand's matrix instead (the game thread, through the
// game's own setter, only for the player's item in hand), so the prop is culled where it is drawn. W +0x80 itself
// (the aim, the fire) is untouched.
using ObjSetMatrix_t = uint64_t (*)(uintptr_t obj, const float* m);
ObjSetMatrix_t o_obj_set_matrix = nullptr;
// [Body] LassoAtHands (2026-10-09): the lasso's rope drawn at the drawn hands (hk_lasso_draw, hk_rope_points)
using LassoDraw_t = void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
LassoDraw_t o_lasso_draw = nullptr;
using RopePoints_t = void (*)(uintptr_t, uint64_t, uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t);
RopePoints_t o_rope_points = nullptr;
std::atomic<bool> g_lasso_cfg{true};
thread_local int t_lasso_draw = 0;
std::atomic<uint64_t> g_lasso_draws{0}, g_lasso_moved{0}, g_lasso_points{0};
std::mutex g_lasso_mutex;
float g_lasso_dbg[6][3] = {};  // the last draw: its first and last point (the game's), the game's wrists L R, the first and last drawn
int g_lasso_n = 0, g_lasso_moved_n = 0;
uintptr_t g_gun_place_ret = 0;
std::atomic<bool> g_held_at_hand_cfg{true};
std::atomic<uint64_t> g_lasso_left{0};  // (C12c) the lasso's updates left to the game with its rope out
std::atomic<uintptr_t> g_held_wmgr{0};  // the player's weapon manager (actor +0x70), from the last visibility build
struct Placed {
    uintptr_t W = 0;
    uint64_t build = 0;  // the visibility builds done before it (it belongs to the next one)
    float m[16] = {};
    float A[9] = {}, a[3] = {};
    double ad[3] = {};
    int john = -1;       // the second gun's: John's hand it was placed at
    double ms = 0;       // when (dual.cpp's held effects: this game frame's placement only)
    bool carry = false;  // the second gun's: placed as a carry (place_carry), not a dual-wielded gun
    // [Hands] CarryByForegrip: the carried gun placed rigidly at John's hand follow_j's target (the item in hand's: its
    // put-away still in the game's hand; the second gun's: until its T is made), its draws moved by that target (HeldSample's)
    int follow_j = -1;
    // John's front hand's target as this placement's update read it (body_points): with follow_j always (its draws'
    // move, HeldSample's), the others only for "skel carry trace" (pair_front)
    bool pair_ok = false;
    float pair_t[3] = {}, pair_r[9] = {};
    bool put = false;  // the second gun's: the carried gun placed while the game puts it away (the item not yet holstered)
    float mshift[3] = {};  // the copy's: its shot's move by the two models' muzzles (DualWieldOwnModel; C's axes)
};
Placed g_placed;
std::atomic<uint32_t> g_placed_seq{0};
std::atomic<uint64_t> g_placements{0}, g_vis_builds{0};
alignas(16) float g_place_buf[16];  // the game thread's
// [Hands] DualWield (run 3 item 5, research\run3\dualwield.md 2.2-2.4): the second gun (dual.cpp) placed in John's free
// hand by the game thread (place_secondary), its samples at the visibility build, and the player's skeleton and
// attachment bones for that thread (published by before_scene)
Placed g_sec_placed;
std::atomic<uint32_t> g_sec_seq{0};
HeldSample g_sec_ring[4];
size_t g_sec_next = 0;
std::atomic<uint32_t> g_sec_ring_seq{0};
std::atomic<uintptr_t> g_skel_game{0};
std::atomic<int> g_att_bone[2] = {-1, -1};
std::atomic<uint64_t> g_sec_placements{0}, g_sec_refused{0}, g_sec_draws{0};
std::atomic<int> g_sec_why{0};  // the last refusal: 1 no hand, 2 no hand bone, 3 no grip locator, 4 no hand correction, 5 a write fault
// [Hands] CarryByForegrip: the long gun in hand held two-handed, as placed this update in the front hand's corrected
// attachment bone's frame (T = P C^-1, row vectors; hk_obj_set_matrix), for the hand-over (two_hand_P)
struct TwoHandT {
    uintptr_t W = 0;
    float P[16] = {};  // the gun's place as drawn (g_place_buf: corrected space, as place_carry writes W +0x80)
    int fj = 0;        // John's front hand
    float hand0[3] = {};  // that hand's target then (body_points), for the gun to ride with until its T is made
    float rot0[9] = {};   // and its axes (columns): the gun turns with it too
    bool hand0_ok = false;
    double ms = 0;
};
TwoHandT g_two_T;
// (2026-10-30, round 55, item 8; r299: with the hold learned still, a moving let-go's P, the drawn gun lagging its
// controllers, no longer matched it: the carried gun 4 cm / 10 degrees off) the last capture made with both controllers
// still kTwoStillMs on end (tracking space): a let-go while moving takes the gun's place from it, carried rigidly to the
// front target of now (carry_rigid), the same still moment the carrying hand's hold comes from. Under g_two_T_seq too
TwoHandT g_two_T_still;
bool g_two_T_moving = false;  // the latest capture's controllers not still
std::atomic<uint64_t> g_two_T_still_used{0};  // the hand-overs that took the still capture ("skel carry")
std::atomic<uint32_t> g_two_T_seq{0};
std::atomic<uint64_t> g_two_T_takes{0}, g_carry_placements{0}, g_carry_refused{0};
std::atomic<uint64_t> g_carry_part_draws{0};  // [Reload] CarriedBolt: the carried gun's draws with its bolt drawn ("skel carry")
// (cycle r197: the Bolt Action's hand-over found no two-handed place, "skel carry"): the two-handed updates not captured,
// by why: the game's item hung on its left hand, the place not finite, John's front hand not known; and the captures
// whose fourth column (the game's own data, not used) was not finite
std::atomic<uint64_t> g_two_T_left{0}, g_two_T_nonfinite{0}, g_two_T_nohand{0}, g_two_T_col4{0};
// [Reload] CarriedBolt's check ("skel boltbone", read only): the bolt guns' drawn bolt (bone 2) in the world as last drawn
// with its parts (BoltParts), [0] the gun in hand, [1] the carried gun: its axes as columns and its origin (under
// g_draw_mutex)
struct BoltBone {
    int weapon = -1;
    float R[9] = {};
    float o[3] = {};
    double ms = 0.0;
};
BoltBone g_bolt_bone[2];
// GOAL-weapons-handling item 1b's check (2026-10-28, the review: the aim's barrel against the gun as DRAWN, not against
// barrel_ray itself; read only): the gun in hand's root (bone 0, or a rigid record) as last drawn in an eye's pass (not
// the shadow pre-pass), [0] after the hand's correction and before the steadying, [1] after the steadying (as seen),
// both before GunKick: axes as columns, the origin (world); the body frame; whether the draw matched an older build's
// sample than the newest. Written under g_draw_mutex, read by g_dg_seq (aim.cpp's barrel_step_tick, the game thread)
struct DrawnGun {
    float R[2][9] = {};
    float o[2][3] = {};
    double od[2][3] = {};  // (GOAL-older-items item 21) o's sum in double ("skel shake")
    uint64_t frame = 0;
    double ms = 0.0;
    bool older = false;
    bool ok = false;
    bool copy = false;  // g_dg2's: the copy's prop (else the second gun of another slot)
};
DrawnGun g_dg;
std::atomic<uint32_t> g_dg_seq{0};
std::atomic<uint64_t> g_dg_notes{0};
std::atomic<bool> g_dg_on{false};  // only while "aim barrelstep" is on (the session): the draws untouched otherwise
// (2026-10-28, round 38 check 2's check, read only: "aim barrelstep2") the same for the second gun (another slot's) or the
// copy's prop: its root as last drawn in an eye's pass after its hand's correction, before GunKick ([0] and [1] alike:
// no steadying), written under g_draw_mutex, read by g_dg2_seq (aim.cpp, the XR thread); only while it is on
DrawnGun g_dg2;
std::atomic<uint32_t> g_dg2_seq{0};
std::atomic<uint64_t> g_dg2_notes{0};
std::atomic<bool> g_dg2_on{false};
std::atomic<bool> g_dg2_scope{false};
std::atomic<bool> g_dg2_bolt{false};  // (step 4, answer 2) actions' wish for it (a long bolt second gun's knob)  // (step 4, S5) the scope's own wish for the capture (a scoped second gun), ORed with the test's
// one stage's root: m a 3x4 (axes as columns, then the move); rec: a set under its record's matrix (rows, 4x4), else
// po: the set's offset (the world less it); rows: a rigid record itself (rows 0-2 its axes, 12-14 its place)
void drawn_gun_stage(DrawnGun& d, int st, const float* m, const float* rec, const float* po, const float* rows) {
    for (int k = 0; k < 3; ++k) {
        if (rows) {
            for (int j = 0; j < 3; ++j) d.R[st][k * 3 + j] = rows[j * 4 + k];
            d.o[st][k] = rows[12 + k];
            d.od[st][k] = rows[12 + k];
        } else if (rec) {
            for (int j = 0; j < 3; ++j) d.R[st][k * 3 + j] = m[0 * 4 + j] * rec[0 * 4 + k] + m[1 * 4 + j] * rec[1 * 4 + k] + m[2 * 4 + j] * rec[2 * 4 + k];
            d.o[st][k] = m[3] * rec[k] + m[7] * rec[4 + k] + m[11] * rec[8 + k] + rec[12 + k];
            d.od[st][k] = static_cast<double>(m[3]) * rec[k] + static_cast<double>(m[7]) * rec[4 + k] + static_cast<double>(m[11]) * rec[8 + k] + rec[12 + k];
        } else {
            for (int j = 0; j < 3; ++j) d.R[st][k * 3 + j] = m[k * 4 + j];
            d.o[st][k] = m[k * 4 + 3] + (po ? po[k] : 0.0f);
            d.od[st][k] = static_cast<double>(m[k * 4 + 3]) + (po ? po[k] : 0.0f);
        }
    }
}
// (2026-11-02, GOAL-older-items item 21: optimumbox's "far from the map's middle the body, the hands and the gun shake")
// "skel shake on|off", "skel shake": each body frame's drawn gun root (as seen, [1]) and the frame's game camera, as the
// game has them (floats, world); reported: their moves frame to frame and the gun's against the camera's (with the head
// and the hands still, the floats' own steps). Under g_draw_mutex (drawn_gun_publish's)
struct ShakeRec {
    float gun[3], cam[3];
    double gund[3];          // the gun's place summed in double (its local move plus the set's world offset)
    float tgt[3], actor[3];  // the gun hand's wrist target (the IK's, world) and John's place, this frame
    double wr[3];            // John's right wrist as drawn (its skinning matrix's move plus the set's offset), this frame's
    bool wr_ok;
};
double g_shake_wr[3] = {};  // (the player's draw, render thread) this frame's drawn right wrist
uint64_t g_shake_wr_frame = ~0ull;
constexpr int kShakeN = 1200;
ShakeRec g_shake[kShakeN];
uint64_t g_shake_n = 0, g_shake_frame = ~0ull;
std::atomic<bool> g_shake_on{false};
// [Hands] GunSteady (on; 2026-11-02, GOAL-older-items item 21, after optimumbox's 0.8.11.11 "far-from-origin tremble";
// cycles N9-N13: the camera, the wrist target and John still, the drawn gun moved 0.14 mm a frame at the start (a float's
// step 0.24 mm) and 0.33 at Fort Mercer (0.49): its world place, a float at the world's scale, flipped between steps
// frame to frame; not the hold's learning, not the target): the gun in hand drawn where it was last frame while the gun
// hand's target and the camera stayed still (within 0.02 mm) and its root came within 3.5 float steps of it; else as
// placed. Once a body frame (its first draw), the same shift for every draw of it (both eyes, the shadows). Render thread
std::atomic<bool> g_gun_steady{true};
std::atomic<uint64_t> g_gs_held{0}, g_gs_free{0};
bool g_gs_ok = false;
uint64_t g_gs_frame = ~0ull, g_gs_prev = ~0ull;
int g_gs_count = 0;
double g_gs_out[3] = {}, g_gs_delta[3] = {};
float g_gs_tgt[3] = {}, g_gs_cam[3] = {};
void drawn_gun_publish(const DrawnGun& t, uint64_t frame, bool older) {  // (under g_draw_mutex: one writer)
    if (g_shake_on.load(std::memory_order_relaxed) && frame != g_shake_frame && g_frame.valid) {
        g_shake_frame = frame;
        ShakeRec& r = g_shake[g_shake_n++ % kShakeN];
        for (int k = 0; k < 3; ++k)
            r.gun[k] = t.o[1][k], r.cam[k] = g_frame.cam[12 + k], r.gund[k] = t.od[1][k], r.tgt[k] = g_frame.ik_t[1][k], r.actor[k] = g_frame.actor_world[k],
            r.wr[k] = g_shake_wr[k];
        r.wr_ok = g_shake_wr_frame == frame;
    }
    g_dg_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    g_dg = t;
    g_dg.frame = frame;
    g_dg.ms = log::now_ms();
    g_dg.older = older;
    g_dg.ok = true;
    g_dg_seq.fetch_add(1, std::memory_order_release);
    g_dg_notes.fetch_add(1, std::memory_order_relaxed);
}
void drawn_second_publish(const DrawnGun& t, uint64_t frame, bool copy) {  // (round 38 check 2's: under g_draw_mutex, one writer)
    g_dg2_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    g_dg2 = t;
    g_dg2.frame = frame;
    g_dg2.ms = log::now_ms();
    g_dg2.copy = copy;
    g_dg2.ok = true;
    g_dg2_seq.fetch_add(1, std::memory_order_release);
    g_dg2_notes.fetch_add(1, std::memory_order_relaxed);
}
// [Hands] DualWieldCopy (run 4 item 3): the copy of the gun in hand, placed at John's free hand as place_secondary places
// a second gun (never written to the game), its samples at the visibility build, and its draws
Placed g_copy_placed;
std::atomic<uint32_t> g_copy_seq{0};
HeldSample g_copy_ring[4];
size_t g_copy_next = 0;
std::atomic<uint32_t> g_copy_ring_seq{0};
std::atomic<uint64_t> g_copy_placements{0}, g_copy_refused{0}, g_copy_draws{0}, g_copy_nomatch{0};
std::atomic<uint32_t> g_copy_by_pass[8] = {}, g_copy_stale_by_pass[8] = {};  // run 7 item 1: the copy's draws, as the held gun's
std::atomic<int> g_copy_why{0};
std::atomic<uint64_t> g_sec_near{0};  // draws within 0.3 m of the second gun's newest sample, matched to none
std::atomic<uintptr_t> g_sec_drawable{0};  // the second gun's drawable (rec[0x10]), learned from its exactly matched draws
std::atomic<uint64_t> g_sec_loose{0};      // its draws matched by the drawable near the newest sample (an older placement's)
float g_cuts[3][3] = {};  // "skel cuts" (under g_draw_mutex): the show mode's cut points (left, right) and their midpoint, world
bool g_cuts_ok = false;
float g_sec_near_d = 1e9f, g_sec_near_x = 0.0f;  // the nearest one's distance (m) and its first row's difference
std::atomic<uint64_t> g_held_draws{0}, g_held_frames{0}, g_held_missed{0}, g_held_frame_draws{0}, g_head_refused{0};
// 2026-10-13 (the user: "In fist fights, npc hats pop in and out"): the props matched to the player's bones, by their
// draw's instance owner: the player's skeleton or another (a diagnostic: "skel body")
std::atomic<uint64_t> g_prop_own_skel{0}, g_prop_other_owner{0}, g_prop_head_own{0}, g_prop_head_other{0};
std::atomic<uint64_t> g_head_foreign{0};  // draws of John's hat's model facing away from it (another's hat): left alone
std::atomic<uint64_t> g_head_lost{0};     // draws of John's hat's model facing as it does but matched to no bone (shown)
// run 12 item 11 (reg2's flaky "every frame in hand has the gun drawn"): the missed frames by their place in the hold
// (the frames since the item came into hand: 1st, 2nd, 3rd, 4th-10th, later), and the last few's indices
std::atomic<uint64_t> g_held_miss_at[5] = {};
std::atomic<uint64_t> g_held_miss_last[4] = {};
std::atomic<uint64_t> g_held_newest{0}, g_held_older{0};  // held draws matched to the latest build's sample, to an older one
// "skel lag" (round 9: after vigorous movement the gun shakes or lags behind a bit): the game's gun (the newest sample's
// own matrix) in its hand's attachment bone's frame, against the current set and the previous set (a gun hung on last
// frame's bones keeps a fixed place in the previous one only): the largest move from the first since the reset, mm and
// degrees; and the draws near the gun that matched no sample (they get no hand correction)
struct LagRange {
    bool have = false;
    float p0[3] = {}, R0[9] = {};
    float max_mm = 0, max_deg = 0;
    uint64_t n = 0;
};
LagRange g_lag_now, g_lag_prev;  // under g_draw_mutex
float g_ik_miss_max[2] = {};             // the drawn wrist's largest distance from its target since the reset (m)
// two-handed ("skel lag"): the barrel-in-target the turn used, frame to frame (deg); grips learned and pose flips while
// held; the snap point's drift in the gun's frame during a hold (m); the front wrist's largest miss of the snap (m)
float g_bt_jit_max = 0.0f, g_bt_jit_sum = 0.0f;
uint64_t g_bt_jit_n = 0, g_bt_jit_over = 0;
uint64_t g_two_learns = 0, g_two_flips = 0;
float g_two_drift_max = 0.0f, g_two_miss_max = 0.0f;
// run 6 item 7 ("skel barrel"): the off hand drawn on the open barrels: its draws, the drawn front wrist's worst miss of
// the turned grip point while fully on, and the snap's own largest jump in a frame (the weight's change times the turned
// grip point's distance from the controller's target: the controller's own motion is not in it)
uint64_t g_barrel_snaps = 0;
uint64_t g_part_snaps = 0;                      // [Reload] PartHandSnap: draws fully on the part ("skel partsnap")
float g_part_miss = 0.0f, g_part_miss_max = 0.0f;  // the drawn wrist from the part's handle (m)
// (2026-10-28) the hand's interaction spot as drawn from the handle (m; [0] the part in hand, [1] the carried bolt, fully
// on), its frames, the carried bolt's draws, and the fingers closed on the bolt (their draws, and from what: 0 none, 1 the
// hand's pinch, 2 the other hand's mirrored, 3 the foregrip's, 4 the foregrip's mirrored)
float g_part_spot_miss[2] = {}, g_part_spot_max[2] = {};
uint64_t g_part_spot_n[2] = {}, g_cpart_snaps = 0, g_grasp_frames = 0;
int g_grasp_src = 0, g_grasp_hand = -1;
float g_grasp_curl = -1.0f, g_grasp_src_curl = -1.0f, g_grasp_err = -1.0f;  // (r228) the grasp's measures ("skel partsnap")
std::atomic<bool> g_grasp_cfg{true};  // "skel grasp on|off" (the session, a test's control): the fingers closed on the bolt
std::atomic<int> g_grasp_force{0};    // "skel grasp auto|open|pinch" (the session, a test's control): the pose put (0 the chosen one)
// [Reload] ThumbCock (2026-10-28): each single action's hammer spur (where John's thumb takes it) from its hammer's bind
// origin with the hammer down (the model's frame: x right, y up, z back; m), and the thumb's tip past its last joint
// (thumb_03's) along that bone; tunable for the session ("skel thumb spur <weapon> x y z", "skel thumb tip <m>"). Under
// g_draw_mutex. (r228: the first guesses, (0, 20, 10) mm, moved the spur 15 mm, the real spur twice that) Measured from the models
// (the run 4 extracts, research\eco\frag_guns, gitignored; read only): the hammer bone's vertices, the top of the spur at
// its rear (where a thumb takes it) less the kGunModels pivot: the Cattleman's at (0, 74.2, 36.1) mm, the Schofield's
// (0, 72.3, 24.9), the LeMat's (0, 56.2, 42.5): 49, 32 and 41 mm from the pivot, so 34, 22 and 28 mm of travel over
// the hammer's 40 degrees
// (2026-10-29, round 40: "John's thumb touches the top of the hammer but does not cock it ... the gun also pulls back
// slightly ... the tip of John's thumb bends backward"; r267-r269's pictures across the view) the tip's point 26 mm past
// thumb_03 (at 18 the drawn glove's end reached about a centimetre past it and lay over the hammer), each spur's point 6 mm
// up and 4 mm back of the spur's top (the thumb's middle on it left half the thumb below the spur's top: cocked, the spur
// stood above the thumb), and the gun's roll 8 degrees (15 before; with none the thumb, straight, fell 7-17 mm short)
float g_spur[3][3] = {{0.0f, 0.0528f, 0.0192f}, {0.0f, 0.0375f, -0.0016f}, {0.0f, 0.0356f, 0.0330f}};  // the Cattleman, the Schofield, the LeMat
float g_thumb_tip = 0.026f;
int spur_slot(int w) { return w == 4 ? 0 : w == 5 ? 1 : w == 7 ? 2 : -1; }
std::atomic<bool> g_thumb_hold{false};  // "skel thumb hold on": the thumb kept on the spur (tuning the offsets by eye)
std::atomic<uint64_t> g_thumb_rest_frames{0};  // (round 55) the frames the thumb was kept at its rest after a shot
// (2026-10-31, the off-hand build step 1, R8; round 55.3, the user: "gun pulls back with hammer, only hammer should
// move") 0: r78 actions P4 measured the thumb's tip reaching the spur with no roll (least 0.0 mm, mean 3.1 mm at full
// weight), so round 40's shortfall no longer holds
std::atomic<float> g_thumb_roll{0.0f};  // (r230; 8 from round 40 to 2026-10-31) the gun's roll back in the hand at the thumb's full weight (deg; "skel thumb roll <deg>")
// the gun's hammer row (kGunModels'), whose pivot and turn the spur rides
const GunPart* hammer_part(int w) {
    for (const GunModel& m : kGunModels)
        if (m.weapon == w)
            for (int i = 0; i < m.n; ++i)
                if (m.part[i].drv == kDrvHammer) return &m.part[i];
    return nullptr;
}
// "skel thumb": the last frame's thumb against the spur (world), and a trace of the frames ("skel thumb arm", written to
// the log by "skel thumb trace"): the weight, the drawn hammer, the tip's distance from the spur, the tip in the wrist's frame
struct ThumbRec {
    double ms;
    float wt, hammer, dist, tipw[3];
    float spurw[3];        // the spur in the wrist's frame (its travel as drawn)
    float used[3], side;   // each joint's turn used (thumb_01, 02, 03; deg) and the turn out of the bend plane (deg)
};
constexpr int kThumbRecN = 512;
ThumbRec g_thumb_rec[kThumbRecN];
uint64_t g_thumb_rec_n = 0;
bool g_thumb_rec_on = false;
struct ThumbLast {
    bool ok = false;
    int weapon = -1;
    float wt = 0.0f, hammer = 1.0f, dist = 0.0f, spur[3] = {}, tip[3] = {}, tipw[3] = {}, gun_o[3] = {}, spurw[3] = {}, used[3] = {}, side = 0.0f, held = 0.0f;
    double ms = 0.0;
};
ThumbLast g_thumb_last;
float g_barrel_miss_max = 0.0f, g_barrel_jump_max = 0.0f;
bool g_two_drift_rebase = false;  // "skel lag reset": the drift measured from the next held frame
bool g_bt_jit_rebase = false;     // "skel lag reset": the jitter measured from the next pair of frames
// [Reload] TwoHandedSteady (round 9: "the guns shake or lag behind a bit" moving the gun around on the foregrip): the
// two-handed turn takes the drawn barrel in the gun hand's target frame as measured here, from one frame's own correction
// and gun (aim.cpp's barrel_in_target pairs the last render frame's correction with the game update's gun: swinging the
// held gun it moved up to 11 degrees frame to frame, and the turn followed it); and while the front hand holds the gun
// its grip is neither learned again nor switched between the game's aim and hold poses (the snap point slid up to 96 mm
// along the gun)
std::atomic<bool> g_same_frame_cfg{true};
// [Reload] TwoHandedSteer (2026-10-15, the user: "an option for disable the off hands influence on gun position, allow it
// to hold, but only cosmetically"): 1 = the front hand on the foregrip turns the gun toward it (as before); 0 = John's
// front hand is still drawn holding the gun, but the gun follows the gun hand alone.
std::atomic<bool> g_two_steer_cfg{true};
// "skel stock" (run 15, a test, read only): the line from the gun hand's controller grip to the front hand's (world,
// unit) while [Reload] PhysicalStock holds a long gun, the line the calibration lays the barrel along (the IK wrist
// targets are offset back along each hand's own turned target, so their line is not the stock's). The render thread
// writes it each such frame.
std::atomic<float> g_stock_line[3] = {};
std::atomic<float> g_stock_span{0.0f};
std::atomic<double> g_stock_line_ms{0.0};
// [Reload] LeverControl=lock (actions::lever_lock): the gun hand's controller orientation (the XR local space, so a turn
// of the view still turns the gun with you) held from when the lock began, its position still the hand's; let go, it
// eases back to the hand over kLockEaseMs. The render thread only.
constexpr double kLockEaseMs = 120.0;
// [Reload] PhysicalStock: a rotation vector (degrees) as a row-major rotation; false (identity) under 0.01 degrees
bool stock_matrix(const float deg[3], float C[9]) {
    const float a = std::sqrt(deg[0] * deg[0] + deg[1] * deg[1] + deg[2] * deg[2]);
    for (int i = 0; i < 9; ++i) C[i] = i % 4 == 0 ? 1.0f : 0.0f;
    if (a <= 0.01f) return false;
    const float ax[3] = {deg[0] / a, deg[1] / a, deg[2] / a};
    axis_angle(ax, a * 0.0174532925f, C);
    return true;
}
// the calibration: U the target as the gun was drawn on it (R C0), G the drawn gun's axes (world; columns x right, y up,
// z back), d the gun hand's controller to the front hand's (world): the new turn C0 X (X = D F^T in U's frame) as a
// rotation vector in degrees. False: d too short, or the gun's barrel along the world's up.
bool stock_solve(const float U[9], const float C0[9], const float G[9], const float d[3], float out[3]) {
    auto to_u = [&](const float* v, float* o) {  // U^T v
        for (int k = 0; k < 3; ++k) o[k] = U[k] * v[0] + U[3 + k] * v[1] + U[6 + k] * v[2];
    };
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl < 0.08f) return false;
    const float up[3] = {0.0f, 1.0f, 0.0f}, gb[3] = {-G[2], -G[5], -G[8]}, gu[3] = {G[1], G[4], G[7]};
    float F0[3], F1[3], F2[3], D0[3], D1[3], D2[3];  // the gun's (barrel, top, side) now and wanted, in U's axes
    to_u(gb, F0);
    to_u(gu, F1);
    to_u(d, D0);
    to_u(up, D1);
    auto ortho = [](float* x, float* y, float* z) {
        const float xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (xl < 1e-6f) return false;
        for (int k = 0; k < 3; ++k) x[k] /= xl;
        const float dp = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
        for (int k = 0; k < 3; ++k) y[k] -= dp * x[k];
        const float yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        if (yl < 0.05f) return false;  // (the top nearly along the barrel: no roll to learn)
        for (int k = 0; k < 3; ++k) y[k] /= yl;
        cross(x, y, z);
        return true;
    };
    if (!ortho(F0, F1, F2) || !ortho(D0, D1, D2)) return false;
    float X[9], Cn[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) X[r * 3 + c] = D0[r] * F0[c] + D1[r] * F1[c] + D2[r] * F2[c];
    mul3(C0, X, Cn);
    const float tr = std::clamp((Cn[0] + Cn[4] + Cn[8] - 1.0f) * 0.5f, -1.0f, 1.0f);
    const float ang = std::acos(tr);
    const float v[3] = {Cn[7] - Cn[5], Cn[2] - Cn[6], Cn[3] - Cn[1]};
    const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (ang > 1e-4f && vl < 1e-6f) return false;  // (a half turn: the axis not in the skew part)
    for (int k = 0; k < 3; ++k) out[k] = vl > 1e-6f ? v[k] / vl * ang * 57.29578f : 0.0f;
    return true;
}
// q0 to q1 by t (normalised linear, the short way): LeverControl's easings (lock and front)
void nlerp_q(const float* q0, const float* q1, float t, float* out) {
    const float s = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3] < 0.0f ? -1.0f : 1.0f;
    float n = 0.0f;
    for (int i = 0; i < 4; ++i) {
        out[i] = q0[i] * (1.0f - t) + q1[i] * s * t;
        n += out[i] * out[i];
    }
    n = n > 1e-12f ? 1.0f / std::sqrt(n) : 0.0f;
    for (int i = 0; i < 4; ++i) out[i] *= n;
}
// a row-major rotation as x y z w, and back
void mat_to_q(const float* m, float* q) {
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (m[7] - m[5]) / s, q[1] = (m[2] - m[6]) / s, q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[7] - m[5]) / s, q[0] = 0.25f * s, q[1] = (m[1] + m[3]) / s, q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[2] - m[6]) / s, q[0] = (m[1] + m[3]) / s, q[1] = 0.25f * s, q[2] = (m[5] + m[7]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[3] - m[1]) / s, q[0] = (m[2] + m[6]) / s, q[1] = (m[5] + m[7]) / s, q[2] = 0.25f * s;
    }
}
void q_to_mat(const float* q, float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w), m[4] = 1 - 2 * (x * x + z * z), m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w), m[7] = 2 * (y * z + x * w), m[8] = 1 - 2 * (x * x + y * y);
}
// (2026-10-28) LeverControl=hold one-handed (pos: actions::lever_hold_lock without the front hand's carry): the controller's
// place (p, the XR local space) held with its orientation, and eased back with it (qo, po: what the arm follows)
void gun_lock(const float* q, const float* p, bool pos, float* out, float* po) {
    static float held[4] = {0, 0, 0, 1}, held_p[3] = {}, last_q[4] = {0, 0, 0, 1}, last_p[3] = {};
    static bool on = false, with_pos = false;
    static double off_ms = -1e9;
    const double now = log::now_ms();
    const bool lk = actions::lever_lock() || pos;
    if (lk && !on) {  // (the review: locked again within the ease back, the pose drawn now, not the controller's)
        const bool easing = now - off_ms < kLockEaseMs;
        std::memcpy(held, easing ? last_q : q, sizeof(held));
        std::memcpy(held_p, easing && with_pos ? last_p : p, sizeof(held_p));
        with_pos = pos;
    }
    if (!lk && on) off_ms = now;
    on = lk;
    const float t = lk ? 0.0f : static_cast<float>((now - off_ms) / kLockEaseMs);
    if (t >= 1.0f) {
        std::memcpy(out, q, sizeof(float) * 4);
        std::memcpy(po, p, sizeof(float) * 3);
    } else {
        nlerp_q(held, q, t, out);
        for (int k = 0; k < 3; ++k) po[k] = with_pos ? held_p[k] + (p[k] - held_p[k]) * t : p[k];
    }
    std::memcpy(last_q, out, sizeof(last_q));
    std::memcpy(last_p, po, sizeof(last_p));
}
// [Reload] LeverControl=front (run 14, after optimumbox's LeverSteady, 0.8.10.4): held two-handed with the lever to work,
// the gun hand's target (R, t: rotation and position, world) carried by the front hand's (Rf, tf) as it was on it when
// the lever was wanted; worked (or let go), eased back onto the gun hand's own over kLockEaseMs, from where the front hand
// carries it now (not a world pose left behind: the review, a gallop). Either hand untracked: no carry, no ease (tracked
// false; R and t not touched). The render thread only.
void gun_front(bool tracked, bool want, const float* Rf, const float* tf, float* R, float* t) {
    static bool carried = false;
    static float rel_r[9], rel_t[3];
    static double free_ms = -1e9;
    const double now = log::now_ms();
    if (!tracked) {  // (the review: a stale carry after a tracking loss)
        carried = false;
        free_ms = -1e9;
        return;
    }
    float cr[9], ct[3];  // the carried pose now
    auto carried_pose = [&]() {
        mul3(Rf, rel_r, cr);
        for (int k = 0; k < 3; ++k) ct[k] = tf[k] + Rf[k * 3] * rel_t[0] + Rf[k * 3 + 1] * rel_t[1] + Rf[k * 3 + 2] * rel_t[2];
    };
    if (want && !carried) {  // the gun hand's target in the front hand's frame: Rf^T R, Rf^T (t - tf)
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) rel_r[r * 3 + c] = Rf[r] * R[c] + Rf[3 + r] * R[3 + c] + Rf[6 + r] * R[6 + c];
            rel_t[r] = Rf[r] * (t[0] - tf[0]) + Rf[3 + r] * (t[1] - tf[1]) + Rf[6 + r] * (t[2] - tf[2]);
        }
        carried = true;
        log::info("[body] lever: the gun carried by the front hand while the lever is worked (LeverControl=%s)", actions::lever_hold_lock() ? "hold" : "front");
    } else if (!want && carried) {
        carried = false;
        free_ms = now;
    }
    if (carried) {
        carried_pose();
        std::memcpy(R, cr, sizeof(cr));
        std::memcpy(t, ct, sizeof(ct));
    } else if (now - free_ms < kLockEaseMs) {  // back onto the gun hand's own target, from the front hand's carry now
        const float a = static_cast<float>((now - free_ms) / kLockEaseMs);
        carried_pose();
        float q0[4], q1[4], q[4];
        mat_to_q(cr, q0);
        mat_to_q(R, q1);
        nlerp_q(q0, q1, a, q);
        q_to_mat(q, R);
        for (int k = 0; k < 3; ++k) t[k] = ct[k] * (1.0f - a) + t[k] * a;
    }
}
// [Reload] SnapToDrawnGun (run 9 item 3, the user: "Foregrip hand moves after firing, ending up clipped in the gun or
// holding air"): with FixedGunGrip the drawn gun is the learned aiming hold on the animated wrist, while the front hand's
// snap and the two-handed turn's barrel took the game's own gun, which the fire clip moves in the game's hand (8-11 mm
// and 11 degrees at the grip after each Carbine shot in the user's logs: 60-75 mm at the foregrip). On: both from the
// gun as drawn, and FixedGunGrip's learning paused through the fire clip. Off, or FixedGunGrip off: as before
std::atomic<bool> g_snap_drawn_cfg{true};
std::atomic<uint64_t> g_snap_drawn_frames{0};
// "skel two clipshared on|off" (a test only, off): the snap's fire-clip hold one for both sets again, as before the
// 2026-10-10 fix (the A/B of tests/sim/r10_horsefore.py)
std::atomic<bool> g_clip_shared_test{false};
bool fixed_grip_on();  // [Hands] FixedGunGrip (defined with it below)
// [Reload] SnapToDrawnGun: a sample's gun as drawn: its matrix's frame (Gm, gm_o in the set's space) with its placement
// at the drawn hand (A, ad: held_delta's) undone
void unplace(const HeldSample& h, const float* off, const float* Gm, const float* gm_o, float* Gu, float* gu_o) {
    float ut[9], ua[3], d[3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) ut[i * 3 + j] = h.A[j * 3 + i];
    for (int k = 0; k < 3; ++k)
        ua[k] = static_cast<float>(h.ad[k] - off[k] + (static_cast<double>(h.A[k * 3]) * off[0] + static_cast<double>(h.A[k * 3 + 1]) * off[1] +
                                                       static_cast<double>(h.A[k * 3 + 2]) * off[2]));
    mul3(ut, Gm, Gu);
    for (int k = 0; k < 3; ++k) d[k] = gm_o[k] - ua[k];
    for (int k = 0; k < 3; ++k) gu_o[k] = ut[k * 3] * d[0] + ut[k * 3 + 1] * d[1] + ut[k * 3 + 2] * d[2];
}
float g_bt_body[3] = {0.0f, 0.174f, -0.985f};  // under g_draw_mutex
uint64_t g_bt_body_frame = 0;
bool g_bt_body_ok = false;
uint64_t g_ik_miss_frames[2] = {}, g_ik_frames[2] = {};  // frames the wrist missed its target by over 1 cm, frames solved
uint64_t g_lag_frame = ~0ull;
std::atomic<uint64_t> g_held_nearmiss{0};
// the last near miss: 1 a prop draw (its set's count), 2 a rigid draw outside the samples' box, 3 a rigid draw in it;
// its drawable, its distance from the newest sample (mm), and its first row against the sample's (the cosine)
struct NearMiss {
    int path = 0, count = 0;
    uintptr_t drawable = 0;
    float mm = 0, cos_x = 0;
};
NearMiss g_nearmiss_last;
void note_nearmiss(int path, int count, const void* drawable, const float* rc, const float* hm) {
    g_held_nearmiss.fetch_add(1, std::memory_order_relaxed);
    NearMiss m;
    m.path = path;
    m.count = count;
    m.drawable = reinterpret_cast<uintptr_t>(drawable);
    const float dx = rc[12] - hm[12], dy = rc[13] - hm[13], dz = rc[14] - hm[14];
    m.mm = 1000.0f * std::sqrt(dx * dx + dy * dy + dz * dz);
    const float la = std::sqrt(rc[0] * rc[0] + rc[1] * rc[1] + rc[2] * rc[2]), lb = std::sqrt(hm[0] * hm[0] + hm[1] * hm[1] + hm[2] * hm[2]);
    m.cos_x = la > 1e-6f && lb > 1e-6f ? (rc[0] * hm[0] + rc[1] * hm[1] + rc[2] * hm[2]) / (la * lb) : 0.0f;
    g_nearmiss_last = m;  // a diagnostic: a torn read is harmless
}
uint64_t g_corr_frame = ~0ull;  // the frame g_corr_world was last computed for (render thread)
// [Hands] StaleGunFix (2026-10-15, on; the user: the gun "shakes slightly when held still and more dramatically when
// moved" after being drawn outside and taken indoors): there, every frame one of the gun's draws in the eye's depth
// pre-pass (pass 0) came before the player's set, so it took last frame's correction: the gun's depth and shadow
// collector a frame behind the gun drawn after it. Such a draw is now put where the gun was drawn last frame
// (gun_prev_fix's cache), moved by the wrist target's own move since (this frame's targets are made before the scene:
// g_frame): stale_place below. (Not last frame's correction on this frame's game pose: that pose already moves with
// John, and the wrist target too: walking would count twice.)
std::atomic<bool> g_stale_fix{true};
std::atomic<uint64_t> g_stale_shifted{0};
std::atomic<uint64_t> g_stale_shifted_other{0};  // (F10) of them the second gun's and the copy-as-prop's
// (F10, a diagnostic) the second gun's early draws not moved, by why: 0 a set under its record's matrix, 1 put_by_target,
// 2-7 stale_place's refusal (t_sp_why 1-6), 8 a rigid draw's (any refusal)
std::atomic<uint64_t> g_sec_unmoved[9] = {};
bool g_corr_ik_ok[2] = {false, false};  // the wrist targets the correction was made with (render thread)
float g_corr_ikt[2][3] = {}, g_corr_ikr[2][9] = {};
std::atomic<uint64_t> g_held_stale{0};  // held draws made before this frame's first player set (last frame's correction)
std::atomic<uint32_t> g_held_by_pass[8] = {}, g_stale_by_pass[8] = {};  // by DrawVisEntity's pass argument (& 7)
// run 6 item 1c: the same for the second gun's draws and the other sidearm's own model (held_prop slot 1)
std::atomic<uint32_t> g_other_by_pass[8] = {}, g_other_stale_by_pass[8] = {};
thread_local uint64_t t_draw_pass = 0;
float g_shake_diag[8] = {1e9f, 1e9f, 1e9f, -1e9f, -1e9f, -1e9f, 1e9f, -1e9f};  // the attachment in the wrist: min xyz, max xyz (mm), turn min, max (deg)
// "skel two": the game's left hand on the held gun (under g_draw_mutex)
struct TwoDiag {
    float wrist_game[3], palm_game[3], wrist_placed[3], palm_placed[3], ik[3], ik_hold[3];
    bool aiming, placed;
    uint64_t frame;
};
TwoDiag g_two_diag_m{};
// The game's front-hand grip on its long gun, learned from the animated left wrist while it is on the gun (within 6 cm
// of the weapon's IK offsets): its position and orientation in the gun's frame (x right, y up, z back), [0] the
// lowered pose's (IKOffsetHold), [1] the aiming pose's (IKOffset). Under g_draw_mutex.
struct GripRel {
    bool valid = false;
    float p[3] = {};
    float O[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    uintptr_t W = 0;  // the gun it was learned on
    float ik[6] = {};  // and that gun's IKOffset, IKOffsetHold
    bool same_gun(const HeldSample& h) const { return W == h.W && !std::memcmp(ik, h.ik, 12) && !std::memcmp(ik + 3, h.ik_hold, 12); }
};
// Round 8: "other two-handed weapons need the same; some do not have a pose". The IK offsets are one value per weapon
// class (every long gun's base file), so on some weapons the game's hand never comes within the gate. The grips are
// kept per weapon (the eWeapon, across redraws); a weapon without one takes, in order, its other pose's, the last grip
// learned on any long gun (the template), or the built-in one (the Carbine's, measured), with the hand kept where it
// took hold along the barrel.
constexpr int kGripW = 40;
std::atomic<bool> g_fallback_cfg{true};  // [Reload] GripFallback
GripRel g_grips[kGripW][2];  // [eWeapon][0 lowered, 1 aiming]
GripRel g_grip_tpl[2];       // the last grip learned on any long gun
int g_grip_src = 0;          // what the current grip is: 0 the weapon's, 1 the template, 2 the built-in, 3 a sibling's
std::atomic<int> g_grip_src_pub{-1}, g_grip_pose_pub{-1};  // for grip_source_name (other threads)
std::atomic<bool> g_sawed_grip{true};  // [Reload] SawedOffGrip (run 8 item 2)
std::atomic<bool> g_steady_grip{true};  // [Reload] SteadyRing (run 8 item 2): a learned grip the mean of its first 30 samples, then kept
int g_grip_n[40][2] = {};               // the samples in each weapon's pose's grip (kGripW weapons)
// [Weapon.<Gun>] FrontHandPose (2026-10-09): 0 automatic (the game's state picks the hold), 1 the lowered hold, 2 the
// aiming hold (the default since run 9), each with the fingers the game's hand had in it (relative to its wrist, captured with the hold's learning)
std::atomic<int> g_front_pose[kGripW] = {};
struct FingerRel {
    bool valid = false;
    bool has[15] = {};
    float M[15][9] = {};
    float t[15][3] = {};
};
FingerRel g_front_fingers[kGripW][2];
// (2026-11-01, GOAL-offhand-changes C3) the thumb on each sidearm's hammer spur, as the gun in hand's ThumbCock drew it (its
// three bones relative to the wrist, by John's hand), for the second gun's or the copy's cock. The draw mutex's
constexpr int kThumbSnapW = 8;
FingerRel g_thumb_snap[kThumbSnapW][2];
std::atomic<uint64_t> g_sthumb_frames{0}, g_thumb_snaps{0};
std::atomic<uint32_t> g_front_seen[kGripW] = {};  // bit 0/1: the lowered/aiming hold learned; bit 2/3: its fingers
std::atomic<uint64_t> g_front_finger_frames{0};
// a hand's fingers relative to its wrist in a bone set (12 floats a bone: [R | t] row-major), none for a collapsed hand
// [Hands] CarryByForegrip: the front hand's fingers on the foregrip, fully two-handed (the draw mutex's)
FingerRel g_carry_fingers;
constexpr int kCarrySettleMax = 150;     // the holstered updates T waits at most for the front bone to settle
// (2026-10-30, round 55, the user: "When releasing main hand and keeping two handed gun in off hand, if off hand is moving
// position can still end up messed up") and the carrying controller still over the same window, in its own tracking
// space (the review: not the world target, which a walk, a ride or a turn moves): its place within kCarrySettleStill of
// the newest and its turn within kCarrySettleStillTurn (rad). A hand moving steadily drew the bone a steady distance behind
// its target: the candidate T then held still too, with that lag in it, and was kept. While it moves, kCarrySettleMax
// gives way to kCarrySettleMaxMoving
constexpr double kCarrySettleStill = 0.01;      // m
constexpr double kCarrySettleStillTurn = 0.02;  // rad (about 1.1 degrees)
constexpr int kCarrySettleMaxMoving = 900;      // the updates T waits at most while the hand keeps moving (about 15 s)
std::atomic<uint64_t> g_carry_settle_moving{0};  // the settled candidates passed over because the hand moved
constexpr int kCarrySettleWin = 10;      // settled: the candidate T within kCarrySettleTol over this many updates
constexpr double kCarrySettleTol = 0.003;  // m: its translation (in the bone's frame)
constexpr double kCarrySettleTurn = 0.01;  // each of its axes (unit rows: about 0.6 degrees)
std::atomic<int> g_carry_settle_n{-1};   // the last carry's: the updates it waited ("skel carry")
std::atomic<uint64_t> g_carry_puts{0};   // the carried gun's put-away updates placed at the front hand ("skel carry")
std::atomic<uint64_t> g_carry_put_states[8] = {};  // the put-away's updates out of the hand, by the item's state (+0x24) read
std::atomic<bool> g_carry_old{false};    // "skel carry old on|off" (carry_old_test)
std::atomic<bool> g_carry_olddelta{false};  // "skel carry olddelta on|off": the put-away's draws by the wrist's correction (c4279ad's; a control)
std::atomic<double> g_carry_trace_until{0.0};  // "skel carry trace <ms>" (carry_trace_tick): until when
std::atomic<uint64_t> g_carry_finger_frames{0};
// 2026-10-27 (cycle r171; [Reload] RoundInHand): the free hand's pinch, as the game draws it with a sidearm in the other
// hand (its thumb and index tips under 3.5 cm apart), learned from those frames; while that hand holds a round (or
// the shells' pool's handful) with a LONG gun in the other, the game draws it flat (the long gun's front-hand fingers,
// the thumb tucked: the round lay inside the glove), so the pinch is put on it instead, and round_draw places the
// round in it as with a revolver. Until a pinch is learned the hand stays flat and body_points says so (round_flat:
// round_draw's fallback along the index finger). The draw mutex's, as g_carry_fingers
FingerRel g_round_fingers[2];
bool g_round_flat[2] = {false, false}, g_round_pinch_put[2] = {false, false};  // this frame's (slot 0), published with the tips
std::atomic<uint64_t> g_round_pinch_frames{0}, g_round_pinch_learned{0};
FingerRel finger_rel(const float* s, int nb, int hand) {
    FingerRel r;
    const int wb = g_rig.wrist[hand];
    if (wb < 0 || wb >= nb) return r;
    const float* w = s + wb * 12;
    float WR[9], wt[3];
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) WR[i * 3 + k] = w[i * 4 + k];
        wt[i] = w[i * 4 + 3];
    }
    if (std::sqrt(WR[0] * WR[0] + WR[3] * WR[3] + WR[6] * WR[6]) < 0.5f) return r;
    int got = 0;
    for (int i = 0; i < 15; ++i) {
        const int fb = g_rig.finger[hand][i];
        if (fb < 0 || fb >= nb) continue;
        const float* m = s + fb * 12;
        for (int a = 0; a < 3; ++a) {  // W^T F, W^T (t_f - t_w)
            for (int b = 0; b < 3; ++b)
                r.M[i][a * 3 + b] = WR[0 * 3 + a] * m[0 * 4 + b] + WR[1 * 3 + a] * m[1 * 4 + b] + WR[2 * 3 + a] * m[2 * 4 + b];
            r.t[i][a] = WR[0 * 3 + a] * (m[3] - wt[0]) + WR[1 * 3 + a] * (m[7] - wt[1]) + WR[2 * 3 + a] * (m[11] - wt[2]);
        }
        r.has[i] = true;
        ++got;
    }
    r.valid = got >= 10;
    return r;
}
// (2026-10-28) a hand's fingers (finger_rel's, relative to its wrist) as the other hand's: conjugated by the rest pose's
// mirror as the transplant's fingers are (M' = Q M Q, t' = Q (M m + t) + m, with Q = I - 2 n n^T and m = 2 c n). Only with
// g_rig.mirror_ok
FingerRel mirrored_rel(const FingerRel& x) {
    FingerRel o;
    o.valid = x.valid;
    const float* nn = g_rig.mir_n;
    float Q[9], mm[3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) Q[i * 3 + j] = (i == j ? 1.0f : 0.0f) - 2.0f * nn[i] * nn[j];
        mm[i] = 2.0f * g_rig.mir_c * nn[i];
    }
    for (int i = 0; i < 15; ++i) {
        if (!x.has[i]) continue;
        float t1[9], v[3];
        mul3(Q, x.M[i], t1);
        mul3(t1, Q, o.M[i]);
        for (int k = 0; k < 3; ++k) v[k] = x.M[i][k * 3] * mm[0] + x.M[i][k * 3 + 1] * mm[1] + x.M[i][k * 3 + 2] * mm[2] + x.t[i][k];
        for (int k = 0; k < 3; ++k) o.t[i][k] = Q[k * 3] * v[0] + Q[k * 3 + 1] * v[1] + Q[k * 3 + 2] * v[2] + mm[k];
        o.has[i] = true;
    }
    return o;
}
// (2026-10-28) the pose a hand on a bolt closes in (r228: the learned pinch, its fingers flat, opened the hand on the knob):
// the game's own hand closed on the gun, the foregrip's fingers (g_carry_fingers, John's left; mirrored for his right),
// only without them that hand's pinch (else the other hand's mirrored); null without any. src: 3, 4, 1, 2 (g_grasp_src's).
// The draw mutex's (g_carry_fingers, g_round_fingers)
// (2026-10-29, round 40: "Bolt snap pose doesn't look right, it's just a open hand on the bolt") hand h's pose's four
// fingers closed further: each joint's own turn from its parent bone (the first's from the wrist) times k, at most cap
// (rad). The set's matrices are skinning matrices (a bind point j drawn at M j + t, as jdrawn), the bind's turns none
// ("skel grasp open" puts the bind's fingers), so a joint's turn is its own from the bind's and pivots about its bind place:
// M' = M_p' L', t' = (M_p' j + t_p') - M' j (r272: about the origin, the index tip went 0.9 m off). The thumb (bones 0-2)
// as it was: the pinch keeps it on the knob
FingerRel curled(const FingerRel& x, int h, float k, float cap) {
    FingerRel o = x;
    if (h < 0 || h > 1) return o;
    for (int f = 1; f < 5; ++f) {
        float PM[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // the parent's turn as it was (the wrist: none)
        float NM[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, Nt[3] = {0, 0, 0};  // and as it is now
        for (int j = 0; j < 3; ++j) {
            const int i = f * 3 + j, bone = g_rig.finger[h][i];
            if (!x.has[i] || bone < 0 || static_cast<size_t>(bone) * 3 + 2 >= g_rig.jbind.size()) break;
            const float* jb = &g_rig.jbind[static_cast<size_t>(bone) * 3];
            float PMt[9], L[9], ax[3], an = 0.0f, Ln[9], M[9], t[3];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) PMt[r * 3 + c] = PM[c * 3 + r];
            mul3(PMt, x.M[i], L);  // its own turn from its parent
            to_axis_angle(L, ax, &an);
            axis_angle(ax, std::fmin(an * k, cap), Ln);
            mul3(NM, Ln, M);
            for (int r = 0; r < 3; ++r)  // the joint stays on its parent as drawn now
                t[r] = NM[r * 3] * jb[0] + NM[r * 3 + 1] * jb[1] + NM[r * 3 + 2] * jb[2] + Nt[r] - (M[r * 3] * jb[0] + M[r * 3 + 1] * jb[1] + M[r * 3 + 2] * jb[2]);
            std::memcpy(PM, x.M[i], sizeof(PM));
            std::memcpy(NM, M, sizeof(NM));
            std::memcpy(Nt, t, sizeof(Nt));
            std::memcpy(o.M[i], M, sizeof(M));
            std::memcpy(o.t[i], t, sizeof(t));
        }
    }
    return o;
}
// (r273's pictures, the turns about each joint: at 1 two fingertips stood straight out past the bolt, at 1.4 one, at 1.8 the
// fingers closed round it; r272's knob check: its tips' middle 8 mm from the knob)
std::atomic<float> g_grasp_curl_k{1.8f};  // "skel grasp curl <k>" (the session, tuning by the pictures): curled()'s k on the bolt's pose
const FingerRel* grasp_pose(int h, FingerRel* scratch, int* src) {
    *src = 0;
    if (h < 0 || h > 1) return nullptr;
    if (h == 0 && g_carry_fingers.valid) {
        *src = 3;
        return &g_carry_fingers;
    }
    if (h == 1 && g_rig.mirror_ok && g_carry_fingers.valid) {
        *scratch = mirrored_rel(g_carry_fingers);
        *src = 4;
        return scratch;
    }
    if (g_round_fingers[h].valid) {
        *src = 1;
        return &g_round_fingers[h];
    }
    if (g_rig.mirror_ok && g_round_fingers[1 - h].valid) {
        *scratch = mirrored_rel(g_round_fingers[1 - h]);
        *src = 2;
        return scratch;
    }
    return nullptr;
}
// (r230: the knob at the wrist, InteractOffset 0, a hand's width from the fist in the pictures) where an object sits in
// that pose: the middle of its thumb's last joint and its index's (thumb_03, finger_13: either side of what the hand
// holds), from the wrist's joint, in the bind's frame (the drawn wrist's turn takes it to the world). False without both
bool grasp_point(int h, const FingerRel& fr, float out[3]) {
    const int wb = g_rig.wrist[h], t3 = g_rig.finger[h][2], i3 = g_rig.finger[h][5];
    const int nb = static_cast<int>(g_rig.jbind.size() / 3);
    if (wb < 0 || t3 < 0 || i3 < 0 || wb >= nb || t3 >= nb || i3 >= nb || !fr.valid || !fr.has[2] || !fr.has[5]) return false;
    float q[2][3];
    const int bi[2] = {t3, i3}, fi[2] = {2, 5};
    for (int j = 0; j < 2; ++j) {  // the joint where the pose puts it, relative to the wrist's skinning: M j + t
        const float* jb = &g_rig.jbind[static_cast<size_t>(bi[j]) * 3];
        const float* M = fr.M[fi[j]];
        for (int k = 0; k < 3; ++k) q[j][k] = M[k * 3] * jb[0] + M[k * 3 + 1] * jb[1] + M[k * 3 + 2] * jb[2] + fr.t[fi[j]][k];
    }
    const float* jw = &g_rig.jbind[static_cast<size_t>(wb) * 3];
    for (int k = 0; k < 3; ++k) out[k] = 0.5f * (q[0][k] + q[1][k]) - jw[k];
    return std::isfinite(out[0] + out[1] + out[2]);
}
GripRel g_grip_builtin[2];   // filled at first use (built_in_grips)
GripRel g_grip_sawed[2];     // the Sawed-off's (run 8 item 2; built_in_grips)
void built_in_grips() {
    static bool done = false;
    if (done) return;
    done = true;
    // the Carbine's grips in the simulator (skel two), x right, y up, z back of the gun
    const float p0[3] = {-0.084f, 0.098f, -0.156f}, p1[3] = {-0.069f, -0.003f, -0.183f};
    // the hand's turn in the gun's frame, measured in the simulator (the Carcano's lowered grip, close to the Carbine's)
    const float O0[9] = {-0.8188f, -0.3009f, 0.4889f, 0.3781f, -0.9235f, 0.0647f, 0.4321f, 0.2378f, 0.8699f};
    const float O1[9] = {-0.8188f, -0.3009f, 0.4889f, 0.3781f, -0.9235f, 0.0647f, 0.4321f, 0.2378f, 0.8699f};
    std::memcpy(g_grip_builtin[0].p, p0, sizeof(p0));
    std::memcpy(g_grip_builtin[1].p, p1, sizeof(p1));
    std::memcpy(g_grip_builtin[0].O, O0, sizeof(O0));
    std::memcpy(g_grip_builtin[1].O, O1, sizeof(O1));
    g_grip_builtin[0].valid = g_grip_builtin[1].valid = true;
    // run 8 item 2: the Sawed-off, the Double-barrel's grips (skel two in the simulator, the run of 2026-10-05 14:58:08:
    // lowered (-0.080 0.008 -0.137), aiming (-0.061 -0.003 -0.165), the grip turn as above)
    const float s0[3] = {-0.080f, 0.008f, -0.137f}, s1[3] = {-0.061f, -0.003f, -0.165f};
    std::memcpy(g_grip_sawed[0].p, s0, sizeof(s0));
    std::memcpy(g_grip_sawed[1].p, s1, sizeof(s1));
    std::memcpy(g_grip_sawed[0].O, O0, sizeof(O0));
    std::memcpy(g_grip_sawed[1].O, O1, sizeof(O1));
    g_grip_sawed[0].valid = g_grip_sawed[1].valid = true;
}
int g_grip_pose = 0;
GripRel g_grip_cur;          // the one used: toward the game's pose's grip, a quarter of the way a frame (no jump
uint64_t g_grip_cur_frame = 0;  // when the game changes its pose, e.g. into its aim)
float g_snap_miss = -1.0f;  // the drawn front wrist from the snap target (m), the last snapped draw
std::atomic<uint64_t> g_snaps{0}, g_grip_learns{0};
std::vector<PropBone> g_prop_bones;
uint64_t g_body_frame = 0;
// "skel carry trace" (tests/sim/r32_carry_move.py; the review: the placements alone missed what the draw adds): the long
// gun as last drawn in an eye's pass, the frame's first such draw (1 the gun in hand, 2 the carried gun's put-away, 3 the
// carry), its root's axes as columns and its origin (world), with the frame's front target (g_frame's: what the drawn
// hands were made for) and John's front wrist as drawn (r228: past the arm's reach the drawn hand, which the gun follows
// once its T is made, is not on its target: the wrist's own frame, and its miss). Under g_draw_mutex; only while a
// trace runs
struct CarryDrawn {
    bool ok = false;
    int src = 0;
    uint64_t frame = 0;
    float R[9] = {}, o[3] = {}, t[3] = {}, r[9] = {};
    bool w_ok = false;
    float w_t[3] = {}, w_r[9] = {}, w_miss = 0.0f;  // the drawn wrist (world; its turn, columns), from its own target (m)
    // the arm's solve then (r231: the wrist 1.6 cm short of a still target with the stretch on): the shoulder (world), its
    // distance to the wrist's target, the arm's length (l1 + l2), the stretch taken, the reach used (the clamp's d)
    float w_s[3] = {}, w_dl = 0.0f, w_len = 0.0f, w_ks = 1.0f, w_d = 0.0f;
};
CarryDrawn g_carry_drawn;
CarryDrawn g_carry_wrist[2];  // each hand's drawn wrist, the frame's first player set's (transform_set: w_* only)
std::atomic<bool> g_carry_trace_on{false};  // (carry_trace_tick's, once a frame: no clock read in the draws)
// (2026-11-01, GOAL-offhand-changes C37b; round 66 check 12, the user: on the gun carried by its foregrip every ring keeps its
// place to the others while the gun moves away from them) the carried gun as drawn (src 3: its root's axes as columns, its
// origin; world), each frame's first eye draw, for carry_frame: the rings on the model, not on where the mod placed it (the
// drawn hand, which the gun follows, misses its target past the arm's reach: r228). Under g_cmodel_mx
struct CarryModel {
    float R[9] = {}, o[3] = {};
    double ms = 0.0;  // (the review: a time, not g_body_frame across threads)
    bool ok = false;
};
CarryModel g_cmodel;
std::mutex g_cmodel_mx;
std::atomic<uint64_t> g_cmodel_uses{0};
void carry_drawn_note(int src, const float* m, const float* rec, const float* po, const float* rows) {
    if (src == 3 && (t_draw_pass & 7) != 0) {
        static uint64_t pub_frame = ~0ull;
        if (pub_frame != g_body_frame) {
            pub_frame = g_body_frame;
            DrawnGun d;
            drawn_gun_stage(d, 0, m, rec, po, rows);
            bool fin = true;
            for (float x : d.R[0]) fin = fin && std::isfinite(x);
            for (float x : d.o[0]) fin = fin && std::isfinite(x);
            std::lock_guard lock(g_cmodel_mx);
            if (fin) {
                std::memcpy(g_cmodel.R, d.R[0], sizeof(g_cmodel.R));
                std::memcpy(g_cmodel.o, d.o[0], sizeof(g_cmodel.o));
                g_cmodel.ms = log::now_ms();  // (once a body frame)
            }
            g_cmodel.ok = fin;
        }
    }
    if (!g_carry_trace_on.load(std::memory_order_relaxed) || (t_draw_pass & 7) == 0) return;  // (not the shadow pre-pass)
    if (g_carry_drawn.ok && g_carry_drawn.frame == g_body_frame) return;
    const int fj = 1 - g_frame.gun_j;
    if (fj < 0 || fj > 1 || !g_frame.ik[fj]) return;
    const CarryDrawn& w = g_carry_wrist[fj];
    if (!w.w_ok || w.frame != g_body_frame) return;  // (a draw before this frame's player set: the frame's next draw, with it)
    DrawnGun d;
    drawn_gun_stage(d, 0, m, rec, po, rows);
    g_carry_drawn.src = src;
    g_carry_drawn.frame = g_body_frame;
    std::memcpy(g_carry_drawn.R, d.R[0], sizeof(g_carry_drawn.R));
    std::memcpy(g_carry_drawn.o, d.o[0], sizeof(g_carry_drawn.o));
    std::memcpy(g_carry_drawn.t, g_frame.ik_t[fj], sizeof(g_carry_drawn.t));
    std::memcpy(g_carry_drawn.r, g_frame.ik_r[fj], sizeof(g_carry_drawn.r));
    g_carry_drawn.w_ok = true;
    std::memcpy(g_carry_drawn.w_t, w.w_t, sizeof(w.w_t));
    std::memcpy(g_carry_drawn.w_r, w.w_r, sizeof(w.w_r));
    g_carry_drawn.w_miss = w.w_miss;
    std::memcpy(g_carry_drawn.w_s, w.w_s, sizeof(w.w_s));
    g_carry_drawn.w_dl = w.w_dl;
    g_carry_drawn.w_len = w.w_len;
    g_carry_drawn.w_ks = w.w_ks;
    g_carry_drawn.w_d = w.w_d;
    g_carry_drawn.ok = true;
}
// [Hands] GunMotionVectors (the user's log, 2026-10-12: the gun in hand lagging, in combat, never the second gun): a
// draw's sets and records carry last frame's matrices too (the previous half: the motion vectors only), and the gun in
// hand had this frame's correction put on the game's last pose there, so its motion vectors were the game's own swing
// of the gun (its aim, its recoil: a shot moves it 0.2-0.3 m in the hand) and never the hand's: DLSS and TAA pulled
// its history from the wrong place. Now the previous half is where the gun was drawn last frame (each hand and drawable:
// its drawn world matrices, the frame's first draw made with the frame's correction). Under g_draw_mutex.
struct GunPrev {
    uintptr_t key = 0;  // the drawable
    int hand = -1, count = 0, prev_count = 0;  // count: the set's matrices (0: a rigid draw's record, 16 floats)
    uint64_t frame = ~0ull, prev_frame = ~0ull;
    float cur[8 * 12] = {}, prev[8 * 12] = {};
};
GunPrev g_gun_prev[8];
std::atomic<bool> g_gun_mv{true};
struct GunMvStats {
    uint64_t fixed = 0, none = 0, cut = 0, n = 0;  // draws given last frame's drawn gun; none to give; a jump; measured
    double sum = 0.0;                              // the game's previous half off it (mm), at the root
    float max = 0.0f;
};
GunMvStats g_gun_mv_log, g_gun_mv_cmd;  // the [lag] line's (since the last line) and "skel gunmv"'s (since its reset)
// the draw's previous half (`prev`: `count` matrices of 12 floats, the world less `off_prev`; count 0: one 4x4 record,
// the world) set to last frame's drawn gun of that hand and drawable; this frame's drawn (`cur`, with `off_cur`) kept
void gun_prev_fix(int hand, uintptr_t key, const float* cur, float* prev, int count, const float* off_cur, const float* off_prev) {
    if (hand < 0 || hand > 1 || !key || count < 0 || g_corr_frame != g_body_frame) return;  // (a stale draw: as it was)
    if (count > 8) {  // (the review: a gun of more matrices than the table keeps: counted, left as the game's)
        ++g_gun_mv_log.none;
        ++g_gun_mv_cmd.none;
        return;
    }
    GunPrev* g = nullptr;
    for (GunPrev& e : g_gun_prev)
        if (e.key == key && e.hand == hand) g = &e;
    if (!g) {  // a new one: the slot seen longest ago
        g = &g_gun_prev[0];
        for (GunPrev& e : g_gun_prev)
            if (e.frame == ~0ull || (g->frame != ~0ull && e.frame < g->frame)) g = &e;
        *g = GunPrev{};
        g->key = key;
        g->hand = hand;
    }
    const int nf = count ? count * 12 : 16;
    const int t[3] = {3, 7, 11}, tr[3] = {12, 13, 14};
    if (g->frame != g_body_frame) {  // the frame's first draw of it: last frame's becomes the previous
        g->prev_frame = g->frame;
        g->prev_count = g->count;
        std::memcpy(g->prev, g->cur, sizeof(g->prev));
        g->frame = g_body_frame;
        g->count = count;
        std::memcpy(g->cur, cur, sizeof(float) * nf);
        for (int j = 0; count && j < count; ++j)
            for (int k = 0; k < 3; ++k) g->cur[j * 12 + t[k]] += off_cur[k];
    }
    GunMvStats* st[2] = {&g_gun_mv_log, &g_gun_mv_cmd};
    if (g->prev_frame == ~0ull || g->prev_frame + 1 != g_body_frame || g->prev_count != count) {
        for (GunMvStats* x : st) ++x->none;
        return;
    }
    float d2 = 0.0f;  // the root's previous place, the game's against the drawn
    for (int k = 0; k < 3; ++k) {
        const float a = count ? prev[t[k]] + off_prev[k] : prev[tr[k]], b = count ? g->prev[t[k]] : g->prev[tr[k]];
        d2 += (a - b) * (a - b);
    }
    const float mm = std::sqrt(d2) * 1000.0f;
    if (mm > 1000.0f) {  // a jump (a teleport, the gun moved to the other hand): the game's own
        for (GunMvStats* x : st) ++x->cut;
        return;
    }
    for (GunMvStats* x : st) {
        x->max = std::fmax(x->max, mm);
        x->sum += mm;
        ++x->n;
    }
    if (!g_gun_mv.load(std::memory_order_relaxed)) return;
    std::memcpy(prev, g->prev, sizeof(float) * nf);
    for (int j = 0; count && j < count; ++j)
        for (int k = 0; k < 3; ++k) prev[j * 12 + t[k]] -= off_prev[k];
    for (GunMvStats* x : st) ++x->fixed;
}

// [Hands] StaleGunFix: an early draw of a held gun (before this frame's correction): its current half put at last
// frame's drawn matrices (the cache: world, the set's offset added) moved by `side`'s wrist target from the targets the
// correction was made with to this frame's (D = R_now R_then^T, t = T_now - D T_then); its previous half at last frame's
// drawn ones as they were. `count` 0: a rigid record (16 floats: the axes in rows 0-2, the position at 12-14); else a
// skinned set's 12-float matrices (rows: the 3x3 and the position column), less off_cur/off_prev. False: left as it was.
// (2026-11-01, GOAL-offhand-fixes F5) a set's bone (3x4 rows: the 3x3, then the move column) local to its record's matrix
// rm (the game's 4x4 rows: the axes, then the place) to the world, W = R M + t (R's columns the record's axis rows), and back
// (R's inverse its transpose, as the correction takes it)
void rec_to_world(const float* rm, const float* m, float* o) {
    for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 4; ++j) o[k * 4 + j] = rm[0 * 4 + k] * m[0 * 4 + j] + rm[1 * 4 + k] * m[1 * 4 + j] + rm[2 * 4 + k] * m[2 * 4 + j];
        o[k * 4 + 3] += rm[12 + k];
    }
}
void rec_from_world(const float* rm, const float* o, float* m) {
    for (int r = 0; r < 3; ++r) {
        for (int j = 0; j < 3; ++j) m[r * 4 + j] = rm[r * 4] * o[j] + rm[r * 4 + 1] * o[4 + j] + rm[r * 4 + 2] * o[8 + j];
        m[r * 4 + 3] = rm[r * 4] * (o[3] - rm[12]) + rm[r * 4 + 1] * (o[7] - rm[13]) + rm[r * 4 + 2] * (o[11] - rm[14]);
    }
}
thread_local int t_sp_why = 0;  // (F10, a diagnostic) stale_place's last refusal: 1 off/args, 2 no IK, 3 no entry, 4 its frame, 5 its count, 6 a jump
bool stale_place(int side, int hand, uintptr_t key, float* cur, float* prev, int count, const float* off_cur, const float* off_prev) {
    t_sp_why = 1;
    if (!g_stale_fix.load(std::memory_order_relaxed) || g_corr_frame == g_body_frame || side < 0 || side > 1 || hand < 0 || hand > 1 || !key ||
        count < 0 || count > 8)
        return false;
    t_sp_why = 2;
    if (!g_corr_ik_ok[side] || !g_frame.ik[side]) return false;
    const GunPrev* e = nullptr;
    for (const GunPrev& x : g_gun_prev)
        if (x.key == key && x.hand == hand) e = &x;
    t_sp_why = !e ? 3 : (e->frame == ~0ull || e->frame + 1 != g_body_frame) ? 4 : e->count != count ? 5 : 6;
    if (!e || e->frame == ~0ull || e->frame + 1 != g_body_frame || e->count != count) return false;
    const float* Rn = g_frame.ik_r[side];
    const float* Rp = g_corr_ikr[side];
    const float* tp = g_corr_ikt[side];
    const float* tn = g_frame.ik_t[side];
    if ((tn[0] - tp[0]) * (tn[0] - tp[0]) + (tn[1] - tp[1]) * (tn[1] - tp[1]) + (tn[2] - tp[2]) * (tn[2] - tp[2]) > 0.25f) return false;  // (a jump)
    float D[9], t[3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) D[i * 3 + j] = Rn[i * 3] * Rp[j * 3] + Rn[i * 3 + 1] * Rp[j * 3 + 1] + Rn[i * 3 + 2] * Rp[j * 3 + 2];
    for (int k = 0; k < 3; ++k) t[k] = tn[k] - (D[k * 3] * tp[0] + D[k * 3 + 1] * tp[1] + D[k * 3 + 2] * tp[2]);
    if (count == 0) {
        const float* m = e->cur;
        for (int ax = 0; ax < 3; ++ax)
            for (int k = 0; k < 3; ++k) cur[ax * 4 + k] = D[k * 3] * m[ax * 4] + D[k * 3 + 1] * m[ax * 4 + 1] + D[k * 3 + 2] * m[ax * 4 + 2];
        for (int k = 0; k < 3; ++k) cur[12 + k] = D[k * 3] * m[12] + D[k * 3 + 1] * m[13] + D[k * 3 + 2] * m[14] + t[k];
        for (int ax = 0; ax < 4; ++ax) cur[ax * 4 + 3] = m[ax * 4 + 3];
        if (prev) std::memcpy(prev, m, sizeof(float) * 16);
    } else {
        for (int j = 0; j < count; ++j) {
            const float* m = e->cur + j * 12;
            float* o = cur + j * 12;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) o[r * 4 + c] = D[r * 3] * m[c] + D[r * 3 + 1] * m[4 + c] + D[r * 3 + 2] * m[8 + c];
                o[r * 4 + 3] = D[r * 3] * m[3] + D[r * 3 + 1] * m[7] + D[r * 3 + 2] * m[11] + t[r] - off_cur[r];
            }
            if (prev) {
                float* q = prev + j * 12;
                std::memcpy(q, m, sizeof(float) * 12);
                for (int r = 0; r < 3; ++r) q[r * 4 + 3] -= off_prev[r];
            }
        }
    }
    g_stale_shifted.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// [Hands] GunKick (2026-10-27, off; the user: "Look into the recoil optimumbox added"; research/run27/recoil.md):
// after optimumbox's recoil ([Hands] Recoil), 0.8.11.10. Each shot kicks the drawn gun muzzle up and back in John's
// grip: a critically damped spring (x'' = -w^2 x - 2 w x', w kKickRate) a shot strikes (v += w e: a lone shot peaks at
// exactly 1, 1/w = 40 ms after it; 0.56 at 100 ms, 0.03 at 250 ms; quick shots stack), its value times the gun's peak
// turn and slide (kick_peak, the fork's table) times [Hands] GunKick (0 - 2). Re-made here, not the fork's code, and
// unlike the fork's VISUAL ONLY: the fork multiplied the kick into the published item correction, so its aim, laser,
// reticle, held objects and the copy's placement followed the kicked barrel (a follow-up or a fanned shot left up to
// 7 degrees high, a carry's hand-over could freeze the gun tilted, TwoHandedSteer's fallback fought it a frame late:
// recoil.md 5). Here g_item_corr, g_bt, g_points' gun frame, g_place_buf (the game's object matrix, the carry's capture,
// the copy's placement) and BarrelAim never see it: only the gun's drawn matrices in hk_DrawVisEntity, after its hand's
// correction (and the steadying), before StaleGunFix and GunMotionVectors, as the fork applies it. The shots go where the
// barrel was before the kick (PerfectAccuracy, BarrelAim: the reticle and the laser on the gun as held). Two springs: 0
// the gun in hand (aim::player_shots: the launch as the game makes it, not a later clip poll), 1 the second gun or the
// copy (dual::shots_fired: its own trigger taken by the game); the carry never fires, so it never kicks. The move is
// made in the gun's own axes (x right, y up, z back: the root's drawn matrix M becomes M L, every other bone K B with
// K = M L M^-1), so every draw of the frame (the passes, the eyes) kicks alike and a set's camera-relative offset drops
// out. Two-handed (the long guns, two_blend): the turn about the grip would lift the foregrip out of John's front hand
// (4-8 cm at 0.35-0.45 m ahead, recoil.md 5), so the turn is scaled to kKickTwoTurn of itself and the slide kept: the
// gun recoils along its own barrel into the shoulder, the foregrip sliding in the front hand (moving that hand with the
// gun instead would put the kick into the arm IK's targets, whose points the holsters' foregrip hold, TwoHandedSteer
// and the carry read: the kick out of everything but the draw is the point). Under g_draw_mutex; off: the draws' one
// test of g_frame.kick_any.
std::atomic<float> g_recoil{0.0f};
constexpr float kKickRate = 25.0f;     // the spring's rate (1/s): a lone shot's peak at 40 ms
constexpr float kKickTwoTurn = 0.1f;   // two-handed: the share of the turn left (a shotgun's 10 deg: 1 deg, the foregrip ~7 mm)
constexpr float kKickE = 2.7182818f;
struct KickSpring {  // kick_frame's (before_scene, the render thread), g_kick_mutex
    float s = 0.0f, v = 0.0f;
    double step_ms = 0.0, shot_ms = -1e12, pulse_ms = 0.0;
    uint64_t seen = 0;
    bool seen_ok = false;
    float deg = 0.0f, back = 0.0f;  // the last shot's gun: its peak turn (deg) and slide back (m)
    int weapon = -1;
    uint64_t shots = 0, pulses = 0;
    float peak_s = 0.0f;  // the highest the spring reached (since "skel recoil reset")
    int key = 0;          // the gun it kicks (Frame's kick_key): another, and the spring is settled at once
    bool key_ok = false;
    uint64_t switches = 0;
    float now_turn = 0.0f, now_back = 0.0f;  // this frame's kick (deg, m), for "skel recoil"
};
KickSpring g_kick[2];
std::mutex g_kick_mutex;
std::atomic<int> g_kick_pulse_ms[2] = {};  // "skel recoil pulse <0|1> <ms>": a test impulse every ms (0 off; the session)
std::atomic<bool> g_kick_trace_on{false};  // "skel recoil trace on": each frame's drawn gun kept (the test's record)
std::atomic<uint64_t> g_kick_draws[2] = {}, g_kick_early[2] = {}, g_kick_noik[2] = {}, g_kick_bad[2] = {};
// a spring's gun drawn as a set under its record's matrix (flag 0x10000), kicked as the others (the fork left these alone;
// cycle r179: most of the copy's draws and the second gun's are these, so they never kicked): counted
std::atomic<uint64_t> g_kick_recm[2] = {};
// the kick put on each spring's gun by the frame's draws made with the frame's correction (the identity: none), this
// frame's and the one before: a draw's previous half (its motion vectors) takes last frame's kick, as it was drawn then
// (the review of 2026-10-27: this frame's on both halves gave DLSS a 7-10 degree flick with no velocity, and the copy
// the gun in hand's kick in reverse), and StaleGunFix's cache is last frame's draw, so an early draw takes this
// frame's kick in place of that one
// (the second review, 2026-10-27: each kept with the gun it was put on, Frame's kick_key: after a switch the new gun's
// first previous half took the old gun's last kick, a frame's flick in its motion vectors; another gun's is none)
struct KickApplied {
    uint64_t frame = ~0ull, prev_frame = ~0ull;
    int key = -2, prev_key = -2;
    float L[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    float prev[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};
KickApplied g_kick_applied[2];
const float kKickI[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
const float* kick_then(int r) {  // spring r's kick as drawn last frame on this gun (the identity: none, another gun, or not drawn then)
    const KickApplied& ka = g_kick_applied[r];
    const int key = g_frame.kick_key[r];
    if (ka.frame != ~0ull && ka.frame + 1 == g_body_frame) return ka.key == key ? ka.L : kKickI;
    if (ka.frame == g_body_frame && ka.prev_frame != ~0ull && ka.prev_frame + 1 == g_body_frame) return ka.prev_key == key ? ka.prev : kKickI;
    return kKickI;
}
// "skel recoil trace": each frame's drawn gun of each spring (its first draw made with the frame's correction), as the
// draw leaves this hook (after StaleGunFix and GunMotionVectors): its barrel (-z), top (+y) and root in the axes of the
// target of John's hand holding it, the aim's barrel in that target (the gun in hand), the spring and its last shot; its
// current and previous root (world) for the motion vectors' check (the previous half against last frame's drawn); the
// frame's early draw (before the frame's correction: the depth and shadow pre-pass) of that gun, as drawn; the published
// foregrip's distance ahead of the gun's root (the gun in hand, held two-handed), for the foregrip's lift
struct KickTrace {
    double ms = 0.0, shot_ms = -1e12;
    uint64_t frame = 0;
    float s = 0.0f;
    float bt[3] = {}, ut[3] = {}, pt[3] = {}, aim[3] = {};
    bool aim_ok = false, skinned = false, early_ok = false;
    float cur[12] = {}, prev[12] = {};  // world (the set's offsets added)
    float early_bt[3] = {}, early_pt[3] = {};
    float fore_d = -1.0f;
    // the kick as delivered: D = the root before kick_draw ^-1 o the root as it leaves this hook, in the gun's axes (anything
    // after the kick that undid it shows here): its turn about x (deg), its slide back (+z, m), its row 1's z and t (a
    // point d ahead, (0 0 -d), lifts by -d D[6] + D[7])
    bool d_ok = false;
    float d_turn = 0.0f, d_slide = 0.0f, d_r1z = 0.0f, d_r1t = 0.0f;
};
constexpr int kKickTraceN = 512;  // (5.7 s at 90 frames a second)
KickTrace g_kick_trace[2][kKickTraceN];
uint64_t g_kick_trace_n[2] = {}, g_kick_trace_frame[2] = {~0ull, ~0ull};
struct KickEarly {  // the frame's early draw of a spring's gun (kick_note), for its trace entry
    uint64_t frame = ~0ull;
    float bt[3] = {}, pt[3] = {};
};
KickEarly g_kick_early_s[2];
struct KickPre {  // the trace's draw of the frame before its kick (kick_draw), for the delivered kick (kick_note)
    uint64_t frame = ~0ull;
    const float* m = nullptr;
    float root[12] = {};
};
KickPre g_kick_pre[2];
// a spring's draw that makes its trace entry: the gun in hand's first made with the frame's correction (an earlier one is
// the pre-pass's, StaleGunFix's case, kept apart); the second gun's or the copy's first of the frame, early or not (they
// have no stale fix: the review's cycle r178 found the second gun's draws all before the frame's correction, so its trace
// was empty)
bool kick_entry_draw(int r, bool early) { return r == 1 || !early; }
// a gun's peak turn (deg) and slide back (m) at GunKick=1, by eWeapon (the fork's table; recoil.md 2); none for the rest
// (the bow, the thrown weapons, the Explosive Rifle, the Blunderbuss)
void kick_peak(int w, float* deg, float* back) {
    *deg = 0.0f, *back = 0.0f;
    if (w >= 0 && w <= 3) *deg = 5.0f, *back = 0.012f;                       // the pistols
    else if (w >= 4 && w <= 7) *deg = w == 7 ? 8.0f : 7.0f, *back = 0.015f;  // the revolvers (the LeMat heavier)
    else if (w >= 8 && w <= 11) *deg = 4.0f, *back = 0.012f;                 // the repeaters
    else if (w == 14) *deg = 9.0f, *back = 0.022f;                           // the Buffalo rifle
    else if (w >= 15 && w <= 18) *deg = 10.0f, *back = 0.025f;               // the shotguns
    else if (w >= 12 && w <= 20) *deg = 6.0f, *back = 0.016f;                // the other rifles
}
bool finite_n(const float* m, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}
bool is_identity34(const float* m) { return !std::memcmp(m, kKickI, sizeof(kKickI)); }
// the kick in the gun's axes: the turn about its x (right) by deg, the barrel (-z) toward its top (+y), about the root
// (the grip), then the slide back along +z (m)
void kick_local(float deg, float back, float* L) {
    const float a = deg * 0.017453293f, c = std::cos(a), s = std::sin(a);
    const float l[12] = {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, back};
    std::memcpy(L, l, sizeof(l));
}
// [Hands] GunCollision (2026-10-29, off; GOAL-weapons-handling item 4 and its amendment, research/run28/collision.md): the
// guns in John's hands, his hands and his forearms as capsules (a segment and a radius), and each drawn gun held apart at
// what it touches. The real hands cannot be stopped, so a contact is drawn only: (round 43, g_coll_handmove below) the
// hand holding a gun drawn moved with it (its wrist target), never a turn, so the gun stays in the hand and its barrel
// parallel; the aim and the shots follow the drawn gun by that slide. (Before, and the control: each spring's gun took the
// slide in its own axes on top of its kick, kick_frame and kick_draw, out of the hand, the aim keeping the gun as held.)
// Every zone keeps the controller. A gun held by a snap (the front hand on its foregrip, a part
// or the barrels in the off hand, the thumb on its hammer, the carry by the foregrip) is held still: the other gun takes
// the whole move, or, an empty hand against a gun held still, that hand's wrist target is moved (its IK only: the published
// hand stays the controller's, so every zone and grip still reads the controller). A hand loading (a round, a magazine), an
// empty hand gripping (a take, a part, the foregrip, the carried gun's take-back) or near a long gun's foregrip neither pushes
// nor is pushed by that gun: the grips and the snaps win. Each contact begun pulses both controllers (a gun on its own
// forearm: its own); two guns met at speed play the game's ARMOR_METAL_SHORT (actions.cpp, the frame end). The solve, the
// render thread once a frame (before_scene, before kick_frame): the last frame's drawn guns and arms (the published) carried
// to this frame's hand targets (rigid with them; the elbow by the wrist's move), each pair's normal kept while it stays in
// contact (a crossing pushes on the side it began from), a projected Gauss-Seidel over the pairs' separations with
// kCollSkin of gap, a pair pressed past kCollMaxDepth let go until it parts (the guns then pass through: the move eased out
// over kCollEaseMs).
std::atomic<bool> g_coll_cfg{false};
// "skel collide measure on" (the session, a test aid): the bodies taken and their distances measured with the option off
// too (nothing moved): the pass-through's control
std::atomic<bool> g_coll_measure{false};
// "skel collide nosnap on" (the session, a test aid): the grips' and the snaps' rules off (every hand collides, no gun held
// still): their control, a hold that would be pushed off without them
std::atomic<bool> g_coll_nosnap{false};
// (2026-10-29, round 43: "guns get pushed out of hand") a gun's move at a contact taken by the hand holding it: that hand's
// wrist target moved (its IK, as an empty hand's), so the gun stays in the drawn hand and the hand goes with it. The aim and
// the shots follow the drawn gun (its hand's correction), a slide only: the barrel parallel, a few centimetres at most.
// The bodies as held take the hand's last move out (as the arms do), so the move is not counted twice.
// "skel collide handmove off" (the session, a test aid): the old slide of the drawn gun off the hand, its control
std::atomic<bool> g_coll_handmove{true};
constexpr float kCollSkin = 0.003f;       // the gap kept at a contact (m)
constexpr float kCollMaxDepth = 0.15f;    // a pair pressed deeper is let go until it parts (m)
constexpr double kCollEaseMs = 40.0;      // a move let go eased out over this
constexpr float kCollHandR = 0.035f, kCollArmR = 0.035f;  // the hand's and the forearm's radius (m: the glove, the sleeve)
constexpr float kCollForeFree = 0.15f;    // a free hand this near a long gun's foregrip neither pushes nor is pushed by it (m)
constexpr float kCollGunNear = 0.02f;    // two guns this near (as held): their pair takes them, not the other's holding hand (m)
constexpr float kCollGunFar = 0.03f;     // ...until they are this far apart again (the hysteresis)
constexpr float kCollOwnCut = 0.07f;      // a gun against its own forearm: the forearm from the elbow to this short of the wrist
constexpr int kCollN = 5;                 // the bodies: 0 the gun in hand, 1 the second gun or the copy, 2 the carried gun, 3 4 John's arms
struct CollCap {
    float a[3] = {}, b[3] = {};
    float r = 0.0f;
};
// a gun's capsules in its own axes (x right, y up, z back; m; [2]: against its own forearm only, its barrel past the grip,
// from the root to the muzzle: the review, the Semi-Automatic's toggle housing 0.14 m back and the Mauser's 0.09 lie over the
// wrist in every hold): [0] its body along the barrel (the frame or the receiver to
// the muzzle), [1] its grip (the sidearms) or its stock (the long guns), for every gun reload::is_gun takes (0 - 20, the
// Explosive Rifle 31 and the Blunderbuss 34, as long guns); none for the rest (the lasso, the knives, the thrown weapons, the bow,
// the mounted guns). The muzzle: mo (the tune's MuzzleOffset, the held sample's) when sane, else a sidearm's locator
// (dual::sidearm_muzzle, the run 5 models), else 0.7 m ahead (the Sawed-off's 0.4, the Blunderbuss's 0.55 [I]). The rest per class: the pistols' from their models' bounds (run 28
// magazines.md 1: the High Power y -0.049..0.084, z -0.154..0.078), the revolvers' about their muzzles and hammers (the
// cylinder 4 cm across), the long guns' receiver and stock [I: not measured]
int coll_shape(int w, const float* mo, CollCap* c) {
    if (!reload::is_gun(w)) return 0;
    float m[3] = {0.0f, 0.05f, w == 15 ? -0.40f : w == 34 ? -0.55f : -0.70f};
    const bool side = w <= 7;
    const bool mo_ok = mo && std::isfinite(mo[0] + mo[1] + mo[2]) && std::fabs(mo[0]) < 0.05f;
    if (side) {
        if (mo_ok && mo[2] < -0.08f && mo[2] > -0.40f && std::fabs(mo[1]) < 0.15f) std::memcpy(m, mo, sizeof(m));
        else if (!dual::sidearm_muzzle(w, m)) return 0;
    } else if (mo_ok && mo[2] < -0.25f && mo[2] > -1.4f && std::fabs(mo[1]) < 0.2f) {
        std::memcpy(m, mo, sizeof(m));
    }
    if (side) {
        // the frame from behind the hammer or the slide (the Semi-Automatic's toggle housing 0.16 m back)
        const float rear = w == 1 ? 0.14f : w == 3 ? 0.09f : w == 2 ? 0.06f : w == 0 ? 0.05f : 0.035f;
        const float y = m[1] - 0.006f;
        c[0] = CollCap{{m[0], y, rear}, {m[0], y, m[2] + 0.008f}, w >= 4 ? 0.020f : 0.016f};
        c[1] = CollCap{{0.0f, 0.02f, 0.03f}, {0.0f, w >= 4 ? -0.05f : -0.035f, 0.055f}, 0.016f};
    } else {
        c[0] = CollCap{{0.0f, 0.045f, 0.03f}, {m[0], m[1] - 0.006f, m[2] + 0.01f}, 0.024f};
        c[1] = CollCap{{0.0f, 0.02f, 0.07f}, {0.0f, -0.035f, w == 15 ? 0.17f : 0.33f}, 0.028f};
    }
    c[2] = CollCap{{c[0].b[0], c[0].b[1], 0.0f}, {c[0].b[0], c[0].b[1], c[0].b[2]}, c[0].r};
    return 2;  // (the pairs take [0] and [1]; [2] only its own forearm's)
}
float cdot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
// the closest points c1 on p1 q1 and c2 on p2 q2 (Ericson, Real-Time Collision Detection 5.1.9); their squared distance
float seg_seg(const float* p1, const float* q1, const float* p2, const float* q2, float* c1, float* c2) {
    float d1[3], d2[3], r[3];
    for (int k = 0; k < 3; ++k) {
        d1[k] = q1[k] - p1[k];
        d2[k] = q2[k] - p2[k];
        r[k] = p1[k] - p2[k];
    }
    const float a = cdot(d1, d1), e = cdot(d2, d2), f = cdot(d2, r);
    float s = 0.0f, t = 0.0f;
    if (a > 1e-10f || e > 1e-10f) {
        if (a <= 1e-10f) {
            t = clamp01(f / e);
        } else {
            const float c = cdot(d1, r);
            if (e <= 1e-10f) {
                s = clamp01(-c / a);
            } else {
                const float b = cdot(d1, d2), den = a * e - b * b;
                s = den > 1e-12f ? clamp01((b * f - c * e) / den) : 0.0f;
                t = (b * s + f) / e;
                if (t < 0.0f) {
                    t = 0.0f;
                    s = clamp01(-c / a);
                } else if (t > 1.0f) {
                    t = 1.0f;
                    s = clamp01((b - c) / a);
                }
            }
        }
    }
    float d2s = 0.0f;
    for (int k = 0; k < 3; ++k) {
        c1[k] = p1[k] + d1[k] * s;
        c2[k] = p2[k] + d2[k] * t;
        d2s += (c2[k] - c1[k]) * (c2[k] - c1[k]);
    }
    return d2s;
}
struct CollBody {
    bool ok = false, gun = false;
    int john = -1, weapon = -1;
    float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, o[3] = {};  // a gun's axes (columns) and origin as held (the world less the base)
    int n = 0;
    CollCap cap[3];  // a gun's in its own axes ([2] its barrel past the grip); an arm's in the world less the base: [0] the hand, [1] the forearm, [2] the
                     // forearm short of the wrist (against its own gun)
    float w = 0.0f;  // its share of a move: 1 it moves, 0 held still
    float x[3] = {};  // the move found this frame (world)
};
// body b's capsule i (the world less the base), moved by mv (nullptr: as held)
void coll_cap(const CollBody& b, int i, const float* mv, float* p, float* q) {
    const CollCap& c = b.cap[i];
    for (int k = 0; k < 3; ++k) {
        if (b.gun) {
            p[k] = b.o[k] + b.R[k * 3] * c.a[0] + b.R[k * 3 + 1] * c.a[1] + b.R[k * 3 + 2] * c.a[2];
            q[k] = b.o[k] + b.R[k * 3] * c.b[0] + b.R[k * 3 + 1] * c.b[1] + b.R[k * 3 + 2] * c.b[2];
        } else {
            p[k] = c.a[k];
            q[k] = c.b[k];
        }
        if (mv) p[k] += mv[k], q[k] += mv[k];
    }
}
struct CollPair {  // a pair's state across frames (g_coll_pair: a < b; the render thread)
    bool active = false, latched = false, seen = false;
    float n[3] = {0.0f, 1.0f, 0.0f};  // the normal it pushes along (a to b)
    float last_s = 0.0f;
    double last_ms = 0.0, end_ms = -1e12;
    uint64_t stamp = 0;  // the frame (g_coll_stamp) it was last taken in: a pair left out a frame is no longer in contact
    // a's share of the pair's move as applied, eased toward wa / (wa + wb) over kCollEaseMs while the contact lasts (the
    // review: a snap begun mid-contact moves the other body's share in as the snapped one's eases out, never in a frame)
    float share = 0.5f;
};
CollPair g_coll_pair[kCollN][3][kCollN][3];
float g_coll_app[kCollN][3] = {};  // each body's move as applied (eased out after a let-go)
// (the review) a body whose contact begins while its move is still easing out, or that stays in contact but loses a pair to
// a let-go: the gap from its move as applied to the new one taken once (g_coll_off, world), then decayed by the ease each
// frame on top of the new move (applied = move + gap), whatever the move does meanwhile, until the gap is under 1 mm (about
// 5 kCollEaseMs). (The review of 31ec62d: a low-pass toward each frame's move lagged a pushing hand by v x 40 ms all through)
bool g_coll_easing[kCollN] = {};
float g_coll_off[kCollN][3] = {};
uint64_t g_coll_stamp = 0;
bool g_coll_gun_near[3][3] = {};  // each gun pair's rule state (the hysteresis; g < g2)
double g_coll_last_ms = 0.0;
bool g_coll_was = false;
// "skel collide" (g_coll_mutex: written by the render thread once a frame)
struct CollStats {
    bool on = false, measure = false;
    uint64_t frames = 0, moved = 0, begun = 0, pulses[2] = {}, sounds = 0, releases = 0, unresolved = 0, hand_frames = 0, drawn_frames = 0;
    float max_push[kCollN] = {};
    // the closest each kind of pair came since "skel collide reset" (m between the surfaces: under 0 they overlap): 0 a gun and
    // the other gun, 1 a gun and the other hand, 2 a gun and the other forearm, 3 a gun and its own forearm; as held (the
    // hands'), as the moves put them (the solve's), and as drawn (the last frame's drawn guns from "skel recoil trace", the
    // published drawn arms)
    float held_min[4] = {1e9f, 1e9f, 1e9f, 1e9f}, moved_min[4] = {1e9f, 1e9f, 1e9f, 1e9f}, drawn_min[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    uint64_t held_n[4] = {}, drawn_n[4] = {};
    int contacts = 0, pairs = 0;
    bool holding[2] = {}, loading[2] = {};
    float grip[2] = {};
    float hand_in[2][2] = {};  // the most the other hand was in each gun's body and grip since the reset, as held (m; r49's AH20)
    bool barrel_ok[2] = {};
    float barrel[2][3] = {};  // each spring's drawn barrel in the last frame measured (the trace's)
    double us_sum = 0.0;  // coll_frame's own time (the option on or measuring), since the reset
    float us_max = 0.0f;
    uint64_t us_n = 0;
    bool body_ok[kCollN] = {};
    int weapon[kCollN] = {-1, -1, -1, -1, -1};
    float push[kCollN][3] = {};
    float held_now[4] = {1e9f, 1e9f, 1e9f, 1e9f}, drawn_now[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    char list[480] = {};
};
CollStats g_coll_st;
std::mutex g_coll_mutex;
body::CollGuns g_coll_pub;  // collision_guns' (g_coll_mutex)
bool g_coll_sound_ok = false;  // collision_sound's (g_coll_mutex)
float g_coll_sound_at[3] = {};
double g_coll_sound_ms = -1e12;  // the render thread's
const char* const kCollBodyName[kCollN] = {"the gun in hand", "the second gun", "the carried gun", "the left arm", "the right arm"};
// This frame's collisions (before_scene, once a frame, the render thread; before kick_frame, which puts the guns' moves on
// their kicks): f's coll_* set. With the option off (and no "skel collide measure"), nothing read or written but the clear
void coll_frame(Frame& f) {
    f.coll_on[0] = f.coll_on[1] = f.coll_hand_on[0] = f.coll_hand_on[1] = false;
    std::memset(f.coll_l, 0, sizeof(f.coll_l));
    std::memset(f.coll_hand, 0, sizeof(f.coll_hand));
    const bool on = g_coll_cfg.load(std::memory_order_relaxed), meas = g_coll_measure.load(std::memory_order_relaxed);
    const bool nosnap = g_coll_nosnap.load(std::memory_order_relaxed);
    const double now = log::now_ms();
    // (the review of 8ea2be1) turned off with "skel collide measure" left on: the moves and the pairs' state (the let-go
    // latches) cleared as a full off clears them, so the measuring that follows starts clean, never stuck latched
    static bool s_on_was = false;
    const bool turned_off = s_on_was && !on;
    s_on_was = on;
    if ((!on && !meas) || turned_off) {
        if (g_coll_was || turned_off) {
            g_coll_was = false;
            std::memset(g_coll_app, 0, sizeof(g_coll_app));
            std::memset(g_coll_easing, 0, sizeof(g_coll_easing));
            std::memset(g_coll_off, 0, sizeof(g_coll_off));
            std::memset(g_coll_gun_near, 0, sizeof(g_coll_gun_near));
            for (auto& a : g_coll_pair)
                for (auto& b : a)
                    for (auto& c : b)
                        for (CollPair& p : c) p = CollPair{};
            std::lock_guard lock(g_coll_mutex);
            g_coll_st.on = g_coll_st.measure = false;
            g_coll_pub.ms = 0.0;
            g_coll_sound_ok = false;  // (the review: no stale knock at an old place when it is turned on again)
        }
        if (!meas) return;
    }
    g_coll_was = true;
    ++g_coll_stamp;
    LARGE_INTEGER q0, qf;
    QueryPerformanceCounter(&q0);
    const double dt_ms = g_coll_last_ms > 0.0 ? now - g_coll_last_ms : 0.0;
    const float ease = dt_ms > 0.0 ? static_cast<float>(std::exp(-dt_ms / kCollEaseMs)) : 0.0f;  // (a let-go's, a share's)
    g_coll_last_ms = now;
    BodyPoints bp;
    const bool bp_ok = body_points(&bp);
    CollBody B[kCollN];
    const int gj = f.gun_j >= 0 && f.gun_j <= 1 ? f.gun_j : 1;
    float base[3];
    std::memcpy(base, f.ik_t[gj], sizeof(base));
    // a point (or a direction) rigid with John's hand h's target, from that target as it was (Rl columns, tl; world) to
    // this frame's (f.ik_r, f.ik_t), less the base
    auto carry_pt = [&](int h, const float* Rl, const float* tl, const float* p, float* out) {
        float d[3] = {p[0] - tl[0], p[1] - tl[1], p[2] - tl[2]}, q[3];
        for (int i = 0; i < 3; ++i) q[i] = Rl[i] * d[0] + Rl[3 + i] * d[1] + Rl[6 + i] * d[2];
        for (int k = 0; k < 3; ++k) out[k] = f.ik_t[h][k] - base[k] + f.ik_r[h][k * 3] * q[0] + f.ik_r[h][k * 3 + 1] * q[1] + f.ik_r[h][k * 3 + 2] * q[2];
    };
    auto carry_dir = [&](int h, const float* Rl, const float* v, float* out) {
        float q[3];
        for (int i = 0; i < 3; ++i) q[i] = Rl[i] * v[0] + Rl[3 + i] * v[1] + Rl[6 + i] * v[2];
        for (int k = 0; k < 3; ++k) out[k] = f.ik_r[h][k * 3] * q[0] + f.ik_r[h][k * 3 + 1] * q[1] + f.ik_r[h][k * 3 + 2] * q[2];
    };
    // a gun as held: its axes R (columns) and origin o (world) in John's hand h's target (Rl, tl) of that frame, carried to
    // this frame's (carry false: as it was), into body b
    auto set_gun = [&](CollBody& b, int h, const float* R, const float* o, const float* Rl, const float* tl, bool carry, int w, const float* mo) {
        b.n = coll_shape(w, mo, b.cap);
        if (!b.n) return;
        float Rn[9];
        for (int j = 0; j < 3; ++j) {
            const float col[3] = {R[j], R[3 + j], R[6 + j]};
            float c2[3];
            if (carry) carry_dir(h, Rl, col, c2);
            else std::memcpy(c2, col, sizeof(c2));
            for (int k = 0; k < 3; ++k) Rn[k * 3 + j] = c2[k];
        }
        {  // (unit axes, square: the game's matrix may carry a little scale; ortho_cols takes a set matrix's 3x4 rows)
            const float s[12] = {Rn[0], Rn[1], Rn[2], 0, Rn[3], Rn[4], Rn[5], 0, Rn[6], Rn[7], Rn[8], 0};
            ortho_cols(s, b.R);
        }
        if (carry) {
            carry_pt(h, Rl, tl, o, b.o);
        } else {
            for (int k = 0; k < 3; ++k) b.o[k] = o[k] - base[k];
        }
        bool fin = true;
        for (int i = 0; i < 9; ++i) fin = fin && std::isfinite(b.R[i]);
        for (int k = 0; k < 3; ++k) fin = fin && std::isfinite(b.o[k]) && std::fabs(b.o[k]) < 5.0f;
        b.ok = fin;
        b.gun = true;
        b.john = h;
        b.weapon = w;
    };
    // 0: the gun in hand, as last drawn (item_gun: its build's game matrix through its hand's correction), on John's gun hand
    const HeldSample* hs = newest_held(f.held);
    if (hs && hs->valid && !hs->sec && hs->follow_j < 0 && !f.carry_put && f.ik[gj] && bp_ok && bp.hand_ok[gj] && reload::is_gun(f.weapon)) {
        float A[9], a[3], gm[16];
        bool ik = false;
        uintptr_t W = 0;
        uint64_t fr = 0;
        if (item_gun(hs->left ? 0 : 1, A, a, &ik, gm, &W, &fr) && W == hs->W) {
            float R[9], o[3];
            for (int j = 0; j < 3; ++j) {
                float l = std::sqrt(gm[j * 4] * gm[j * 4] + gm[j * 4 + 1] * gm[j * 4 + 1] + gm[j * 4 + 2] * gm[j * 4 + 2]);
                l = l > 1e-6f ? l : 1.0f;
                for (int k = 0; k < 3; ++k) R[k * 3 + j] = (A[k * 3] * gm[j * 4] + A[k * 3 + 1] * gm[j * 4 + 1] + A[k * 3 + 2] * gm[j * 4 + 2]) / l;
            }
            for (int k = 0; k < 3; ++k) o[k] = A[k * 3] * gm[12] + A[k * 3 + 1] * gm[13] + A[k * 3 + 2] * gm[14] + a[k] - bp.hand_push[gj][k];
            set_gun(B[0], gj, R, o, bp.target_rot[gj], bp.hand[gj], fr == bp.frame, f.weapon, hs->mo_ok ? hs->mo : nullptr);
            // held still by a snap: the front hand on its foregrip, a part or the barrels in the off hand, the thumb on its hammer
            B[0].w = !nosnap && (f.two_blend > 0.0f || f.part_blend > 0.0f || f.barrel_blend > 0.0f || (f.thumb_w >= 0 && f.thumb_wt > 0.0f)) ? 0.0f : 1.0f;
        }
    }
    // 1, 2: the second gun or the copy, or the carried gun (held still: its foregrip in the carrying hand), as last drawn
    const dual::State ds = dual::state();
    {
        SecondDrawn sd;
        double age = 0.0;
        if (ds.on && second_drawn(&sd, &age) && sd.john >= 0 && sd.john <= 1 && f.ik[sd.john]) {
            const int bi = ds.carry ? 2 : 1;
            const int w = ds.carry ? ds.carry_weapon : ds.copy ? (dual::own_model() && dual::copy_model() >= 0 ? dual::copy_model() : f.weapon) : ds.weapon;
            float R[9], o[3];
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) R[k * 3 + j] = sd.m[j * 4 + k];
            for (int k = 0; k < 3; ++k) o[k] = sd.m[12 + k] - (bp_ok ? bp.hand_push[sd.john][k] : 0.0f);  // (round 43: its hand's last move out)
            set_gun(B[bi], sd.john, R, o, sd.R, sd.t, bp_ok && sd.frame == bp.frame, w, nullptr);
            B[bi].w = ds.carry ? 0.0f : 1.0f;
        }
    }
    // 3, 4: John's arms as last drawn, less their hand's move then, carried to this frame's targets (the hand rigid with its
    // target; the elbow by the wrist's move)
    float wrist_last[2][3] = {};
    for (int h = 0; h < 2; ++h) {
        CollBody& b = B[3 + h];
        if (!bp_ok || !bp.limb_ok[h] || !bp.hand_ok[h] || !f.ik[h]) continue;
        float W0[3], K0[3], E0[3], Wn[3], Kn[3], En[3];
        for (int k = 0; k < 3; ++k) {
            W0[k] = bp.wrist_d[h][k] - bp.hand_push[h][k];
            K0[k] = bp.knuckle_d[h][k] - bp.hand_push[h][k];
            E0[k] = bp.elbow_d[h][k];
        }
        std::memcpy(wrist_last[h], W0, sizeof(W0));
        carry_pt(h, bp.target_rot[h], bp.hand[h], W0, Wn);
        carry_pt(h, bp.target_rot[h], bp.hand[h], K0, Kn);
        for (int k = 0; k < 3; ++k) En[k] = E0[k] - base[k] + (Wn[k] + base[k] - W0[k]);
        float fa[3] = {Wn[0] - En[0], Wn[1] - En[1], Wn[2] - En[2]};
        const float fl = std::sqrt(cdot(fa, fa));
        if (!(fl > 0.05f && fl < 1.0f)) continue;
        for (int k = 0; k < 3; ++k) {
            b.cap[0].a[k] = Wn[k];
            b.cap[0].b[k] = Kn[k] + 0.35f * (Kn[k] - Wn[k]);
            b.cap[1].a[k] = En[k];
            b.cap[1].b[k] = Wn[k];
            b.cap[2].a[k] = En[k];
            b.cap[2].b[k] = Wn[k] - fa[k] / fl * kCollOwnCut;
        }
        b.cap[0].r = kCollHandR;
        b.cap[1].r = b.cap[2].r = kCollArmR;
        b.n = 3;
        b.ok = true;
        b.john = h;
        bool fin = true;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) fin = fin && std::isfinite(b.cap[i].a[k]) && std::isfinite(b.cap[i].b[k]) && std::fabs(b.cap[i].a[k]) < 5.0f;
        b.ok = fin;
    }
    // what each of John's hands holds, loads and grips
    const int mag_h = round_draw::held_mag_hand();
    bool holding[2] = {false, false}, loading[2] = {false, false};
    float grip[2] = {0.0f, 0.0f};
    for (int i = 0; i < 3; ++i)
        if (B[i].ok && B[i].john >= 0) holding[B[i].john] = true;
    for (int h = 0; h < 2; ++h) {
        loading[h] = f.round_hand == h || mag_h == h;
        grip[h] = hands::get(f.ctrl[h]).grip;
    }
    // a hand that neither pushes nor is pushed by gun g: loading, an empty hand gripping, on a snap of g, near a long gun's foregrip
    auto exempt = [&](int h, int g) {
        if (nosnap) return false;
        if (loading[h] || (!holding[h] && grip[h] >= 0.5f)) return true;
        if (g == 0) {
            if (h != gj && (f.two_blend > 0.0f || f.part_blend > 0.0f || f.barrel_blend > 0.0f)) return true;
            if (B[0].weapon >= 8 && bp_ok && bp.fore_ok) {
                float d[3] = {wrist_last[h][0] - bp.fore[0], wrist_last[h][1] - bp.fore[1], wrist_last[h][2] - bp.fore[2]};
                if (cdot(d, d) < kCollForeFree * kCollForeFree) return true;
            }
        }
        if (g == 2 && f.cpart_blend > 0.0f && h != B[2].john) return true;
        return false;
    };
    // the pairs: each gun with each later gun (not while either's hand loads: the cross reload); each gun with the other
    // arm's hand and forearm, and its body with its own forearm short of the wrist
    struct P {
        int a, ia, b, ib, cat;
        float wa, wb;
        float ka;  // a's share of the move (CollPair::share), b's 1 - ka
        float n[3], depth;
        bool act;
        CollPair* ps;  // its state across frames
        bool begin;     // a contact begun this frame (its pulse, its log line and its knock after the let-go's cut)
        float closing, in_mm, mid[3];
    };
    P pr[64];
    int np = 0;
    bool lost[kCollN] = {};  // a body that lost a pair in contact this frame (a let-go; the guns' rule below)
    auto add = [&](int a, int ia, int b, int ib, int cat, float wa, float wb) {
        if (np < 64) pr[np++] = P{a, ia, b, ib, cat, wa, wb, 0.5f, {0.0f, 1.0f, 0.0f}, 0.0f, false, &g_coll_pair[a][ia][b][ib], false, 0.0f, 0.0f, {}};
    };
    // (r243) two guns touching, or within kCollGunNear as held, or in contact last frame: the gun pair takes them, and neither
    // gun collides with the hand (or forearm) holding the other. Driven together, the copy in the left hand met the gun in
    // hand, that gun met the left hand and the copy the right one along other normals: together over the let-go's 15 cm, so
    // the sum cut let every pair go and the guns were left 19 mm inside each other (G; r240's clamp had held them apart)
    // (the review of 08cc317) only for a gun pair that is taken this frame (not while either hand loads, the cross reload: the
    // pair is not added then, and the guns must keep their hands' pairs), and not while one of its pairs is let go; with a
    // hysteresis per gun pair: on under kCollGunNear, off over kCollGunFar (or in contact the frame before)
    bool gun_near[3][3] = {};
    for (int g = 0; g < 3; ++g)
        for (int g2 = g + 1; g2 < 3; ++g2) {
            bool& st_near = g_coll_gun_near[g][g2];
            if (!B[g].ok || !B[g2].ok || (!nosnap && (loading[B[g].john] || loading[B[g2].john]))) {
                st_near = false;
                continue;
            }
            bool contact = false, latched = false;
            float dmin = 1e9f;
            for (int i = 0; i < B[g].n; ++i)
                for (int j = 0; j < B[g2].n; ++j) {
                    const CollPair& q = g_coll_pair[g][i][g2][j];
                    if (q.active && q.stamp + 1 == g_coll_stamp) contact = true;
                    if (q.latched) latched = true;
                    float a0[3], a1[3], b0[3], b1[3], ca[3], cb[3];
                    coll_cap(B[g], i, nullptr, a0, a1);
                    coll_cap(B[g2], j, nullptr, b0, b1);
                    dmin = std::fmin(dmin, std::sqrt(seg_seg(a0, a1, b0, b1, ca, cb)) - B[g].cap[i].r - B[g2].cap[j].r);
                }
            if (latched) st_near = false;
            else if (contact || dmin < kCollGunNear) st_near = true;
            else if (dmin > kCollGunFar) st_near = false;
            gun_near[g][g2] = gun_near[g2][g] = st_near;
        }
    auto holds_near = [&](int h, int g) {  // John's hand h holds a gun near gun g
        for (int g2 = 0; g2 < 3; ++g2)
            if (g2 != g && B[g2].ok && B[g2].john == h && gun_near[g][g2]) return true;
        return false;
    };
    for (int g = 0; g < 3; ++g) {
        if (!B[g].ok) continue;
        for (int g2 = g + 1; g2 < 3; ++g2) {
            if (!B[g2].ok || (!nosnap && (loading[B[g].john] || loading[B[g2].john]))) continue;
            for (int i = 0; i < B[g].n; ++i)
                for (int j = 0; j < B[g2].n; ++j) add(g, i, g2, j, 0, B[g].w, B[g2].w);
        }
        for (int h = 0; h < 2; ++h) {
            const CollBody& arm = B[3 + h];
            if (!arm.ok) continue;
            if (h == B[g].john) {
                add(g, 2, 3 + h, 2, 3, B[g].w, 0.0f);  // (its barrel past the grip against its forearm short of the wrist)
            } else if (!exempt(h, g) && holds_near(h, g)) {
                // (the rule's: a hand pair in contact last frame dropped by it is a pair lost, the onset gap covers it)
                for (int i = 0; i < B[g].n; ++i)
                    for (int c = 0; c < 2; ++c) {
                        const CollPair& q = g_coll_pair[g][i][3 + h][c];
                        if (q.active && q.stamp + 1 == g_coll_stamp) lost[g] = lost[3 + h] = true;
                    }
            } else if (!exempt(h, g)) {
                const float wa = B[g].w > 0.0f ? 0.0f : holding[h] ? 0.0f : 1.0f;  // (an empty hand moves off a gun held still)
                for (int i = 0; i < B[g].n; ++i) {
                    add(g, i, 3 + h, 0, 1, B[g].w, wa);
                    add(g, i, 3 + h, 1, 2, B[g].w, wa);
                }
            }
        }
    }
    // each pair's separation as held, its normal kept while in contact
    float held_now[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    int unresolved = 0, releases = 0, begun_now = 0;
    float hand_in[2][2] = {};  // how far the other hand is in the gun in hand's and the second gun's body [0] and grip [1], as held (m)
    bool pulse_c[2] = {false, false};
    float pulse_a[2] = {0.0f, 0.0f};
    bool sound = false;
    float sound_at[3] = {};
    for (int i = 0; i < np; ++i) {
        P& p = pr[i];
        CollPair& ps = g_coll_pair[p.a][p.ia][p.b][p.ib];
        float a0[3], a1[3], b0[3], b1[3], ca[3], cb[3];
        coll_cap(B[p.a], p.ia, nullptr, a0, a1);
        coll_cap(B[p.b], p.ib, nullptr, b0, b1);
        const float d = std::sqrt(seg_seg(a0, a1, b0, b1, ca, cb));
        const float rr = B[p.a].cap[p.ia].r + B[p.b].cap[p.ib].r;
        float v[3] = {cb[0] - ca[0], cb[1] - ca[1], cb[2] - ca[2]}, n[3], s;
        if (ps.active && ps.stamp + 1 == g_coll_stamp && (d < 0.004f || cdot(v, ps.n) < 0.0f)) {  // (crossing: the side it began from)
            std::memcpy(n, ps.n, sizeof(n));
            s = cdot(v, n);
        } else if (d > 1e-6f) {
            for (int k = 0; k < 3; ++k) n[k] = v[k] / d;
            s = d;
        } else {
            for (int k = 0; k < 3; ++k) n[k] = B[p.a].gun ? B[p.a].R[k * 3 + 1] : (k == 1 ? 1.0f : 0.0f);
            s = 0.0f;
        }
        held_now[p.cat] = std::fmin(held_now[p.cat], s - rr);
        if (p.cat == 1 && p.a < 2 && p.ia < 2) hand_in[p.a][p.ia] = std::fmax(hand_in[p.a][p.ia], rr - s);
        const float closing = ps.seen && now - ps.last_ms > 1.0 && now - ps.last_ms < 100.0 ? (ps.last_s - s) / static_cast<float>((now - ps.last_ms) * 0.001) : 0.0f;
        ps.seen = true;
        ps.last_s = s;
        ps.last_ms = now;
        const bool was = ps.active && ps.stamp + 1 == g_coll_stamp;
        if (ps.active && !was) ps.end_ms = now;  // (left out since: ended then)
        ps.stamp = g_coll_stamp;
        p.depth = rr + kCollSkin - s;
        ps.active = false;
        if (!on) continue;  // (measured only)
        if (ps.latched) {  // let go: until it parts
            if (d - rr > 0.01f) ps.latched = false;
            continue;
        }
        if (p.depth <= 0.0f) {
            if (was) ps.end_ms = now;
            continue;
        }
        if (p.wa + p.wb <= 0.0f) {
            ++unresolved;
            continue;
        }
        if (p.depth > kCollMaxDepth) {
            if (was) lost[p.a] = lost[p.b] = true;
            ps.latched = true;
            ps.end_ms = now;
            ++releases;
            log::limited("body.coll.let", 20, "[collide] %s and %s pressed %.0f mm deep: let go until they part", kCollBodyName[p.a], kCollBodyName[p.b],
                         p.depth * 1000.0f);
            continue;
        }
        ps.active = p.act = true;
        {  // its shares: a new contact at its weights; a held one eased toward them
            const float t = p.wa / (p.wa + p.wb);
            ps.share = was ? t + (ps.share - t) * ease : t;
            if (!std::isfinite(ps.share) || std::fabs(ps.share - t) < 0.01f) ps.share = t;
            p.ka = ps.share;
        }
        std::memcpy(ps.n, n, sizeof(n));
        std::memcpy(p.n, n, sizeof(n));
        if (!was && now - ps.end_ms > 120.0) {  // a contact begun (its pulse and knock below, unless the cut lets it go at once)
            p.begin = true;
            p.closing = closing;
            p.in_mm = (rr - s) * 1000.0f;
            for (int k = 0; k < 3; ++k) p.mid[k] = 0.5f * (ca[k] + cb[k]);
        }
    }
    // the moves: a projected Gauss-Seidel over the active pairs (each (x_b - x_a) . n at least its depth), shared by weight
    auto solve = [&]() {
        for (CollBody& bb : B) bb.x[0] = bb.x[1] = bb.x[2] = 0.0f;
        for (int it = 0; it < 16; ++it) {
            bool any = false;
            for (int i = 0; i < np; ++i) {
                const P& p = pr[i];
                if (!p.act) continue;
                float* xa = B[p.a].x;
                float* xb = B[p.b].x;
                const float rel[3] = {xb[0] - xa[0], xb[1] - xa[1], xb[2] - xa[2]};
                const float viol = p.depth - cdot(rel, p.n);
                if (viol <= 1e-5f) continue;
                any = true;
                const float ka = p.ka, kb = 1.0f - p.ka;
                for (int k = 0; k < 3; ++k) {
                    xa[k] -= p.n[k] * viol * ka;
                    xb[k] += p.n[k] * viol * kb;
                }
            }
            if (!any) break;
        }
    };
    // a body its pairs together would move past kCollMaxDepth lets them all go, as a pair pressed that deep does (r240's AH:
    // the hand through the gun's body and grip at once, two pairs under 15 cm each but 15 cm together: cut to 15 cm, the
    // gun was left 11 mm inside the hand, neither held apart nor let go). (The review) one body at a time, the one moved
    // furthest first, then solved again: another body over the depth only through the pairs just cut is not cut
    for (int round = 0; round <= np; ++round) {
        solve();
        int bi = -1;
        float worst = kCollMaxDepth * kCollMaxDepth;
        for (int c = 0; c < kCollN; ++c) {
            const float l2 = cdot(B[c].x, B[c].x);
            if (l2 > worst) worst = l2, bi = c;
        }
        if (bi < 0) break;
        int n_cut = 0;
        for (int i = 0; i < np; ++i) {
            P& p = pr[i];
            if (!p.act || (p.a != bi && p.b != bi)) continue;
            lost[p.a] = lost[p.b] = true;
            p.act = false;
            p.ps->active = false;
            p.ps->latched = true;
            p.ps->end_ms = now;
            ++releases;
            ++n_cut;
        }
        if (!n_cut) break;  // (none of its own to let go: the guard clamp below)
        log::limited("body.coll.let", 20, "[collide] %s moved %.0f mm by its contacts together: %d let go until they part", kCollBodyName[bi],
                     std::sqrt(worst) * 1000.0f, n_cut);
    }
    // the contacts begun this frame and still held: a pulse on the controllers that touched, the log line, the knock (the
    // review: never for one the cut let go at once)
    for (int i = 0; i < np; ++i) {
        const P& p = pr[i];
        if (!p.act || !p.begin) continue;
        ++begun_now;
        const int ja = B[p.a].john, jb = p.cat == 3 ? -1 : B[p.b].john;
        const float amp = p.cat == 0 ? 0.5f : 0.35f;
        for (int j : {ja, jb})
            if (j >= 0 && j <= 1) {
                const int c = f.ctrl[j];
                if (c >= 0 && c <= 1) {
                    pulse_c[c] = true;
                    pulse_a[c] = std::fmax(pulse_a[c], amp);
                }
            }
        log::limited("body.coll.touch", 40, "[collide] contact: %s and %s (%s, %.1f mm in, closing %.2f m/s): a pulse on %s", kCollBodyName[p.a],
                     p.cat == 0 ? kCollBodyName[p.b] : p.cat == 1 ? (p.b == 3 ? "the left hand" : "the right hand")
                                                     : p.cat == 2 ? (p.b == 3 ? "the left forearm" : "the right forearm") : "its own forearm",
                     p.cat == 0 ? "gun on gun" : "gun on arm", p.in_mm, p.closing, p.cat == 3 ? "its controller" : "both controllers");
        if (p.cat == 0 && p.closing >= 0.3f && now - g_coll_sound_ms > 250.0) {
            sound = true;
            g_coll_sound_ms = now;
            for (int k = 0; k < 3; ++k) sound_at[k] = p.mid[k] + base[k];
        }
    }
    // applied: at once while a contact moves it, eased out after (a let-go, or held still by a snap begun mid-contact: the
    // review, no pop of up to kCollMaxDepth in a frame)
    bool active_b[kCollN] = {};
    for (int i = 0; i < np; ++i)
        if (pr[i].act) {
            if (pr[i].ka > 0.0f) active_b[pr[i].a] = true;
            if (pr[i].ka < 1.0f) active_b[pr[i].b] = true;
        }
    static bool s_active_was[kCollN] = {};
    for (int b = 0; b < kCollN; ++b) {
        float l2 = cdot(B[b].x, B[b].x);
        if (l2 > kCollMaxDepth * kCollMaxDepth) {  // (a guard only: past it the pairs were let go above)
            const float s = kCollMaxDepth / std::sqrt(l2);
            for (float& x : B[b].x) x *= s;
        }
        if (!on || !B[b].ok) {
            std::memset(g_coll_app[b], 0, sizeof(g_coll_app[b]));
            g_coll_easing[b] = false;
        } else if (!active_b[b]) {  // eased out (a let-go; held still)
            for (int k = 0; k < 3; ++k) g_coll_app[b][k] = B[b].x[k] + (g_coll_app[b][k] - B[b].x[k]) * ease;
            g_coll_easing[b] = false;
        } else {
            // its onset: in contact again while still easing out, or a pair lost to a let-go: the gap taken once
            const float gap[3] = {g_coll_app[b][0] - B[b].x[0], g_coll_app[b][1] - B[b].x[1], g_coll_app[b][2] - B[b].x[2]};
            if (((!s_active_was[b] && cdot(g_coll_app[b], g_coll_app[b]) > 1e-6f) || lost[b]) && cdot(gap, gap) > 1e-6f) {
                std::memcpy(g_coll_off[b], gap, sizeof(gap));
                g_coll_easing[b] = true;
            }
            if (g_coll_easing[b]) {
                for (float& o : g_coll_off[b]) o *= ease;
                if (cdot(g_coll_off[b], g_coll_off[b]) < 1e-6f) g_coll_easing[b] = false;
            }
            for (int k = 0; k < 3; ++k) g_coll_app[b][k] = B[b].x[k] + (g_coll_easing[b] ? g_coll_off[b][k] : 0.0f);
        }
        s_active_was[b] = active_b[b];
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(g_coll_app[b][k])) g_coll_app[b][k] = 0.0f, g_coll_easing[b] = false;
        if (cdot(g_coll_app[b], g_coll_app[b]) < 4e-8f) std::memset(g_coll_app[b], 0, sizeof(g_coll_app[b]));
    }
    // onto the holding hands (round 43; the control: the springs, their gun's own axes) and the empty hands
    const bool handmove = g_coll_handmove.load(std::memory_order_relaxed);
    for (int r = 0; r < 2; ++r) {
        const CollBody& b = B[r];
        const float* x = g_coll_app[r];
        if (!on || !b.ok || cdot(x, x) <= 0.0f || (r == 1 && f.kick_j[1] != b.john) || (r == 0 && gj != b.john)) continue;  // (kick_j[0] is gj: kick_frame's)
        if (handmove && b.john >= 0 && b.john <= 1) {
            for (int k = 0; k < 3; ++k) f.coll_hand[b.john][k] += x[k];
            f.coll_hand_on[b.john] = true;
            continue;
        }
        for (int j = 0; j < 3; ++j) f.coll_l[r][j] = b.R[j] * x[0] + b.R[3 + j] * x[1] + b.R[6 + j] * x[2];
        f.coll_on[r] = true;
    }
    for (int h = 0; h < 2; ++h) {
        const float* x = g_coll_app[3 + h];
        if (!on || !B[3 + h].ok || holding[h] || cdot(x, x) <= 0.0f) continue;
        std::memcpy(f.coll_hand[h], x, sizeof(f.coll_hand[h]));
        f.coll_hand_on[h] = true;
    }
    for (int c = 0; c < 2; ++c)
        if (pulse_c[c]) controllers::pulse(c, pulse_a[c], 20);
    // the measures: as the moves put them, and the last frame's as drawn (the drawn guns "skel recoil trace" kept, the published
    // drawn arms; the carried gun as held: it never moves)
    float moved_now[4] = {1e9f, 1e9f, 1e9f, 1e9f}, drawn_now[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    for (int i = 0; i < np; ++i) {
        const P& p = pr[i];
        float a0[3], a1[3], b0[3], b1[3], ca[3], cb[3];
        coll_cap(B[p.a], p.ia, g_coll_app[p.a], a0, a1);
        coll_cap(B[p.b], p.ib, g_coll_app[p.b], b0, b1);
        moved_now[p.cat] = std::fmin(moved_now[p.cat], std::sqrt(seg_seg(a0, a1, b0, b1, ca, cb)) - B[p.a].cap[p.ia].r - B[p.b].cap[p.ib].r);
    }
    bool drawn_ok = false;
    bool barrel_ok[2] = {false, false};
    float barrel[2][3] = {};  // each spring's drawn barrel (its root's -z) then
    if (bp_ok && g_kick_trace_on.load(std::memory_order_relaxed)) {
        CollBody D[kCollN];
        bool got[kCollN] = {};
        {
            std::lock_guard lock(g_draw_mutex);
            for (int r = 0; r < 2; ++r) {
                if (!B[r].ok || !g_kick_trace_n[r]) continue;
                const KickTrace& e = g_kick_trace[r][(g_kick_trace_n[r] - 1) % kKickTraceN];
                if (e.frame != bp.frame) continue;
                D[r] = B[r];
                float Rs[9];
                for (int k = 0; k < 3; ++k)
                    for (int j = 0; j < 3; ++j) Rs[k * 3 + j] = e.cur[k * 4 + j];
                const float s12[12] = {Rs[0], Rs[1], Rs[2], 0, Rs[3], Rs[4], Rs[5], 0, Rs[6], Rs[7], Rs[8], 0};
                ortho_cols(s12, D[r].R);
                for (int k = 0; k < 3; ++k) D[r].o[k] = e.cur[k * 4 + 3] - base[k];
                got[r] = true;
                for (int k = 0; k < 3; ++k) barrel[r][k] = -D[r].R[k * 3 + 2];
                barrel_ok[r] = true;
            }
        }
        if (B[2].ok) {  // (as held: its last drawn place, not carried to this frame)
            SecondDrawn sd;
            if (second_drawn(&sd) && sd.frame == bp.frame) {
                D[2] = B[2];
                for (int j = 0; j < 3; ++j)
                    for (int k = 0; k < 3; ++k) D[2].R[k * 3 + j] = sd.m[j * 4 + k];
                for (int k = 0; k < 3; ++k) D[2].o[k] = sd.m[12 + k] - base[k];
                got[2] = true;
            }
        }
        for (int h = 0; h < 2; ++h) {  // the arms as drawn then (their hand's move in)
            if (!B[3 + h].ok) continue;
            CollBody& b = D[3 + h];
            b = B[3 + h];
            float W[3], K[3], E[3];
            for (int k = 0; k < 3; ++k) {
                W[k] = bp.wrist_d[h][k] - base[k];
                K[k] = bp.knuckle_d[h][k] - base[k];
                E[k] = bp.elbow_d[h][k] - base[k];
            }
            float fa[3] = {W[0] - E[0], W[1] - E[1], W[2] - E[2]};
            const float fl = std::sqrt(cdot(fa, fa));
            if (!(fl > 0.05f)) continue;
            for (int k = 0; k < 3; ++k) {
                b.cap[0].a[k] = W[k];
                b.cap[0].b[k] = K[k] + 0.35f * (K[k] - W[k]);
                b.cap[1].a[k] = E[k];
                b.cap[1].b[k] = W[k];
                b.cap[2].a[k] = E[k];
                b.cap[2].b[k] = W[k] - fa[k] / fl * kCollOwnCut;
            }
            got[3 + h] = true;
        }
        for (int i = 0; i < np; ++i) {
            const P& p = pr[i];
            if (!got[p.a] || !got[p.b]) continue;
            float a0[3], a1[3], b0[3], b1[3], ca[3], cb[3];
            coll_cap(D[p.a], p.ia, nullptr, a0, a1);
            coll_cap(D[p.b], p.ib, nullptr, b0, b1);
            drawn_now[p.cat] = std::fmin(drawn_now[p.cat], std::sqrt(seg_seg(a0, a1, b0, b1, ca, cb)) - D[p.a].cap[p.ia].r - D[p.b].cap[p.ib].r);
            drawn_ok = true;
        }
    }
    // the guns as held for actions.cpp (the hammer cocked by the other gun), and the status
    body::CollGuns pub;
    pub.ms = now;
    pub.frame = g_body_frame;
    for (int g = 0; g < 3; ++g) {
        const CollBody& b = B[g];
        body::CollGun& o = pub.gun[g];
        if (!b.ok || !on) continue;
        o.ok = true;
        o.weapon = b.weapon;
        o.john = b.john;
        std::memcpy(o.R, b.R, sizeof(o.R));
        for (int k = 0; k < 3; ++k) o.o[k] = b.o[k] + base[k];
        std::memcpy(o.a, b.cap[0].a, sizeof(o.a));
        std::memcpy(o.b, b.cap[0].b, sizeof(o.b));
        o.r = b.cap[0].r;
        const int sl = g == 0 ? spur_slot(b.weapon) : -1;
        const GunPart* hp = sl >= 0 ? hammer_part(b.weapon) : nullptr;
        if (hp) {
            std::lock_guard lock(g_draw_mutex);  // (g_spur's)
            for (int k = 0; k < 3; ++k) o.spur[k] = hp->pivot[k] + g_spur[sl][k];
            o.spur_ok = true;
        }
    }
    LARGE_INTEGER q1;
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    const float us = qf.QuadPart > 0 ? static_cast<float>(static_cast<double>(q1.QuadPart - q0.QuadPart) * 1e6 / static_cast<double>(qf.QuadPart)) : 0.0f;
    std::lock_guard lock(g_coll_mutex);
    if (!on) g_coll_sound_ok = false;  // (the review: measuring with the option off, no knock left to play)
    CollStats& st = g_coll_st;
    st.on = on;
    st.us_sum += us;
    st.us_max = std::fmax(st.us_max, us);
    ++st.us_n;
    st.measure = meas;
    ++st.frames;
    st.contacts = 0;
    st.pairs = np;
    for (int i = 0; i < np; ++i) st.contacts += pr[i].act ? 1 : 0;
    st.begun += static_cast<uint64_t>(begun_now);
    st.releases += static_cast<uint64_t>(releases);
    st.unresolved += static_cast<uint64_t>(unresolved);
    for (int c = 0; c < 2; ++c) st.pulses[c] += pulse_c[c] ? 1 : 0;
    if (f.coll_on[0] || f.coll_on[1]) ++st.moved;
    if (f.coll_hand_on[0] || f.coll_hand_on[1]) ++st.hand_frames;
    for (int g = 0; g < 2; ++g)
        for (int c = 0; c < 2; ++c) st.hand_in[g][c] = std::fmax(st.hand_in[g][c], hand_in[g][c]);
    for (int h = 0; h < 2; ++h) {
        st.holding[h] = holding[h];
        st.loading[h] = loading[h];
        st.grip[h] = grip[h];
    }
    for (int b = 0; b < kCollN; ++b) {
        st.body_ok[b] = B[b].ok;
        st.weapon[b] = B[b].weapon;
        std::memcpy(st.push[b], g_coll_app[b], sizeof(st.push[b]));
        st.max_push[b] = std::fmax(st.max_push[b], std::sqrt(cdot(g_coll_app[b], g_coll_app[b])));
    }
    for (int c = 0; c < 4; ++c) {
        st.held_now[c] = held_now[c];
        st.drawn_now[c] = drawn_now[c];
        if (held_now[c] < 1e8f) {
            st.held_min[c] = std::fmin(st.held_min[c], held_now[c]);
            ++st.held_n[c];
        }
        if (moved_now[c] < 1e8f) st.moved_min[c] = std::fmin(st.moved_min[c], moved_now[c]);
        if (drawn_now[c] < 1e8f) {
            st.drawn_min[c] = std::fmin(st.drawn_min[c], drawn_now[c]);
            ++st.drawn_n[c];
        }
    }
    if (drawn_ok) ++st.drawn_frames;
    for (int r = 0; r < 2; ++r) {
        st.barrel_ok[r] = barrel_ok[r];
        std::memcpy(st.barrel[r], barrel[r], sizeof(st.barrel[r]));
    }
    {
        std::string l;
        char b[96];
        for (int i = 0; i < np && l.size() < 400; ++i) {
            const P& p = pr[i];
            if (!p.act) continue;
            std::snprintf(b, sizeof(b), " %d.%d-%d.%d %.1fmm", p.a, p.ia, p.b, p.ib, p.depth * 1000.0f);
            l += b;
        }
        std::snprintf(st.list, sizeof(st.list), "%s", l.c_str());
    }
    if (sound) {
        g_coll_sound_ok = true;
        std::memcpy(g_coll_sound_at, sound_at, sizeof(g_coll_sound_at));
        ++st.sounds;
    }
    g_coll_pub = pub;
}
// This frame's kick of both springs (before_scene, once a frame, the render thread): the shots since the last frame
// strike the springs, the springs stepped to now (exactly: s(t) = (s0 + (v0 + w s0) t) e^-wt), and f's kick made. A gap
// over 250 ms (first person off, a load) settles them and takes the shots meanwhile as seen; so does another gun (the
// review: a switch within 250 ms of a shot kicked the new item; kick_key: the weapon in hand, the second gun's slot and
// weapon or the copy). The counters are read every frame, with the option off too, so turning it on never kicks for old
// shots. Two-handed, the turn is also capped so the foregrip (the published one's distance ahead of the root, else
// kKickForeReach) lifts at most kKickForeLift, whatever the strength or the stacking; the slide is kept.
constexpr float kKickForeReach = 0.5f, kKickForeLift = 0.005f;
void kick_frame(Frame& f) {
    const float k = g_recoil.load(std::memory_order_relaxed);
    const bool trace = g_kick_trace_on.load(std::memory_order_relaxed);
    const double now = log::now_ms();
    int sec_w = -1;
    const uint64_t n[2] = {aim::player_shots(), dual::shots_fired(&sec_w)};
    const int w[2] = {f.weapon, sec_w};
    f.kick_j[0] = f.gun_j >= 0 && f.gun_j <= 1 ? f.gun_j : -1;
    f.kick_key[0] = f.weapon;
    float two_cap = std::asin(kKickForeLift / kKickForeReach) * 57.29578f;  // (deg)
    if (k > 0.0f && f.two_blend > 0.0f) {
        BodyPoints bp;
        if (body_points(&bp) && bp.fore_ok && bp.gun_frame_ok) {  // (the last frame's: the gun as held, unkicked)
            const float* G = bp.gun_frame_R;  // columns: x right, y up, z back
            float d = 0.0f;
            for (int i = 0; i < 3; ++i) d -= (bp.fore[i] - bp.gun_frame_o[i]) * G[i * 3 + 2];
            if (std::isfinite(d)) two_cap = std::asin(kKickForeLift / std::fmin(1.0f, std::fmax(0.2f, d))) * 57.29578f;
        }
    }
    std::lock_guard lock(g_kick_mutex);
    for (int r = 0; r < 2; ++r) {
        KickSpring& sp = g_kick[r];
        double dt = (now - sp.step_ms) * 0.001;
        sp.step_ms = now;
        const bool switched = sp.key_ok && sp.key != f.kick_key[r];
        if (switched) ++sp.switches;
        sp.key = f.kick_key[r];
        sp.key_ok = true;
        if (!sp.seen_ok || !(dt >= 0.0) || dt > 0.25 || switched) {
            sp.s = sp.v = 0.0f;
            sp.seen = n[r];
            sp.seen_ok = true;
            dt = 0.0;
        }
        if (dt > 0.0 && (sp.s != 0.0f || sp.v != 0.0f)) {
            const float t = static_cast<float>(dt), e = std::exp(-kKickRate * t), c = sp.v + kKickRate * sp.s;
            const float s1 = (sp.s + c * t) * e, v1 = (sp.v - kKickRate * c * t) * e;
            sp.s = s1, sp.v = v1;
            if (!std::isfinite(sp.s) || !std::isfinite(sp.v) || (std::fabs(sp.s) < 1e-4f && std::fabs(sp.v) < 1e-3f)) sp.s = sp.v = 0.0f;
        }
        uint64_t add = n[r] > sp.seen ? n[r] - sp.seen : 0;
        sp.seen = n[r];
        sp.shots += add;
        const int pm = g_kick_pulse_ms[r].load(std::memory_order_relaxed);
        if (pm > 0 && now - sp.pulse_ms >= pm) {  // (a test's impulse: the gun in that hand's own peak)
            sp.pulse_ms = now;
            ++sp.pulses;
            ++add;
        }
        if (add) {
            kick_peak(w[r] >= 0 ? w[r] : f.weapon, &sp.deg, &sp.back);
            sp.weapon = w[r] >= 0 ? w[r] : f.weapon;
            sp.v += kKickRate * kKickE * static_cast<float>(add < 2 ? add : 2);  // (a Dead Eye volley: two at most a frame)
            sp.shot_ms = now;
        }
        if (sp.s > 2.5f) sp.s = 2.5f, sp.v = std::fmin(sp.v, 0.0f);  // (a held stream of shots stacks, within reason)
        sp.peak_s = std::fmax(sp.peak_s, sp.s);
        float turn = k * sp.s * sp.deg;
        const float back = k * sp.s * sp.back;
        if (r == 0 && f.two_blend > 0.0f) {  // two-handed: a tenth of the turn, at most two_cap
            const float b = std::fmin(1.0f, f.two_blend);
            const float tt = std::fmax(-two_cap, std::fmin(two_cap, kKickTwoTurn * turn));
            turn += (tt - turn) * b;
        }
        sp.now_turn = turn, sp.now_back = back;
        f.kick_s[r] = sp.s;
        f.kick_shot_ms[r] = sp.shot_ms;
        // [Reload] ThumbCock (r230): the gun in hand rolled back in the hand, muzzle up about its origin, as the thumb takes the
        // hammer and rides it (by the thumb's weight): the hammer down was out of the thumb's reach (17.7 mm short, thumb_01 at
        // its cap). Drawn only, as the kick: the aim and the shots stay on the gun as held
        const float roll = r == 0 && f.thumb_w >= 0 && f.thumb_wt > 0.0f ? g_thumb_roll.load(std::memory_order_relaxed) * std::fmin(1.0f, f.thumb_wt) : 0.0f;
        const float tturn = turn + roll;
        // [Hands] GunCollision (2026-10-29): the gun held apart at a contact (coll_frame's), a slide in its own axes after the kick
        const bool col = f.coll_on[r] && std::isfinite(f.coll_l[r][0] + f.coll_l[r][1] + f.coll_l[r][2]);
        f.kick_on[r] = (k > 0.0f || roll > 0.0f || col) && f.kick_j[r] >= 0 && std::isfinite(tturn) && std::isfinite(back) &&
                       (std::fabs(tturn) > 0.005f || std::fabs(back) > 5e-5f || col);
        if (f.kick_on[r]) {
            kick_local(tturn, back, f.kick_l[r]);
            if (col)
                for (int j = 0; j < 3; ++j) f.kick_l[r][j * 4 + 3] += f.coll_l[r][j];
        }
    }
    f.kick_any = f.kick_on[0] || f.kick_on[1] || trace;
    f.kick_aim_ok = trace && aim::barrel_in_target(f.kick_aim);
}
// A rigid record half (16 floats: the axes in rows 0-2, the position at 12-14) as M (12 floats: the axes as columns)
void rec_to34(const float* r, float* m) {
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 3; ++c) m[i * 4 + c] = r[c * 4 + i];
        m[i * 4 + 3] = r[12 + i];
    }
}
void rec_from34(const float* m, float* r) {
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 3; ++c) r[c * 4 + i] = m[i * 4 + c];
        r[12 + i] = m[i * 4 + 3];
    }
}
// a move X (in the gun's axes) put on one half of a draw: a skinned set's `count` matrices (the root first: M X itself;
// every other bone K B, K = M X M^-1), or with count 0 a rigid record half (16 floats). Every value checked finite into
// `out` first; false: none written (the caller leaves the half as it was)
bool kick_half(float* m, int count, const float* X, float* out) {
    const int per = count ? count : 1;
    float root[12], ri[12], K[12];
    if (count) std::memcpy(root, m, sizeof(root));
    else rec_to34(m, root);
    if (!finite_n(X, 12) || !finite_n(root, 12)) return false;
    if (per > 1) {
        if (!inv34(root, ri)) return false;
        mul34(root, X, K);
        mul34(K, ri, K);
        if (!finite_n(K, 12)) return false;
    }
    for (int j = 0; j < per; ++j) {
        // (the root, and a rigid record in the world's large coordinates, as M X: no round trip of K through them)
        if (j == 0) mul34(root, X, out);
        else mul34(K, m + static_cast<size_t>(j) * 12, out + j * 12);
        if (!finite_n(out + j * 12, 12)) return false;
    }
    for (int j = 0; j < per; ++j) {
        if (count) std::memcpy(m + static_cast<size_t>(j) * 12, out + j * 12, sizeof(float) * 12);
        else rec_from34(out, m);
    }
    return true;
}
// spring r's kick put on a draw of its gun (John's hand `side` holding it): a skinned set's `count` matrices of each
// half (the root first; or a set under its record's matrix, its local matrices: the record is a fixed left factor, so
// M0 X there is the drawn root's X too), or with count 0 a rigid record and its previous half (m + 16). The current half takes this
// frame's kick (none while its hand's IK is off), the previous half last frame's as drawn then (kick_then: the motion
// vectors carry the kick's own motion), each after its `undo`'s inverse (the copy: the kick the gun in hand's draw it is
// made from already carries, undo_cur on its current half, undo_prev on its previous). applied_cur / applied_prev (if
// given): the spring's own kick put on each half (the identity: none). A half with a non-finite value is left as it was
// (counted). The frame's draws made with its correction note this frame's kick (g_kick_applied).
void kick_draw(int r, int side, float* m, int count, float* applied_cur, float* applied_prev, const float* undo_cur = nullptr,
               const float* undo_prev = nullptr) {
    if (applied_cur) std::memcpy(applied_cur, kKickI, sizeof(kKickI));
    if (applied_prev) std::memcpy(applied_prev, kKickI, sizeof(kKickI));
    if (r < 0 || r > 1 || !m || count < 0 || count > 8 || !g_frame.valid || !g_frame.kick_any || side < 0 || side > 1 || g_frame.kick_j[r] < 0)
        return;
    const bool early = g_corr_frame != g_body_frame;
    const bool own = g_frame.kick_on[r] && g_frame.ik[side];
    if (g_frame.kick_on[r] && !g_frame.ik[side]) g_kick_noik[r].fetch_add(1, std::memory_order_relaxed);
    const float* Lh[2] = {own ? g_frame.kick_l[r] : kKickI, kick_then(r)};
    const float* U[2] = {undo_cur, undo_prev};
    bool moved = false, bad = false;
    if (g_kick_trace_on.load(std::memory_order_relaxed) && kick_entry_draw(r, early) && g_kick_trace_frame[r] != g_body_frame &&
        g_kick_pre[r].frame != g_body_frame) {
        KickPre& kp = g_kick_pre[r];
        kp.frame = g_body_frame;
        kp.m = m;
        if (count) std::memcpy(kp.root, m, sizeof(kp.root));
        else rec_to34(m, kp.root);
    }
    for (int h = 0; h < 2; ++h) {
        float X[12];  // undo^-1 o L
        std::memcpy(X, Lh[h], sizeof(X));
        if (U[h] && !is_identity34(U[h])) {
            float ui[12];
            if (!inv34(U[h], ui)) {
                bad = true;
                continue;
            }
            mul34(ui, X, X);
        }
        if (is_identity34(X)) continue;
        float out[8 * 12];
        if (kick_half(m + (count ? static_cast<size_t>(h) * count * 12 : static_cast<size_t>(h) * 16), count, X, out)) moved = true;
        else bad = true;
    }
    if (bad) g_kick_bad[r].fetch_add(1, std::memory_order_relaxed);
    if (moved) {
        g_kick_draws[r].fetch_add(1, std::memory_order_relaxed);
        if (early) g_kick_early[r].fetch_add(1, std::memory_order_relaxed);
    }
    if (applied_cur && own) std::memcpy(applied_cur, g_frame.kick_l[r], sizeof(float) * 12);
    if (applied_prev) std::memcpy(applied_prev, Lh[1], sizeof(float) * 12);
    {  // (after kick_then's read: a later draw of the frame reads last frame's from `prev`. Every draw, early ones too: they put
       // on the frame's same kick, and the second gun's draws may all come before the frame's correction)
        KickApplied& ka = g_kick_applied[r];
        if (ka.frame != g_body_frame) {
            ka.prev_frame = ka.frame;
            ka.prev_key = ka.key;
            std::memcpy(ka.prev, ka.L, sizeof(ka.prev));
            ka.frame = g_body_frame;
        }
        ka.key = g_frame.kick_key[r];
        std::memcpy(ka.L, own ? g_frame.kick_l[r] : kKickI, sizeof(ka.L));
    }
}
// StaleGunFix moved an early draw of the gun in hand (its current half) to last frame's drawn gun, which carried last
// frame's kick (kick_then): that kick undone and this frame's (`now`, the one kick_draw put on it before) put on
// instead, so the depth and shadow pre-pass's gun kicks with the main draw's, not a frame behind it (recoil.md 5: up to
// 3 deg off for a frame as the kick rises). The current half only: its previous half is last frame's drawn gun as it
// was (its motion vectors, with last frame's kick).
void kick_stale(const float* now, float* m, int count, int r = 0) {  // (F10: r, the spring: 1 the second gun's or the copy's)
    if (count < 0 || count > 8 || r < 0 || r > 1) return;  // (with the option off too: last frame's kick, if any, undone)
    const float* then = kick_then(r);
    if (!std::memcmp(then, now, sizeof(float) * 12)) return;
    float ti[12], X[12], out[8 * 12];
    if (!inv34(then, ti)) return;
    mul34(ti, now, X);  // then^-1 o now, in the gun's axes
    if (!kick_half(m, count, X, out)) g_kick_bad[r].fetch_add(1, std::memory_order_relaxed);
}
// The drawn root without its kick (the diagnostics: [Debug] TwoHandShotLog's post and "skel follow" measure the hand on
// the gun as held, not the kick): M L^-1 (12 floats, the root's layout)
void kick_unkick(const float* m, const float* L, float* out) {
    float li[12];
    if (is_identity34(L) || !inv34(L, li)) {
        std::memcpy(out, m, sizeof(float) * 12);
        return;
    }
    mul34(m, li, out);
}
// "skel recoil trace" (on: a relaxed load per draw site otherwise): a draw of spring r's gun as it leaves this hook,
// the set's offsets (world less the set: off_cur, off_prev; nullptr: a rigid record, in the world). The frame's early
// draw is kept for its frame's entry; the frame's first draw made with its correction makes the entry.
// rec (2026-10-27, cycle r179): a set under its record's matrix (the set's matrices local to it): the world root is the
// record (its current half at rec, its previous at rec + 16) times the set's root.
void kick_note(int r, int side, const float* m, int count, const float* off_cur, const float* off_prev, const float* rec = nullptr) {
    if (!g_kick_trace_on.load(std::memory_order_relaxed) || r < 0 || r > 1 || side < 0 || side > 1 || !m || count < 0 || count > 8 ||
        !g_frame.valid || (rec && !count))
        return;
    const bool early = g_corr_frame != g_body_frame;
    float M[12], P[12];
    if (count) {
        std::memcpy(M, m, sizeof(M));
        std::memcpy(P, m + static_cast<size_t>(count) * 12, sizeof(P));
    } else {
        rec_to34(m, M);
        rec_to34(m + 16, P);
    }
    for (int k = 0; k < 3; ++k) {
        if (off_cur) M[k * 4 + 3] += off_cur[k];
        if (off_prev) P[k * 4 + 3] += off_prev[k];
    }
    if (rec) {
        float Rc[12], Rp[12];
        rec_to34(rec, Rc);
        rec_to34(rec + 16, Rp);
        mul34(Rc, M, M);
        mul34(Rp, P, P);
    }
    const float* R = g_frame.ik_r[side];  // columns: the target's axes
    const float* T = g_frame.ik_t[side];
    float bt[3], ut[3], pt[3];
    for (int k = 0; k < 3; ++k) {
        bt[k] = -(R[0 * 3 + k] * M[2] + R[1 * 3 + k] * M[6] + R[2 * 3 + k] * M[10]);
        ut[k] = R[0 * 3 + k] * M[1] + R[1 * 3 + k] * M[5] + R[2 * 3 + k] * M[9];
        pt[k] = R[0 * 3 + k] * (M[3] - T[0]) + R[1 * 3 + k] * (M[7] - T[1]) + R[2 * 3 + k] * (M[11] - T[2]);
    }
    norm(bt);
    norm(ut);
    if (!kick_entry_draw(r, early)) {  // the gun in hand's pre-pass draw: kept for its frame's entry
        KickEarly& ke = g_kick_early_s[r];
        if (ke.frame == g_body_frame) return;  // (the frame's first)
        ke.frame = g_body_frame;
        std::memcpy(ke.bt, bt, sizeof(bt));
        std::memcpy(ke.pt, pt, sizeof(pt));
        return;
    }
    if (g_kick_trace_frame[r] == g_body_frame) return;
    g_kick_trace_frame[r] = g_body_frame;
    KickTrace& e = g_kick_trace[r][g_kick_trace_n[r]++ % kKickTraceN];
    e = KickTrace{};
    e.ms = log::now_ms();
    e.frame = g_body_frame;
    e.shot_ms = g_frame.kick_shot_ms[r];
    e.s = g_frame.kick_s[r];
    e.skinned = count > 0;
    std::memcpy(e.bt, bt, sizeof(bt));
    std::memcpy(e.ut, ut, sizeof(ut));
    std::memcpy(e.pt, pt, sizeof(pt));
    std::memcpy(e.cur, M, sizeof(M));
    std::memcpy(e.prev, P, sizeof(P));
    e.aim_ok = r == 0 && g_frame.kick_aim_ok;
    std::memcpy(e.aim, g_frame.kick_aim, sizeof(e.aim));
    const KickPre& kp = g_kick_pre[r];
    if (kp.frame == g_body_frame && kp.m == m) {  // the delivered kick: D = pre^-1 o now (both in the draw's own space)
        float now[12], pi[12], D[12];
        if (count) std::memcpy(now, m, sizeof(now));
        else rec_to34(m, now);
        if (inv34(kp.root, pi)) {
            mul34(pi, now, D);
            if (finite_n(D, 12)) {
                e.d_ok = true;
                e.d_turn = std::atan2(D[9], D[5]) * 57.29578f;
                e.d_slide = D[11];
                e.d_r1z = D[6];
                e.d_r1t = D[7];
            }
        }
    }
    const KickEarly& ke = g_kick_early_s[r];
    if (ke.frame == g_body_frame) {
        e.early_ok = true;
        std::memcpy(e.early_bt, ke.bt, sizeof(ke.bt));
        std::memcpy(e.early_pt, ke.pt, sizeof(ke.pt));
    }
    BodyPoints bp;  // (the published, unkicked: the gun as held; a test's trace only)
    if (r == 0 && body_points(&bp) && bp.fore_ok && bp.gun_frame_ok) {
        float d = 0.0f;
        for (int i = 0; i < 3; ++i) d -= (bp.fore[i] - bp.gun_frame_o[i]) * bp.gun_frame_R[i * 3 + 2];
        e.fore_d = std::isfinite(d) ? d : -1.0f;
    }
}
// run 12 item 9, [Body] ArmMotionVectors: the player's set's previous half (the motion vectors) from its bones as drawn
// last frame, as GunMotionVectors does for the gun: transform_set's slot 1 solves the arms to this frame's targets, so a
// moving hand and forearm read as still to DLSS and TAA. Per drawable (rec[0x10]; the set itself is made anew each frame: cycle AM1), the frame's first draw made with the
// frame's correction keeps its drawn world matrices; each draw's previous half becomes last frame's (the 1 m guard at
// the root; stale draws as they were). Under g_draw_mutex.
struct SetPrev {
    uintptr_t key = 0;
    int count = 0, prev_count = 0;
    uint64_t frame = ~0ull, prev_frame = ~0ull;
    float cur[256 * 12] = {}, prev[256 * 12] = {};
};
SetPrev g_set_prev[8];
std::atomic<bool> g_arm_mv{false};
struct ArmMvStats {
    uint64_t fixed = 0, none = 0, cut = 0, n = 0;
    double sum = 0.0;  // the right wrist's previous half (as the mod's slot 1 made it) off last frame's drawn wrist (mm)
    float max = 0.0f;
};
ArmMvStats g_arm_mv_cmd;
void set_prev_fix(uintptr_t key, float* sm, int count, const float* off_cur, const float* off_prev) {
    if (!key || count <= 0 || count > 256 || g_corr_frame != g_body_frame) return;
    SetPrev* g = nullptr;
    for (SetPrev& e : g_set_prev)
        if (e.key == key) g = &e;
    if (!g) {
        g = &g_set_prev[0];
        for (SetPrev& e : g_set_prev)
            if (e.frame == ~0ull || (g->frame != ~0ull && e.frame < g->frame)) g = &e;
        g->key = key;
        g->frame = g->prev_frame = ~0ull;
        g->count = g->prev_count = 0;
    }
    const size_t nf = static_cast<size_t>(count) * 12;
    if (g->frame != g_body_frame) {
        g->prev_frame = g->frame;
        g->prev_count = g->count;
        std::memcpy(g->prev, g->cur, sizeof(float) * static_cast<size_t>(g->count) * 12);
        g->frame = g_body_frame;
        g->count = count;
        std::memcpy(g->cur, sm, sizeof(float) * nf);
        for (int j = 0; j < count; ++j)
            for (int k = 0; k < 3; ++k) g->cur[j * 12 + k * 4 + 3] += off_cur[k];
    }
    ArmMvStats& st = g_arm_mv_cmd;
    if (g->prev_frame == ~0ull || g->prev_frame + 1 != g_body_frame || g->prev_count != count) {
        ++st.none;
        return;
    }
    float* prev = sm + nf;
    float d2 = 0.0f;  // the root's previous place, the set's against the drawn
    for (int k = 0; k < 3; ++k) {
        const float a = prev[k * 4 + 3] + off_prev[k] - g->prev[k * 4 + 3];
        d2 += a * a;
    }
    if (d2 > 1.0f) {
        ++st.cut;
        return;
    }
    const int wb = g_rig.wrist[1];
    if (wb >= 0 && wb < count) {
        float e2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float a = prev[wb * 12 + k * 4 + 3] + off_prev[k] - g->prev[wb * 12 + k * 4 + 3];
            e2 += a * a;
        }
        const float mm = std::sqrt(e2) * 1000.0f;
        st.max = std::fmax(st.max, mm);
        st.sum += mm;
        ++st.n;
    }
    if (!g_arm_mv.load(std::memory_order_relaxed)) return;
    std::memcpy(prev, g->prev, sizeof(float) * nf);
    for (int j = 0; j < count; ++j)
        for (int k = 0; k < 3; ++k) prev[j * 12 + k * 4 + 3] -= off_prev[k];
    ++st.fixed;
}
uint64_t g_pub_frame = ~0ull;  // the frame g_hand_seq's data was last published for (the render thread's)
std::vector<Corr> g_corr_set;    // the latest current set's, in the set's own space (the world less its offset)
float g_ik_diag[2][20];          // per hand: S, E, W, T (set space), l1, l2, d, the world target's y, the wrist as drawn, the stretch
float g_cam_diag[12];            // the frame's camera position, the head bone, the root bone, the right wrist target (world)
// "skel still" (2026-10-11): the drawn pelvis, spine00, spine03, the clavicles, the shoulders, the head and the feet against
// the camera in the view's heading (right, up, forward): each one's min xyz and max xyz since the last reset
constexpr int kStillDiag = 11;
float g_still_rng[kStillDiag][6];
uint64_t g_still_n = 0, g_still_sets = 0, g_still_refused = 0;
std::atomic<uint64_t> g_still_off_cam{0};  // (the body job's, outside g_draw_mutex)
// run 12 item 8, [Horse] BodyWithCamera (the user: "When the horse is going fast, Johns body moves back in the saddle,
// leading to holsters being moved back with him. Find a way to keep his body with the camera"): the still stance's body
// is held at the camera less an offset learned while the camera stands; a jump of 0.3 m between that offset and the
// actor's (a script tick old, and the riding animation's root) re-learned it at once. Riding, a spur's start, a rear and
// a gallop's tick lag (6 m/s: 0.2 m) moved the actor that far (cycle GL1: the pelvis 0.95 m up and 0.30 m back against
// the camera at a canter's start, then held 4 cm further back). With the switch on and the player mounted, only a jump
// over 1.5 m (a teleport, a new mount) re-learns it: the body stays where it sat in the saddle standing.
std::atomic<bool> g_body_with_cam{true};
std::atomic<uint64_t> g_still_resets{0}, g_still_resets_riding{0}, g_still_kept_riding{0};
bool g_still_init = false;
float g_feet_err[2] = {0, 0};  // run 10 item 9 stage 2: each drawn foot's distance from its animated place (m), the last set
float g_knee_deg[2] = {0, 0}, g_knee_rng[2][2] = {{1e9f, -1e9f}, {1e9f, -1e9f}}, g_hip_h[2] = {0, 0};  // run 10 item 8b (under g_draw_mutex)
std::vector<uint8_t> g_still_carried;
void still_reset() {
    for (auto& r : g_still_rng)
        for (int k = 0; k < 3; ++k) {
            r[k] = 1e30f;
            r[3 + k] = -1e30f;
        }
    g_still_n = g_still_sets = g_still_refused = 0;
    g_still_off_cam = 0;
    g_still_resets = g_still_resets_riding = g_still_kept_riding = 0;
    for (auto& r : g_knee_rng) r[0] = 1e9f, r[1] = -1e9f;
    g_still_init = true;
}
float g_torso_diag[2] = {0, 0};  // the torso lock's last horizontal correction (m) and the largest since the last read
// The hands' world corrections for the aim (aim.cpp reads them on the game thread): the wrist bones' T_world of the
// latest current player set, written on the render thread behind a sequence counter (odd while writing).
struct HandCorr {
    float A[9], a[3];
    double ad[3];
    double ms;
    bool ik;
    // (GOAL-weapons-handling item 1b, after optimumbox's fork, v0.8.11.10, MIT) g_item_corr only: the item's game matrix
    // (W +0x80) of the build this correction was drawn with (the frame's newest held sample, as g_bt_body takes it) and
    // its W. The game moves that matrix between builds (an animation's clip: the fork measured up to 19 degrees in a
    // frame on the Schofield, the hand still; r178 saw the same swing of our aim's barrel), so the aim's live read of it
    // against this correction turned the barrel for a frame. The game's own matrix: GunKick's turn is never in it
    bool gun_ok;
    float gun[16];
    uintptr_t W;
    uint64_t frame;  // the body frame it was published in (the review's drawn-gun check pairs by it)
};
std::atomic<uint32_t> g_hand_seq{0};
std::atomic<uint64_t> g_seq_waits{0}, g_seq_misses{0};
std::atomic<uintptr_t> g_player_drawable{0};  // rec[0x10] of the player's set draw (the "skel geoms" probe)
std::atomic<uintptr_t> g_player_drawables[8] = {};  // run 10 item 9b: each distinct drawable drawn on the player's set (the body, its accessories)
// [Body] HiddenGeometry (round 8: the sliver from elbow to elbow was still there). The character shaders compute the
// world position in float (gWorldSingle x the skinned position, then less the camera): at the world's coordinates
// (|z| about 2400, a float step of 0.24 mm) the collapsed hidden points, all on the line between the cuts, round off it
// independently and the hidden triangles get a fraction of a pixel of area (eps-independent; with every target on one
// world-x line it vanished). The player's model is cut into small geometries of 4-7 bones each (their palettes):
// while parts are hidden, a geometry whose every bone is hidden is not drawn at all (FUN_140167720 skipped whole, its
// begin and end together); the rest are collapsed as before. Render thread; SEH reads, no system calls.
using DrawModelGeometry_t = void (*)(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6);
DrawModelGeometry_t o_draw_geom = nullptr;
// [Body] HiddenGeometry: 0 draw (as the game), 1 skip (every bone hidden: not drawn; round 8, it thins the line), 2 filter
// (run 3: skip those, and draw a mixed geometry without its hidden triangles that span two cut targets: the sliver)
std::atomic<int> g_geom_mode{0};
thread_local int t_cull_show = 0;  // the player's draw in a show mode (1 forearms, 2 hands), around o_DrawVisEntity
// 3 (the review): each player drawable's last draw (body frames), so an outfit's old drawables age out of the list
std::atomic<uint64_t> g_player_drawn_at[8] = {};
// the filter's entries (render thread): one per (geometry, index buffer, count, show mode, rig)
// a filtered geometry's draw: its index runs (the kept triangles), drawn instead of its whole index range
constexpr int kCullRuns = 64;  // at most this many index runs per geometry (one draw packet each), else it is drawn whole
struct CullDraw {
    uint32_t icount = 0;  // the game's index count for this geometry (a draw of another count is drawn as is)
    uint8_t nruns = 0;
    struct Run {
        uint16_t start, count;  // in indices, within the game's own range
    } runs[kCullRuns] = {};
};
struct CullEntry {
    uintptr_t geom = 0, ib = 0;
    uint32_t icount = 0, rig_gen = 0;
    uint8_t show = 0, kind = 0;  // kind: 1 skip (no kept triangle), 2 draw (none to drop), 3 filter, 4 rejected (drawn as is)
    uint8_t reading = 0;         // the blend indices: 1 through the palette, 2 direct, 3 both (agreeing)
    uint8_t why = 0;  // a rejection's reason: 1 not a triangle list, 7 double-buffered, 8 an offset buffer, 9 no buffers, 10 the
                      // palette, 11 no data, 12 the index count, 13 counts disagree, 14 the vertex layout, 2 the blend channels, 3 an
                      // index or bone out of range, 4 the two readings disagree, 5 over kCullRuns runs, 6 a fault
    uint16_t tris = 0, keep = 0, drop = 0, any = 0, verts = 0, w255 = 0;
    uint64_t used = 0;  // the body frame it was last used in (a full window replaces the least recently used)
    CullDraw draw;
};
thread_local CullEntry t_cull[64];
thread_local uint64_t t_cull_frame = ~0ull;
thread_local int t_cull_builds = 0;
thread_local const CullDraw* t_cull_draw = nullptr;
thread_local bool t_cull_recorded = false;  // a draw packet was recorded while t_cull_draw was set
std::atomic<uint64_t> g_cull_by_pass[8] = {};  // the filtered geometry draws by DrawVisEntity pass (& 7)
// the passes whose draws are filtered (DrawVisEntity's pass & 7): the eye passes, 1 and 3 (ENGINE-NOTES: pass 0, likely
// the shadows, drew about 78% of the splits). Elsewhere the mixed geometries are drawn whole, as the game draws them.
std::atomic<uint32_t> g_cull_pass_mask{(1u << 1) | (1u << 3)};
std::atomic<uint64_t> g_cull_skipped{0}, g_cull_filtered{0}, g_cull_runs{0}, g_cull_mismatch{0}, g_cull_nodraw{0}, g_cull_built{0},
    g_cull_kind[5] = {};
// the last built entries' summaries, for the log (before_scene) and "skel cull list"
struct CullNote {
    uintptr_t geom;
    uint8_t show, kind, reading, why, nruns;
    uint16_t tris, keep, drop, any, verts, w255;
};
std::mutex g_cull_note_mutex;
CullNote g_cull_notes[64];
uint32_t g_cull_note_n = 0, g_cull_note_logged = 0;
struct GeomClass {
    uintptr_t geom = 0, pal = 0;
    uint16_t cnt = 0;
    int show = 0;
    uint8_t cls = 0;  // 1 drawn, 2 skipped (every bone hidden), 3 not judged (drawn), 4 drawn with hidden torso bones
};
thread_local GeomClass t_geom_cache[64];
std::atomic<uint64_t> g_geoms_seen[5] = {};  // per class, the player's geometry draws

uint8_t classify_geom(uintptr_t pal, uint16_t cnt, int show) {
    uint16_t bones[128];
    if (!pal || cnt == 0 || cnt > 128 || !raw(pal, bones, static_cast<size_t>(cnt) * 2)) return 3;
    bool shown = false, torso = false;
    for (int i = 0; i < cnt; ++i) {
        const int b = bones[i];
        if (b >= g_rig.count || b >= static_cast<int>(g_rig.in_elbow.size()) || b >= static_cast<int>(g_rig.in_wrist.size()) ||
            b >= static_cast<int>(g_rig.side.size()))
            return 3;
        if (show == 1 ? g_rig.in_elbow[b] : g_rig.in_wrist[b])
            shown = true;
        else if (g_rig.side[b] < 0)
            torso = true;
    }
    return shown ? (torso ? 4 : 1) : 2;
}

// The filter's builder (research\run3\sliver.md 4): the geometry's CPU vertex and index data (resident), its blend
// weights and indices (vertex channels 1 and 2, 4 bytes each), each triangle classified against this frame's collapse
// (transform_set): KEEP (a vertex on a shown bone), DROP (no shown vertex, its hidden influences reach two or more of
// the targets: either arm's cut or their midpoint), ANY (the rest: a single target, a speck). The runs are the spans
// between DROP triangles that hold a KEEP one. Plain data only (SEH); render thread.
int vtx_channel_offset(uint32_t mask, uint8_t ordered, uint64_t fmt, uint32_t ch) {  // FUN_140173de0
    static const int kSize[16] = {2, 4, 6, 8, 4, 8, 12, 16, 4, 4, 4, 8, 8, 0, 4, 8};
    static const uint32_t kOrder[18] = {0, 3, 14, 15, 6, 7, 8, 9, 10, 11, 12, 13, 1, 16, 17, 2, 4, 5};
    if (!(mask & (1u << ch))) return -1;
    int off = 0;
    if (!ordered) {
        for (uint32_t j = 0; j < ch; ++j)
            if (mask & (1u << j)) off += kSize[(fmt >> ((j * 4) & 63)) & 0xf];
        return off;
    }
    for (uint32_t j : kOrder) {
        if (!(mask & (1u << j))) continue;
        if (j == ch) return off;
        off += kSize[(fmt >> ((j * 4) & 63)) & 0xf];
    }
    return -1;
}
// per vertex: bit 7 shown, bits 0-2 the targets (0 left cut, 1 right cut, 2 the midpoint); 0xff an invalid bone
uint8_t vtx_class(const uint8_t* w, const uint8_t* idx, bool via_palette, const uint16_t* pal, int palcount, int show, bool collapse_root,
                  uint16_t* sum) {
    uint8_t c = 0;
    *sum = static_cast<uint16_t>(w[0] + w[1] + w[2] + w[3]);
    for (int i = 0; i < 4; ++i) {
        if (!w[i]) continue;
        int b = idx[i];
        if (via_palette) {
            if (b >= palcount) return 0xff;
            b = pal[b];
        }
        if (b < 0 || b >= g_rig.count || b >= static_cast<int>(g_rig.in_elbow.size()) || b >= static_cast<int>(g_rig.in_wrist.size()) ||
            b >= static_cast<int>(g_rig.side.size()))
            return 0xff;
        const bool shown = show == 1 ? g_rig.in_elbow[b] != 0 : g_rig.in_wrist[b] != 0;
        if (shown || (b == 0 && !collapse_root)) {  // the root as drawn when it is not collapsed
            c |= 0x80;
            continue;
        }
        c |= static_cast<uint8_t>(1u << (g_rig.side[b] >= 0 ? g_rig.side[b] : 2));
    }
    return c;
}
uint8_t tri_class(uint8_t a, uint8_t b, uint8_t c) {  // 1 keep, 2 drop, 3 any
    if ((a | b | c) & 0x80) return 1;
    const uint8_t m = (a | b | c) & 7;
    return (m & (m - 1)) ? 2 : 3;  // two or more targets
}
// Classifies one reading; returns false for an invalid bone. cls[] gets each triangle's class.
bool classify_tris(const uint8_t* vd, uint32_t stride, uint32_t vcount, int offw, int offi, const uint16_t* ix, uint32_t ntri, bool via_palette,
                   const uint16_t* pal, int palcount, int show, bool collapse_root, uint8_t* cls, uint8_t* vc, uint16_t* w255) {
    *w255 = 0;
    for (uint32_t v = 0; v < vcount; ++v) {
        uint16_t sum = 0;
        vc[v] = vtx_class(vd + static_cast<size_t>(v) * stride + offw, vd + static_cast<size_t>(v) * stride + offi, via_palette, pal, palcount, show,
                          collapse_root, &sum);
        if (vc[v] == 0xff) return false;
        if (sum == 255) ++*w255;
    }
    for (uint32_t t = 0; t < ntri; ++t) cls[t] = tri_class(vc[ix[t * 3]], vc[ix[t * 3 + 1]], vc[ix[t * 3 + 2]]);
    return true;
}
// run 10 item 9b's probe ("skel belt scan"): one geometry's vertices summed by bone (the blend indices index the set
// directly), with the bind-space box and centroid of its positions (channel 0, float3). The command thread only.
struct GeomScan {
    uint16_t vcount = 0;
    uint32_t icount = 0;
    float w[256] = {};  // the summed weight per bone (0..1 per vertex)
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, c[3] = {};
    int why = 0;
    // run 12 item 1: per bone, the vertices whose heaviest weight is on it (the pieces on their own bones) and their box
    uint16_t nmain[256] = {};
    float mlo[256][3], mhi[256][3];
};
bool scan_geom_seh(uintptr_t geom, GeomScan* o) {
    __try {
        uintptr_t vb = 0, ib = 0, vdata = 0, fvf = 0;
        uint32_t stride = 0, mask = 0;
        uint8_t ordered = 0;
        uint64_t fmt = 0;
        std::memcpy(&vb, reinterpret_cast<const void*>(geom + 0x18), 8);
        std::memcpy(&ib, reinterpret_cast<const void*>(geom + 0x38), 8);
        if (!vb || !ib) return o->why = 1, false;
        std::memcpy(&o->icount, reinterpret_cast<const void*>(ib + 8), 4);
        std::memcpy(&o->vcount, reinterpret_cast<const void*>(vb + 8), 2);
        std::memcpy(&stride, reinterpret_cast<const void*>(vb + 0x18), 4);
        std::memcpy(&vdata, reinterpret_cast<const void*>(vb + 0x20), 8);
        std::memcpy(&fvf, reinterpret_cast<const void*>(vb + 0x30), 8);
        if (!vdata || !fvf || !o->vcount || stride == 0 || stride > 256) return o->why = 2, false;
        std::memcpy(&mask, reinterpret_cast<const void*>(fvf), 4);
        std::memcpy(&ordered, reinterpret_cast<const void*>(fvf + 6), 1);
        std::memcpy(&fmt, reinterpret_cast<const void*>(fvf + 8), 8);
        const int offp = vtx_channel_offset(mask, ordered, fmt, 0), offw = vtx_channel_offset(mask, ordered, fmt, 1),
                  offi = vtx_channel_offset(mask, ordered, fmt, 2);
        const uint32_t f0 = static_cast<uint32_t>(fmt & 0xf);
        const uint8_t* vd = reinterpret_cast<const uint8_t*>(vdata);
        for (uint32_t v = 0; v < o->vcount; ++v) {
            const uint8_t* p = vd + static_cast<size_t>(v) * stride;
            if (offp >= 0 && f0 == 6 && offp + 12 <= static_cast<int>(stride)) {  // float3
                float x[3];
                std::memcpy(x, p + offp, 12);
                for (int k = 0; k < 3; ++k) {
                    o->lo[k] = std::fmin(o->lo[k], x[k]);
                    o->hi[k] = std::fmax(o->hi[k], x[k]);
                    o->c[k] += x[k];
                }
            }
            if (offw >= 0 && offi >= 0 && offw + 4 <= static_cast<int>(stride) && offi + 4 <= static_cast<int>(stride)) {
                int mb = -1, mw = -1;
                for (int i = 0; i < 4; ++i) {
                    o->w[p[offi + i]] += p[offw + i] / 255.0f;
                    if (p[offw + i] > mw) mw = p[offw + i], mb = p[offi + i];
                }
                if (mb >= 0 && offp >= 0 && f0 == 6 && offp + 12 <= static_cast<int>(stride)) {
                    float x[3];
                    std::memcpy(x, p + offp, 12);
                    if (!o->nmain[mb])
                        for (int k = 0; k < 3; ++k) o->mlo[mb][k] = o->mhi[mb][k] = x[k];
                    for (int k = 0; k < 3; ++k) {
                        o->mlo[mb][k] = std::fmin(o->mlo[mb][k], x[k]);
                        o->mhi[mb][k] = std::fmax(o->mhi[mb][k], x[k]);
                    }
                    if (o->nmain[mb] < 65535) ++o->nmain[mb];
                }
            }
        }
        for (float& x : o->c) x /= static_cast<float>(o->vcount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o->why = 9;
        return false;
    }
}
// 2026-10-13 (the user's session 08:10: a drawable freed by an outfit change, still in the scan's 120-frame window, gave
// a geometry pointer to nothing; scan_geom_seh's guard caught the faults, but the game's own crash reporter (its
// vectored handler, before any guard) opened its dialog): every pointer the scan follows checked first (VirtualQuery:
// committed, readable data, not code), and the vertices' whole range. Off the draw thread only (system calls).
bool data_readable(uintptr_t a, size_t n) {
    if (image_mem::fast_readable(a, n, image_mem::kReadData)) return true;  // readable data, not code (readable()'s note)
    MEMORY_BASIC_INFORMATION mi{};
    return a && n && image_mem::query(a, &mi, image_mem::kBody) && mi.State == MEM_COMMIT &&
           (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) && !(mi.Protect & PAGE_GUARD) &&
           a + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}
bool scan_geom_checked(uintptr_t geom, GeomScan* o) {
    uintptr_t vb = 0, ib = 0, vdata = 0, fvf = 0;
    uint32_t stride = 0;
    uint16_t vcount = 0;
    if (!data_readable(geom, 0x90)) return o->why = 10, false;
    std::memcpy(&vb, reinterpret_cast<const void*>(geom + 0x18), 8);
    std::memcpy(&ib, reinterpret_cast<const void*>(geom + 0x38), 8);
    if (!data_readable(vb, 0x38) || !data_readable(ib, 0x10)) return o->why = 11, false;
    std::memcpy(&vcount, reinterpret_cast<const void*>(vb + 8), 2);
    std::memcpy(&stride, reinterpret_cast<const void*>(vb + 0x18), 4);
    std::memcpy(&vdata, reinterpret_cast<const void*>(vb + 0x20), 8);
    std::memcpy(&fvf, reinterpret_cast<const void*>(vb + 0x30), 8);
    if (!vcount || !stride || stride > 256 || !data_readable(fvf, 16) || !data_readable(vdata, static_cast<size_t>(vcount) * stride))
        return o->why = 12, false;
    return scan_geom_seh(geom, o);
}
std::string bone_name(uintptr_t data, int i) {  // data: the skeleton data (Rig::data), copied under g_draw_mutex
    uintptr_t bones = 0, p = 0;
    char s[40] = {};
    if (!data || !rd(data, &bones) || !bones || !rd(bones + static_cast<uintptr_t>(i) * 0x110, &p) || !p) return "?";
    for (int k = 0; k < 39; ++k)
        if (!rd(p + k, &s[k]) || !s[k]) break;
    return s;
}


bool build_cull_seh(uintptr_t geom, int show, bool collapse_root, CullEntry* e, uint8_t* cls_a, uint8_t* cls_b, uint8_t* vc, uint16_t* ix) {
    __try {
        uint8_t prim = 0, dbl = 0;
        uint64_t offbuf = 0;
        uint32_t gcount = 0, gprims = 0;
        uintptr_t vb = 0, ib = 0, vdata = 0, idata = 0, fvf = 0, palp = 0;
        uint16_t vcount = 0, palcount = 0;
        uint32_t stride = 0, icount = 0, mask = 0;
        uint8_t ordered = 0;
        uint64_t fmt = 0;
        std::memcpy(&prim, reinterpret_cast<const void*>(geom + 0x62), 1);
        std::memcpy(&dbl, reinterpret_cast<const void*>(geom + 0x63), 1);
        std::memcpy(&offbuf, reinterpret_cast<const void*>(geom + 0x88), 8);
        std::memcpy(&gcount, reinterpret_cast<const void*>(geom + 0x58), 4);
        std::memcpy(&gprims, reinterpret_cast<const void*>(geom + 0x5c), 4);
        std::memcpy(&vb, reinterpret_cast<const void*>(geom + 0x18), 8);
        std::memcpy(&ib, reinterpret_cast<const void*>(geom + 0x38), 8);
        std::memcpy(&palp, reinterpret_cast<const void*>(geom + 0x68), 8);
        std::memcpy(&palcount, reinterpret_cast<const void*>(geom + 0x72), 2);
        e->why = prim != 3 ? 1 : dbl ? 7 : offbuf ? 8 : (!vb || !ib) ? 9 : (!palp || !palcount || palcount > 128) ? 10 : 0;
        if (e->why) return false;
        std::memcpy(&icount, reinterpret_cast<const void*>(ib + 8), 4);
        std::memcpy(&idata, reinterpret_cast<const void*>(ib + 0x10), 8);
        std::memcpy(&vcount, reinterpret_cast<const void*>(vb + 8), 2);
        std::memcpy(&stride, reinterpret_cast<const void*>(vb + 0x18), 4);
        std::memcpy(&vdata, reinterpret_cast<const void*>(vb + 0x20), 8);
        std::memcpy(&fvf, reinterpret_cast<const void*>(vb + 0x30), 8);
        e->why = (!idata || !vdata || !fvf) ? 11 : (icount == 0 || icount > 30000) ? 12 : (icount != gcount || icount != 3 * gprims) ? 13
                 : (vcount == 0 || vcount > 8192 || stride == 0 || stride > 256) ? 14 : 0;
        if (e->why) return false;
        std::memcpy(&mask, reinterpret_cast<const void*>(fvf), 4);
        std::memcpy(&ordered, reinterpret_cast<const void*>(fvf + 6), 1);
        std::memcpy(&fmt, reinterpret_cast<const void*>(fvf + 8), 8);
        const uint32_t f1 = static_cast<uint32_t>((fmt >> 4) & 0xf), f2 = static_cast<uint32_t>((fmt >> 8) & 0xf);
        const int offw = vtx_channel_offset(mask, ordered, fmt, 1), offi = vtx_channel_offset(mask, ordered, fmt, 2);
        if (offw < 0 || offi < 0 || (f1 != 8 && f1 != 9) || (f2 != 8 && f2 != 9) || offw + 4 > static_cast<int>(stride) ||
            offi + 4 > static_cast<int>(stride)) {
            e->why = 2;
            return false;
        }
        std::memcpy(ix, reinterpret_cast<const void*>(idata), static_cast<size_t>(icount) * 2);
        for (uint32_t i = 0; i < icount; ++i)
            if (ix[i] >= vcount) {
                e->why = 3;
                return false;
            }
        const uint8_t* vd = reinterpret_cast<const uint8_t*>(vdata);
        const uint16_t* pal = reinterpret_cast<const uint16_t*>(palp);
        const uint32_t ntri = icount / 3;
        uint16_t wa = 0, wb = 0;
        const bool ra = classify_tris(vd, stride, vcount, offw, offi, ix, ntri, true, pal, palcount, show, collapse_root, cls_a, vc, &wa);
        const bool rb = classify_tris(vd, stride, vcount, offw, offi, ix, ntri, false, pal, palcount, show, collapse_root, cls_b, vc, &wb);
        if (ra && rb) {
            for (uint32_t t = 0; t < ntri; ++t)
                if (cls_a[t] != cls_b[t]) {
                    e->why = 4;
                    return false;
                }
        }
        if (!ra && !rb) {
            e->why = 3;
            return false;
        }
        const uint8_t* cls = ra ? cls_a : cls_b;
        e->reading = ra && rb ? 3 : ra ? 1 : 2;
        e->w255 = ra ? wa : wb;
        e->verts = vcount;
        e->tris = static_cast<uint16_t>(ntri);
        e->keep = e->drop = e->any = 0;
        for (uint32_t t = 0; t < ntri; ++t) (cls[t] == 1 ? e->keep : cls[t] == 2 ? e->drop : e->any)++;
        e->draw.icount = icount;
        e->draw.nruns = 0;
        if (!e->keep) {
            e->kind = 1;
            return true;
        }
        if (!e->drop) {
            e->kind = 2;
            return true;
        }
        // the runs: spans between DROP triangles that hold a KEEP one
        uint32_t t = 0;
        while (t < ntri) {
            while (t < ntri && cls[t] == 2) ++t;
            const uint32_t s0 = t;
            bool k = false;
            while (t < ntri && cls[t] != 2) k |= cls[t++] == 1;
            if (k && t > s0) {
                if (e->draw.nruns >= kCullRuns) {
                    e->why = 5;
                    return false;
                }
                e->draw.runs[e->draw.nruns].start = static_cast<uint16_t>(s0 * 3);
                e->draw.runs[e->draw.nruns].count = static_cast<uint16_t>((t - s0) * 3);
                ++e->draw.nruns;
            }
        }
        e->kind = 3;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        e->why = 6;
        return false;
    }
}
// the entry for this draw: found (validated by its key), or built (at most 8 a frame on this thread), else null
const CullEntry* cull_entry(uintptr_t geom, uintptr_t ib, uint32_t icount, int show) {
    const uint32_t gen = g_rig_gen.load(std::memory_order_relaxed);
    const size_t h = ((geom >> 4) ^ (geom >> 10) ^ (geom >> 16)) & 63;
    for (size_t p = 0; p < 16; ++p) {
        CullEntry& e = t_cull[(h + p) & 63];
        if (e.geom == geom && e.ib == ib && e.icount == icount && e.show == show && e.rig_gen == gen) {
            e.used = g_body_frame;
            return &e;
        }
    }
    if (t_cull_frame != g_body_frame) {
        t_cull_frame = g_body_frame;
        t_cull_builds = 0;
    }
    if (t_cull_builds >= 8) return nullptr;
    ++t_cull_builds;
    // an empty slot of the sixteen, else the least recently used (run 3: replacing the first thrashed after a show mode
    // change, the old mode's entries evicting the new ones' at the cap of 8 builds a frame)
    size_t slot = h;
    for (size_t p = 0; p < 16; ++p) {
        const size_t q = (h + p) & 63;
        if (!t_cull[q].geom) {
            slot = q;
            break;
        }
        if (t_cull[q].used < t_cull[slot].used) slot = q;
    }
    CullEntry& e = t_cull[slot];
    e = CullEntry{};
    static thread_local uint8_t cls_a[10000], cls_b[10000], vc[8192];
    static thread_local uint16_t ix[30000];
    const bool ok = build_cull_seh(geom, show, g_frame.collapse_root, &e, cls_a, cls_b, vc, ix);
    if (!ok) e.kind = 4;
    e.geom = geom;
    e.ib = ib;
    e.icount = icount;
    e.show = static_cast<uint8_t>(show);
    e.rig_gen = gen;
    e.used = g_body_frame;
    g_cull_built.fetch_add(1, std::memory_order_relaxed);
    g_cull_kind[e.kind < 5 ? e.kind : 0].fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_cull_note_mutex);
        CullNote& n = g_cull_notes[g_cull_note_n % 64];
        n = {geom, e.show, e.kind, e.reading, e.why, e.draw.nruns, e.tris, e.keep, e.drop, e.any, e.verts, e.w255};
        ++g_cull_note_n;
    }
    return &e;
}

// [Body] HiddenGeometry=filter: the draw recorder (research\run3\deferred_draw.md). The thread running FUN_140167720
// records draw packets that RenderThread replays into the D3D12 command list, so the split is made at record time:
// while the player's mixed geometry is drawn on this thread (t_cull_draw), its one draw packet (0x2c, the whole index
// range) becomes one 0x2b packet per kept index run, the game's own packet for a draw with offsets (replayed as
// DrawIndexedInstanced(count, 1, start, 0, 0)): no new packet kind, nothing new bound, every run within the game's range.
// ---- [Holsters] InteractionHighlights=parts (run 29 item 2, research\run29\glow.md approach a; the user, 2026-10-29: "I want
// the weapon parts to look like they have a glowing texture on them"): each place round 45 lights on a gun glows on the gun's
// own surface. Record time, this thread (the gun's entity call, hk_DrawVisEntity, sets t_glow for the gun in hand, the second
// gun or the copy): after the game's own draw of the gun's geometry (hk_draw_geom), for each place lit now, a "glow on"
// marker (op 0x4a; its payload the colour times the strength and the part's index runs), the geometry drawn again through
// o_draw_geom with only the part's triangles (t_cull_draw: one 0x2b packet per run, as HiddenGeometry=filter records them),
// and a "glow off" marker. pso.h glow plays them with the glow twin of the list's pipeline, or drops them. Both markers and
// the draw are decided together: the recorder is checked first (prepass::stream_ready), and a mismatched record of the extra
// draw (another count or topology at hk_rec_draw) records nothing. Which triangles: the part's bones (kGunModels) by each
// vertex's heaviest blend index (through the geometry's palette, as the GPU skins it), else, for a place on no bone (or a
// bone with no vertex), the vertices within the place's radius of its point in the gun's own frame (bind space). Reads as
// build_cull_seh's: the sizes HiddenGeometry validates, SEH only, plain data. Nothing of it runs in another mode.
struct GlowPlace {
    int id = -1;
    float shown = 0.0f;   // the eased strength (zone_rings), 0-1
    float rgb[3] = {};    // the blend factor: linear colour x shown x PartGlowStrength
    float pt[3] = {};     // the place's point in the gun's own frame (bind space, m)
    float radius = 0.0f;  // its size (m)
    bool pt_ok = false;
    float alt[3] = {};    // (round 74) holster's fallback point (every gun's ring), the gun's frame: when pt selects nothing
    bool alt_ok = false;
};
struct GlowCtx {
    int role = -1, weapon = -1, n = 0;
    uintptr_t mset = 0;  // the gun's matrix set (rec[0x12]; 0 a rigid record's: not checked): a geometry drawn with another set is not the gun's
    bool eye = false;    // an eye's pass (DrawVisEntity pass 1 or 3)
    float R[9] = {}, o[3] = {};  // the drawn gun's frame (world; its axes as columns)
    GlowPlace pl[holster::kHlCount];
};
thread_local const GlowCtx* t_glow = nullptr;  // the gun's entity call on this thread (hk_DrawVisEntity)
thread_local bool t_glow_drawing = false;      // inside the extra draw (hk_rec_draw records nothing mismatched)
thread_local uintptr_t t_glow_rec = 0;         // the recorder its "on" marker went to (the extra draw recorded only into it)
std::atomic<bool> g_glow_off{false};           // "holster glow force off" (the session): no glow recorded
std::atomic<uint32_t> g_glow_passes{0xfeu};    // the entity passes (& 7) recorded in: all but the shadows' (pass 0)
std::atomic<uint64_t> g_glow_ctx{0}, g_glow_rec{0}, g_glow_nostream{0}, g_glow_onfail{0}, g_glow_offfail{0}, g_glow_empty{0},
    g_glow_nogeom{0}, g_glow_recmis{0}, g_glow_nopacket{0}, g_glow_sel_built{0}, g_glow_sel_over{0}, g_glow_otherset{0}, g_glow_by_pass[8] = {},
    g_glow_alt{0};  // (round 74) selections made at the fallback point (every gun's ring)
// per place, the last record's (any thread writes under g_glow_note_mx; the status and part_glow_live read)
struct GlowNote {
    uint64_t rec = 0, frame = 0;
    double ms = 0.0;
    int role = -1, weapon = -1;
    uint8_t kind = 0, why = 0, nruns = 0;  // kind 1 bones, 2 the place's point, 0 none
    uint16_t tris = 0, all = 0;
    uint64_t bones = 0;
    float lo[3] = {}, hi[3] = {};  // the selection's box in the gun's frame (m)
    float outer[3] = {};             // (round 52) its vertex furthest along +x, the gun's frame
    float corner[8][3] = {};       // and in the world, as drawn
    bool box = false;
    float shown = 0.0f;
};
std::mutex g_glow_note_mx;
GlowNote g_glow_note[holster::kHlCount];
std::atomic<double> g_glow_live_ms[holster::kHlCount];
float glow_s2l(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// The part's bones on gun `weapon` for place `id` (bits < 64), from the parts' table; 0: a place on no bone
uint64_t glow_bones(int weapon, int id) {
    const GunModel* g = gun_model(weapon);
    if (!g) return 0;
    uint64_t m = 0;
    auto add = [&](auto pick) {
        for (int i = 0; i < g->n && i < 4; ++i)
            if (pick(g->part[i]) && g->part[i].bone >= 0 && g->part[i].bone < 64) m |= 1ull << g->part[i].bone;
    };
    const bool revolver = weapon >= 4 && weapon <= 7;
    if (id == holster::kHlLever) {  // the part a flick works: the lever, else a breech's block, else the hammer (holster.cpp's order)
        for (const uint8_t drv : {static_cast<uint8_t>(kDrvLever), static_cast<uint8_t>(kDrvBreech), static_cast<uint8_t>(kDrvHammer)}) {
            add([&](const GunPart& p) { return p.drv == drv && p.deg != 0.0f; });
            if (m) break;
        }
    } else if (id == holster::kHlPart) {  // the action's part: a revolver's hammer (FanHammer), else the breech, bolt or slide
        if (revolver) add([](const GunPart& p) { return p.drv == kDrvHammer; });
        else add([](const GunPart& p) { return p.drv == kDrvBreech || p.drv == kDrvBoltLift || p.drv == kDrvBoltSlide || p.drv == kDrvPump; });
    } else if (id == holster::kHlShut) {  // (2026-10-31, R11) the part that closes: a breech's block, else the bolt, else the opening
        for (const uint8_t drv : {static_cast<uint8_t>(kDrvBreech), static_cast<uint8_t>(kDrvBoltSlide), static_cast<uint8_t>(kDrvBoltLift),
                                  static_cast<uint8_t>(kDrvOpen)}) {
            add([&](const GunPart& p) { return p.drv == drv; });
            if (m) break;
        }
    } else if (id == holster::kHlPump) {  // the Pump-action's fore-end
        if (weapon == 17) add([](const GunPart& p) { return p.drv == kDrvPump; });
    } else if (id == holster::kHlLoad || id == holster::kHlLoadSec) {  // the top-breaks' and the Double-action's cylinder
        if (weapon == 5 || weapon == 6 || weapon == 7) add([](const GunPart& p) { return p.drv == kDrvIndex; });
    }
    return m;
}

struct GlowSel {
    uintptr_t geom = 0, ib = 0;
    uint32_t icount = 0;
    int8_t id = -1;
    int16_t weapon = -1;
    uint64_t bones = 0;
    int32_t q[3] = {};  // the place's point, cm (a selection by the point is made again when it moves a centimetre)
    uint16_t qr = 0;    // its radius, mm
    uint8_t kind = 0;   // 1 by the bones, 2 by the point, 0 none (why says)
    uint8_t why = 0;    // build_cull_seh's codes; 15 no position channel, 16 no triangle selected, 17 a buffer not readable data
    uint16_t tris = 0, all = 0;
    float lo[3] = {}, hi[3] = {};
    float outer[3] = {};  // (round 52) the selected vertex furthest along +x (the gun's right: a bolt's knob), the gun's frame
    bool box = false;
    bool from_alt = false;  // (round 74) built at the place's fallback point
    uint64_t used = 0;
    CullDraw draw;
};
// (round 74, the review) the frame a place's own point last lit a geometry of the gun: its fallback only when none did
std::atomic<uint64_t> g_glow_pt_hit[holster::kHlCount];
constexpr int kGlowSels = 64, kGlowProbe = 16;  // (HiddenGeometry's: two guns and the copy, their geometries, up to seven places)
thread_local GlowSel t_glow_sel[kGlowSels];
thread_local uint64_t t_glow_sel_frame = ~0ull;
thread_local int t_glow_sel_builds = 0;

// the squared distance from p to the triangle abc's nearest point (Ericson, Real-Time Collision Detection 5.1.5)
float tri_dist2(const float* a, const float* b, const float* c, const float* p) {
    auto sub = [](const float* x, const float* y, float* o) { o[0] = x[0] - y[0], o[1] = x[1] - y[1], o[2] = x[2] - y[2]; };
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    float ab[3], ac[3], ap[3], bp[3], cp[3], q[3];
    sub(b, a, ab), sub(c, a, ac), sub(p, a, ap);
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    auto at = [&](float u, float w) {  // a + u ab + w ac, its squared distance to p
        for (int k = 0; k < 3; ++k) q[k] = a[k] + u * ab[k] + w * ac[k] - p[k];
        return dot(q, q);
    };
    if (d1 <= 0.0f && d2 <= 0.0f) return at(0.0f, 0.0f);
    sub(p, b, bp);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return at(1.0f, 0.0f);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return at(d1 / (d1 - d3), 0.0f);
    sub(p, c, cp);
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return at(0.0f, 1.0f);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return at(0.0f, d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return at(1.0f - w, w);
    }
    const float den = va + vb + vc;
    if (!(std::fabs(den) > 1e-20f)) return at(0.0f, 0.0f);  // (a degenerate triangle: its first vertex)
    return at(vb / den, vc / den);
}

// the selection (SEH: build_cull_seh's reads and limits); false with e->why set
bool build_glow_seh(uintptr_t geom, uint64_t bones, bool pt_ok, const float* pt, float radius, GlowSel* e, uint16_t* ix, uint8_t* vsel, uint8_t* tsel) {
    __try {
        uint8_t prim = 0, dbl = 0;
        uint64_t offbuf = 0;
        uint32_t gcount = 0, gprims = 0;
        uintptr_t vb = 0, ib = 0, vdata = 0, idata = 0, fvf = 0, palp = 0;
        uint16_t vcount = 0, palcount = 0;
        uint32_t stride = 0, icount = 0, mask = 0;
        uint8_t ordered = 0;
        uint64_t fmt = 0;
        std::memcpy(&prim, reinterpret_cast<const void*>(geom + 0x62), 1);
        std::memcpy(&dbl, reinterpret_cast<const void*>(geom + 0x63), 1);
        std::memcpy(&offbuf, reinterpret_cast<const void*>(geom + 0x88), 8);
        std::memcpy(&gcount, reinterpret_cast<const void*>(geom + 0x58), 4);
        std::memcpy(&gprims, reinterpret_cast<const void*>(geom + 0x5c), 4);
        std::memcpy(&vb, reinterpret_cast<const void*>(geom + 0x18), 8);
        std::memcpy(&ib, reinterpret_cast<const void*>(geom + 0x38), 8);
        std::memcpy(&palp, reinterpret_cast<const void*>(geom + 0x68), 8);
        std::memcpy(&palcount, reinterpret_cast<const void*>(geom + 0x72), 2);
        e->why = prim != 3 ? 1 : dbl ? 7 : offbuf ? 8 : (!vb || !ib) ? 9 : palcount > 128 ? 10 : 0;
        if (e->why) return false;
        std::memcpy(&icount, reinterpret_cast<const void*>(ib + 8), 4);
        std::memcpy(&idata, reinterpret_cast<const void*>(ib + 0x10), 8);
        std::memcpy(&vcount, reinterpret_cast<const void*>(vb + 8), 2);
        std::memcpy(&stride, reinterpret_cast<const void*>(vb + 0x18), 4);
        std::memcpy(&vdata, reinterpret_cast<const void*>(vb + 0x20), 8);
        std::memcpy(&fvf, reinterpret_cast<const void*>(vb + 0x30), 8);
        e->why = (!idata || !vdata || !fvf) ? 11 : (icount == 0 || icount > 30000) ? 12 : (icount != gcount || icount != 3 * gprims) ? 13
                 : (vcount == 0 || vcount > 8192 || stride == 0 || stride > 256) ? 14 : 0;
        if (e->why) return false;
        std::memcpy(&mask, reinterpret_cast<const void*>(fvf), 4);
        std::memcpy(&ordered, reinterpret_cast<const void*>(fvf + 6), 1);
        std::memcpy(&fmt, reinterpret_cast<const void*>(fvf + 8), 8);
        const uint32_t f0 = static_cast<uint32_t>(fmt & 0xf), f1 = static_cast<uint32_t>((fmt >> 4) & 0xf), f2 = static_cast<uint32_t>((fmt >> 8) & 0xf);
        const int offp = vtx_channel_offset(mask, ordered, fmt, 0), offw = vtx_channel_offset(mask, ordered, fmt, 1),
                  offi = vtx_channel_offset(mask, ordered, fmt, 2);
        const bool have_pos = offp >= 0 && (f0 == 6 || f0 == 7) && offp + 12 <= static_cast<int>(stride);
        const bool have_bones = offw >= 0 && offi >= 0 && (f1 == 8 || f1 == 9) && (f2 == 8 || f2 == 9) && offw + 4 <= static_cast<int>(stride) &&
                                offi + 4 <= static_cast<int>(stride);
        std::memcpy(ix, reinterpret_cast<const void*>(idata), static_cast<size_t>(icount) * 2);
        for (uint32_t i = 0; i < icount; ++i)
            if (ix[i] >= vcount) {
                e->why = 3;
                return false;
            }
        const uint8_t* vd = reinterpret_cast<const uint8_t*>(vdata);
        const uint16_t* pal = palp && palcount ? reinterpret_cast<const uint16_t*>(palp) : nullptr;
        const uint32_t ntri = icount / 3;
        e->all = static_cast<uint16_t>(ntri);
        auto pick = [&](bool by_bone) -> uint32_t {  // vsel per vertex, then tsel per triangle (two of its three vertices; by a point also one, or its centroid, with the edge cap); the count
            for (uint32_t v = 0; v < vcount; ++v) {
                const uint8_t* p = vd + static_cast<size_t>(v) * stride;
                vsel[v] = 0;
                if (by_bone) {
                    int mb = -1, mw = -1;
                    for (int i = 0; i < 4; ++i)
                        if (p[offw + i] > mw) mw = p[offw + i], mb = p[offi + i];
                    if (mb < 0 || mw <= 0) continue;
                    if (pal) {
                        if (mb >= palcount) continue;  // (an index past the palette: not this part's)
                        mb = pal[mb];
                    }
                    vsel[v] = mb >= 0 && mb < 64 && ((bones >> mb) & 1) ? 1 : 0;
                } else {
                    float x[3];
                    std::memcpy(x, p + offp, 12);
                    const float dx = x[0] - pt[0], dy = x[1] - pt[1], dz = x[2] - pt[2];
                    vsel[v] = dx * dx + dy * dy + dz * dz <= radius * radius ? 1 : 0;
                }
            }
            uint32_t n = 0;
            for (uint32_t t = 0; t < ntri; ++t) {
                const int in = vsel[ix[t * 3]] + vsel[ix[t * 3 + 1]] + vsel[ix[t * 3 + 2]];
                if (by_bone) {
                    tsel[t] = in >= 2 ? 1 : 0;
                } else {
                    // (r281: the long guns' and the grips' long triangles: two vertices of three inside the sphere took 8 of the
                    // Winchester's fore-end, too few to see; r282: one vertex or the centroid inside still took 21, a sliver
                    // along its underside, its strips' ends all outside 5 cm. By a point: the triangle touching the sphere
                    // (its nearest point within the radius), and no edge longer than 4 radii or 30 cm, so a fore-end strip
                    // lights whole and no one triangle reaches from the fore-end across the receiver to the stock.)
                    float v[3][3];
                    for (int c = 0; c < 3; ++c) std::memcpy(v[c], vd + static_cast<size_t>(ix[t * 3 + c]) * stride + offp, 12);
                    float edge2 = 0.0f;
                    for (int c = 0; c < 3; ++c) {
                        float e2 = 0.0f;
                        for (int k = 0; k < 3; ++k) {
                            const float dd = v[c][k] - v[(c + 1) % 3][k];
                            e2 += dd * dd;
                        }
                        edge2 = std::fmax(edge2, e2);
                    }
                    const float cap2 = std::fmax(16.0f * radius * radius, 0.09f);
                    tsel[t] = in >= 2 || (edge2 <= cap2 && (in >= 1 || tri_dist2(v[0], v[1], v[2], pt) <= radius * radius)) ? 1 : 0;  // (the old rule kept: a superset)
                }
                n += tsel[t];
            }
            return n;
        };
        uint32_t n = 0;
        e->kind = 0;
        if (bones && have_bones) {
            n = pick(true);
            if (n) e->kind = 1;
        }
        if (!n && pt_ok && have_pos && radius > 0.0f) {
            n = pick(false);
            if (n) e->kind = 2;
        }
        if (!n) {
            e->why = (bones && !have_bones) || (!bones && !have_pos) ? (have_pos ? 2 : 15) : 16;
            return false;
        }
        e->tris = static_cast<uint16_t>(n);
        e->draw.icount = icount;
        e->draw.nruns = 0;
        e->box = have_pos;
        for (int k = 0; k < 3; ++k) e->lo[k] = 1e9f, e->hi[k] = -1e9f;
        uint32_t t = 0;
        while (t < ntri) {
            while (t < ntri && !tsel[t]) ++t;
            const uint32_t s0 = t;
            while (t < ntri && tsel[t]) {
                if (have_pos)
                    for (int c = 0; c < 3; ++c) {
                        float x[3];
                        std::memcpy(x, vd + static_cast<size_t>(ix[t * 3 + c]) * stride + offp, 12);
                        if (x[0] > e->hi[0]) std::memcpy(e->outer, x, sizeof(e->outer));  // (before hi takes it)
                        for (int k = 0; k < 3; ++k) e->lo[k] = std::fmin(e->lo[k], x[k]), e->hi[k] = std::fmax(e->hi[k], x[k]);
                    }
                ++t;
            }
            if (t > s0) {
                if (e->draw.nruns >= pso::glow::kMaxRuns) {
                    e->why = 5;
                    e->kind = 0;
                    return false;
                }
                e->draw.runs[e->draw.nruns].start = static_cast<uint16_t>(s0 * 3);
                e->draw.runs[e->draw.nruns].count = static_cast<uint16_t>((t - s0) * 3);
                ++e->draw.nruns;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        e->why = 6;
        e->kind = 0;
        return false;
    }
}
// (the review: a held gun's CPU copies are read only here, never by the game's own draw, and the game's crash reporter runs before
// any SEH guard) every pointer the build follows and the ranges it reads checked first: committed, readable data (data_readable,
// as scan_geom_checked); its system calls only at a build (at most 4 a frame on a thread, cached, refusals too)
// a range the build reads: readable data by the working set's fast path, else by data_readable (a VirtualQuery, 20-45 us on the
// heap) only outside RDR.exe's image: inside it one can cost about 10 ms (image_mem.h, round 42), so a range there that the
// fast path does not take is refused (the review)
// (r277: the guns' meshes live in RDR.exe's own .data, and a gun's vertex copy spans more than the fast path's 8 pages:
// 1303 x 36 bytes, 12 pages, so it was refused. Inside the image a range is now asked about in pieces of up to 8 pages,
// each by the fast path: every page present in the working set and readable, still never a VirtualQuery there.)
bool glow_range_ok(uintptr_t a, size_t n) {
    if (!a || !n || n > (1u << 22)) return false;
    uintptr_t lo = 0, hi = 0;
    image_mem::image(&lo, &hi);
    if (lo && a < hi && a + n > lo) {  // (the review: inside the image the pieces alone, never the whole range asked twice)
        if (a < lo || a + n > hi) return false;  // (partly outside the image)
        constexpr uintptr_t kPiece = 8 * 4096;
        for (uintptr_t s = a; s < a + n;) {
            const uintptr_t e = std::min<uintptr_t>(a + n, (s & ~uintptr_t(4095)) + kPiece);
            if (!image_mem::fast_readable(s, e - s, image_mem::kReadData)) return false;
            s = e;
        }
        return true;
    }
    return data_readable(a, n);
}
// each geometry's buffers proven once (keyed by them all, never by the place's point), refusals too
struct GlowProof {
    uintptr_t geom = 0, vb = 0, ib = 0, idata = 0, vdata = 0, fvf = 0, palp = 0;
    uint32_t icount = 0, stride = 0;
    uint16_t vcount = 0, palcount = 0;
    bool ok = false;
};
thread_local GlowProof t_glow_proof[16];
thread_local unsigned t_glow_proof_next = 0;
std::atomic<uint64_t> g_glow_proofs{0}, g_glow_proof_refused{0};
bool glow_readable(uintptr_t geom, GlowSel* e) {
    GlowProof k;
    k.geom = geom;
    // (the geometry's and its buffers' own records: the structures the game's draw of this geometry reads itself, by SEH)
    if (!raw(geom + 0x18, &k.vb, 8) || !raw(geom + 0x38, &k.ib, 8) || !raw(geom + 0x68, &k.palp, 8) || !raw(geom + 0x72, &k.palcount, 2) || !k.vb || !k.ib ||
        !raw(k.ib + 8, &k.icount, 4) || !raw(k.ib + 0x10, &k.idata, 8) || !raw(k.vb + 8, &k.vcount, 2) || !raw(k.vb + 0x18, &k.stride, 4) ||
        !raw(k.vb + 0x20, &k.vdata, 8) || !raw(k.vb + 0x30, &k.fvf, 8)) {
        e->why = 17;
        return false;
    }
    if (k.icount == 0 || k.icount > 30000 || k.vcount == 0 || k.vcount > 8192 || k.stride == 0 || k.stride > 256 || k.palcount > 128) return true;  // (the build's own refusal)
    for (const GlowProof& p : t_glow_proof)
        if (p.geom == k.geom && p.vb == k.vb && p.ib == k.ib && p.idata == k.idata && p.vdata == k.vdata && p.fvf == k.fvf && p.palp == k.palp &&
            p.icount == k.icount && p.stride == k.stride && p.vcount == k.vcount && p.palcount == k.palcount) {
            e->why = p.ok ? 0 : 17;
            return p.ok;
        }
    // the CPU copies the build walks (never read by the game's own draw of a gun): each range proven
    const bool f_ok = glow_range_ok(k.fvf, 16), i_ok = glow_range_ok(k.idata, static_cast<size_t>(k.icount) * 2),
               v_ok = glow_range_ok(k.vdata, static_cast<size_t>(k.vcount) * k.stride),
               p_ok = !k.palp || !k.palcount || glow_range_ok(k.palp, static_cast<size_t>(k.palcount) * 2);
    k.ok = f_ok && i_ok && v_ok && p_ok;
    // (the review: a refusal is not kept, so a gun whose copies were trimmed from the working set is proven again at a later
    // build, at most 4 a frame on a thread, instead of staying dark while its refusal is in the ring)
    if (k.ok) t_glow_proof[t_glow_proof_next++ % 16] = k;
    g_glow_proofs.fetch_add(1, std::memory_order_relaxed);
    if (!k.ok && g_glow_proof_refused.fetch_add(1, std::memory_order_relaxed) < 12) {  // (r276: 5 of 7 refused, the guns': which range)
        uintptr_t lo = 0, hi = 0;
        image_mem::image(&lo, &hi);
        log::info("[body] part glow: geometry %p's buffers refused: fvf %p %d, indices %p x%u %d, vertices %p x%u stride %u %d, palette %p x%u %d "
                  "(the image %p-%p)", reinterpret_cast<void*>(geom), reinterpret_cast<void*>(k.fvf), f_ok, reinterpret_cast<void*>(k.idata), k.icount, i_ok,
                  reinterpret_cast<void*>(k.vdata), k.vcount, k.stride, v_ok, reinterpret_cast<void*>(k.palp), k.palcount, p_ok, reinterpret_cast<void*>(lo),
                  reinterpret_cast<void*>(hi));
    }
    e->why = k.ok ? 0 : 17;
    return k.ok;
}
// this thread's selection for (geometry, place), found or built (at most 4 builds a frame on a thread); never null (kind 0: none)
const GlowSel* glow_sel(uintptr_t geom, uintptr_t ib, uint32_t icount, int weapon, const GlowPlace& p) {
    const uint64_t bones = glow_bones(weapon, p.id);
    int32_t q[3] = {};
    for (int k = 0; k < 3; ++k) q[k] = p.pt_ok ? static_cast<int32_t>(std::lround(p.pt[k] * 100.0f)) : 0;
    const uint16_t qr = static_cast<uint16_t>(std::lround(std::fmin(0.2f, std::fmax(0.0f, p.radius)) * 1000.0f));
    const size_t h = ((geom >> 4) ^ (geom >> 12) ^ static_cast<size_t>(p.id) * 7) & (kGlowSels - 1);
    GlowSel* near_e = nullptr;  // (the same geometry and place, its point elsewhere: kept when the frame's builds are spent)
    for (size_t k = 0; k < kGlowProbe; ++k) {
        GlowSel& e = t_glow_sel[(h + k) & (kGlowSels - 1)];
        if (!(e.geom == geom && e.ib == ib && e.icount == icount && e.id == p.id && e.weapon == weapon && e.bones == bones)) continue;
        // (a selection by the bones does not depend on the point: kept while the point moves; one by the point is kept while
        // the point stays within 2 cm of it, so the patch does not follow every hand's tremble, the review)
        if (e.kind == 1 || (std::abs(e.q[0] - q[0]) <= 2 && std::abs(e.q[1] - q[1]) <= 2 && std::abs(e.q[2] - q[2]) <= 2 && e.qr == qr)) {
            e.used = g_body_frame;
            if (p.id >= 0 && p.id < holster::kHlCount) {
                if (e.kind && !e.from_alt) g_glow_pt_hit[p.id].store(g_body_frame, std::memory_order_relaxed);
                if (e.from_alt && g_body_frame - g_glow_pt_hit[p.id].load(std::memory_order_relaxed) <= 2) {  // (its own point lights another)
                    static thread_local GlowSel none_alt;
                    none_alt = GlowSel{};
                    return &none_alt;
                }
            }
            return &e;
        }
        near_e = &e;
    }
    if (t_glow_sel_frame != g_body_frame) {
        t_glow_sel_frame = g_body_frame;
        t_glow_sel_builds = 0;
    }
    static thread_local GlowSel none;
    if (t_glow_sel_builds >= 4) {
        g_glow_sel_over.fetch_add(1, std::memory_order_relaxed);
        if (near_e) return near_e;
        none = GlowSel{};
        return &none;
    }
    ++t_glow_sel_builds;
    size_t slot = near_e ? static_cast<size_t>(near_e - t_glow_sel) : h;  // (its older point's entry replaced)
    for (size_t k = 0; k < kGlowProbe && !near_e; ++k) {
        const size_t s = (h + k) & (kGlowSels - 1);
        if (!t_glow_sel[s].geom) {
            slot = s;
            break;
        }
        if (t_glow_sel[s].used < t_glow_sel[slot].used) slot = s;
    }
    GlowSel& e = t_glow_sel[slot];
    e = GlowSel{};
    static thread_local uint16_t ix[30000];
    static thread_local uint8_t vsel[8192], tsel[10000];
    if (!glow_readable(geom, &e) || !build_glow_seh(geom, bones, p.pt_ok, p.pt, std::fmax(0.03f, p.radius), &e, ix, vsel, tsel)) {
        e.kind = 0;
        // (2026-11-03, round 74, the user: the Henry's loading point never glows; their own ring 3.5 cm right and 10 cm out:
        // no triangle within its 4 cm, cycle N34) its point on none of this geometry: every gun's ring's point instead
        if (!bones && e.why == 16 && p.alt_ok && p.id >= 0 && p.id < holster::kHlCount &&
            g_body_frame - g_glow_pt_hit[p.id].load(std::memory_order_relaxed) > 2) {
            e = GlowSel{};
            if (!glow_readable(geom, &e) || !build_glow_seh(geom, 0, true, p.alt, std::fmax(0.03f, p.radius), &e, ix, vsel, tsel)) e.kind = 0;
            else g_glow_alt.fetch_add(1, std::memory_order_relaxed);
            e.from_alt = true;
        }
    } else if (p.id >= 0 && p.id < holster::kHlCount && e.kind) {
        g_glow_pt_hit[p.id].store(g_body_frame, std::memory_order_relaxed);
    }
    e.geom = geom;
    e.ib = ib;
    e.icount = icount;
    e.id = static_cast<int8_t>(p.id);
    e.weapon = static_cast<int16_t>(weapon);
    e.bones = bones;
    std::memcpy(e.q, q, sizeof(q));
    e.qr = qr;
    e.used = g_body_frame;
    g_glow_sel_built.fetch_add(1, std::memory_order_relaxed);
    return &e;
}

// The gun's entity call (role 0 the gun in hand, 1 the second gun or the copy; its frame as drawn, m/rec/po/rows as
// drawn_gun_stage takes them): the places lit on it now. False: none (another mode, forced off, the shadows' pass, none lit)
thread_local uint64_t t_glow_hl_frame = ~0ull;
thread_local bool t_glow_hl_ok = false;
thread_local holster::Highlight t_glow_hl[holster::kHlCount];
thread_local float t_glow_hl_cam[16];
bool glow_ctx(int role, int weapon, uintptr_t mset, const float* m, const float* rec, const float* po, const float* rows, GlowCtx* c) {
    if (role < 0 || holster::interaction_highlights() != 3 || g_glow_off.load(std::memory_order_relaxed) || !o_draw_geom ||
        !((g_glow_passes.load(std::memory_order_relaxed) >> (t_draw_pass & 7)) & 1))
        return false;
    if (t_glow_hl_frame != g_body_frame) {  // (once a body frame on this thread: zone_rings' mutex)
        t_glow_hl_frame = g_body_frame;
        t_glow_hl_ok = zone_rings::highlights_shown(t_glow_hl, t_glow_hl_cam);
    }
    if (!t_glow_hl_ok) return false;
    DrawnGun dg;
    drawn_gun_stage(dg, 0, m, rec, po, rows);
    c->role = role;
    c->weapon = weapon;
    c->mset = mset;
    c->eye = (t_draw_pass & 7) == 1 || (t_draw_pass & 7) == 3;
    std::memcpy(c->R, dg.R[0], sizeof(c->R));
    std::memcpy(c->o, dg.o[0], sizeof(c->o));
    for (float x : c->R)
        if (!std::isfinite(x)) return false;
    float base[3];
    holster::part_glow_color(base);
    const float gain = holster::part_glow_strength();
    c->n = 0;
    for (int id = 0; id < holster::kHlCount; ++id) {
        const holster::Highlight& h = t_glow_hl[id];
        if (holster::highlight_gun(id) != role || !(h.target >= 0.02f) || !std::isfinite(h.target)) continue;
        GlowPlace& p = c->pl[c->n];
        p.id = id;
        p.shown = std::fmin(1.0f, h.target);
        // (2026-11-01, GOAL-offhand-changes C39; round 66, the user: the glowing parts should not turn white as the hand comes
        // close) the user's colour at every strength (round 45's look went whiter toward reach), brighter as it nears
        for (int k = 0; k < 3; ++k) p.rgb[k] = glow_s2l(base[k]) * p.shown * gain;
        const float d[3] = {h.pos[0] - c->o[0], h.pos[1] - c->o[1], h.pos[2] - c->o[2]};
        for (int j = 0; j < 3; ++j) p.pt[j] = c->R[0 * 3 + j] * d[0] + c->R[1 * 3 + j] * d[1] + c->R[2 * 3 + j] * d[2];
        // (r283: the foregrip's 5 cm round the front hand's grab point, under the wood, touched only the fore-end's lowest
        // strips: one sliver lit. Its glow takes 9 cm, the fore-end round, the sprite's size unchanged.)
        p.radius = id == holster::kHlFore ? std::fmax(h.radius, 0.09f) : h.radius;
        p.pt_ok = std::isfinite(p.pt[0]) && std::isfinite(p.pt[1]) && std::isfinite(p.pt[2]) && std::fabs(p.pt[0]) + std::fabs(p.pt[1]) + std::fabs(p.pt[2]) < 3.0f;
        p.alt_ok = false;
        if (h.alt_ok) {
            const float da[3] = {h.alt[0] - c->o[0], h.alt[1] - c->o[1], h.alt[2] - c->o[2]};
            for (int j = 0; j < 3; ++j) p.alt[j] = c->R[0 * 3 + j] * da[0] + c->R[1 * 3 + j] * da[1] + c->R[2 * 3 + j] * da[2];
            p.alt_ok = std::isfinite(p.alt[0] + p.alt[1] + p.alt[2]) && std::fabs(p.alt[0]) + std::fabs(p.alt[1]) + std::fabs(p.alt[2]) < 3.0f;
        }
        ++c->n;
    }
    if (!c->n) return false;
    g_glow_ctx.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// after the game's own draw of a geometry in the gun's entity call (hk_draw_geom): each lit place's glow draw
void glow_draw(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6) {
    const GlowCtx* c = t_glow;
    if (!c || !c->n || t_glow_drawing) return;
    if (c->mset && mset != c->mset) {  // (another set's geometry: not the gun's, the review)
        g_glow_otherset.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uintptr_t geoms = 0, geom = 0, ib = 0;
    uint32_t icount = 0;
    if (!model || !raw(model + 8, &geoms, sizeof(geoms)) || !geoms || !raw(geoms + (gi & 0xffffffffull) * 8, &geom, sizeof(geom)) || !geom ||
        !raw(geom + 0x38, &ib, sizeof(ib)) || !ib || !raw(ib + 8, &icount, sizeof(icount))) {
        g_glow_nogeom.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (int i = 0; i < c->n; ++i) {
        const GlowPlace& p = c->pl[i];
        const GlowSel* s = glow_sel(geom, ib, icount, c->weapon, p);
        GlowNote note;
        note.role = c->role;
        note.weapon = c->weapon;
        note.kind = s->kind;
        note.why = s->why;
        note.nruns = s->draw.nruns;
        note.tris = s->tris;
        note.all = s->all;
        note.bones = s->bones;
        note.shown = p.shown;
        if (!s->kind || !s->draw.nruns) {
            g_glow_empty.fetch_add(1, std::memory_order_relaxed);
            if (s->geom) {  // (a built selection's reason; the over-budget's none is not noted)
                std::lock_guard lock(g_glow_note_mx);
                GlowNote& n = g_glow_note[p.id];
                n.kind = 0, n.why = s->why, n.tris = 0, n.all = s->all, n.nruns = 0, n.bones = s->bones, n.role = c->role, n.weapon = c->weapon;
            }
            continue;
        }
        if (!prepass::stream_ready()) {  // (another recorder's thread: no marker can reach the playback, so no extra draw either)
            g_glow_nostream.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pso::glow::Payload pl;
        std::memcpy(pl.rgb, p.rgb, sizeof(pl.rgb));
        pl.nruns = s->draw.nruns;
        pl.place = static_cast<int8_t>(p.id);
        pl.eye = c->eye ? 1 : 0;
        const uintptr_t rec = prepass::stream_recorder();  // (the recorder the markers go to; checked by stream_ready just before)
        if (!rec) {
            g_glow_nostream.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (int r = 0; r < s->draw.nruns; ++r) pl.runs[r] = {s->draw.runs[r].start, s->draw.runs[r].count};
        if (!prepass::stream_marker_data(&pso::glow::on_cb, &pl, pso::glow::payload_size(pl.nruns))) {
            g_glow_onfail.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        t_cull_recorded = false;
        t_cull_draw = &s->draw;
        t_glow_rec = rec;
        t_glow_drawing = true;
        o_draw_geom(shader, model, gi, mset, a5, a6);
        t_glow_drawing = false;
        t_glow_rec = 0;
        t_cull_draw = nullptr;
        if (!t_cull_recorded) g_glow_nopacket.fetch_add(1, std::memory_order_relaxed);  // (the draw recorded no 0x2c packet here)
        if (!prepass::stream_marker(&pso::glow::off_cb)) g_glow_offfail.fetch_add(1, std::memory_order_relaxed);  // (its runs end it at playback)
        g_glow_rec.fetch_add(1, std::memory_order_relaxed);
        g_glow_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
        const double now = log::now_ms();
        g_glow_live_ms[p.id].store(now, std::memory_order_relaxed);
        note.ms = now;
        note.frame = g_body_frame;
        note.box = s->box;
        if (s->box) {
            std::memcpy(note.lo, s->lo, sizeof(note.lo));
            std::memcpy(note.hi, s->hi, sizeof(note.hi));
            std::memcpy(note.outer, s->outer, sizeof(note.outer));
            for (int k = 0; k < 8; ++k) {
                const float v[3] = {(k & 1) ? s->hi[0] : s->lo[0], (k & 2) ? s->hi[1] : s->lo[1], (k & 4) ? s->hi[2] : s->lo[2]};
                for (int r = 0; r < 3; ++r) note.corner[k][r] = c->o[r] + c->R[r * 3] * v[0] + c->R[r * 3 + 1] * v[1] + c->R[r * 3 + 2] * v[2];
            }
        }
        std::lock_guard lock(g_glow_note_mx);
        note.rec = g_glow_note[p.id].rec + 1;
        g_glow_note[p.id] = note;
    }
}
static_assert(pso::glow::kMaxRuns == kCullRuns, "a glow selection's runs are a CullDraw's");

using RecorderDraw_t = void (*)(uintptr_t rec, uint32_t topo, uint32_t count);
using RecorderDrawAt_t = void (*)(uintptr_t rec, uint32_t topo, uint32_t start, uint32_t count, uint32_t base);
RecorderDraw_t o_rec_draw = nullptr;
void hk_rec_draw(uintptr_t rec, uint32_t topo, uint32_t count) {
    const CullDraw* cd = t_cull_draw;
    if (!cd) return o_rec_draw(rec, topo, count);
    t_cull_recorded = true;
    const auto at = reinterpret_cast<RecorderDrawAt_t>(anchors::addr(anchors::Id::RecorderDrawAt));
    uintptr_t vt = 0;
    if (count != cd->icount || !cd->nruns || topo != 4 || !at || !raw(rec, &vt, sizeof(vt)) || vt != anchors::addr(anchors::Id::RecorderVtbl) ||
        (t_glow_drawing && rec != t_glow_rec)) {
        if (t_glow_drawing) {  // (or another recorder than its markers': its runs would play outside them, the review)  // ([Holsters] InteractionHighlights=parts: the part glow's extra draw: never the whole gun again)
            g_glow_recmis.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_cull_mismatch.fetch_add(1, std::memory_order_relaxed);  // another count or topology, or not the recorder: as the game draws it
        return o_rec_draw(rec, topo, count);
    }
    for (int i = 0; i < cd->nruns; ++i) at(rec, topo, cd->runs[i].start, cd->runs[i].count, 0);
    if (t_glow_drawing) return;  // (the glow's runs: not HiddenGeometry's counts)
    g_cull_filtered.fetch_add(1, std::memory_order_relaxed);
    g_cull_runs.fetch_add(cd->nruns, std::memory_order_relaxed);
    g_cull_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
}

// run 12, John's own belt's pieces moved (the user: "move the knife sheath, lasso and satchel"; research/run10/
// still-belt-mirror.md 2A): "skel geomshift": one geometry of the player's drawn with the bones its vertices use moved
// (both halves: the motion vectors), the set's bone buffers uploaded again around it (their frame stamps, mset +0x84 and
// +0xbc, reset to -1 before it and after the restore, so the next geometry uploads the true pose). A test of whether the
// game re-uploads within one set's draw.
struct GeomShift {
    std::atomic<uintptr_t> model{0};
    std::atomic<uint32_t> gi{~0u};
    std::atomic<int> nb{0};
    uint8_t bones[16] = {};
    float d[3] = {};
    std::atomic<bool> reset{true};
};
GeomShift g_gshift;
// run 12 item 5: the left holster copy: its geometry (by model and index), the bones its vertices use, the holster's own
// cluster on the pistol bone (bind: the centre's x for its own plane, the top for the collapse point); its move
struct LeftHolster {
    std::atomic<uintptr_t> model{0};
    std::atomic<uint32_t> gi{~0u};
    int nb = 0;
    uint8_t bones[16] = {};
    float cx = 0.0f, top[3] = {};
};
LeftHolster g_lh;
std::atomic<bool> g_left_holster{false};
// how the copy is made (test: "skel lefthol mode"): 0 the holster reflected across its own side plane (bind x) then
// across the body's middle (a rotation: its lean kept, its curve round the thigh the right one's); 1 across its own
// front-back plane (bind z) (its curve mirrored, its lean reversed); 2 the true mirror (det -1: its triangles' winding
// reversed, so a back-face-culled material shows its inside)
std::atomic<int> g_lh_mode{1};  // (cycle LM1's captures: mode 1 reads as a holster, straps and buckles out; 0 shows its inside face, 2 is inside out)
body::Piece g_lh_piece;  // under g_draw_mutex
std::atomic<uint64_t> g_lh_draws{0}, g_lh_scans{0};
float g_lh_delta[3] = {};  // under g_draw_mutex: the user's move of the copy (world) at its last draw
float g_lh_joint[3] = {};  // and the copy's pistol joint there (world)
float g_lh_piv[3] = {}, g_lh_R[9] = {};  // the copy's joint before its own move (world) and the pelvis's frame there (its grab)
float g_lh_tops[2][3] = {};  // the right holster's top point and the copy's (world: on both reflection planes, so mirrors)
float g_lh_plane[2][3] = {};  // the copy's mirror (world): its normal (John's right) and a point on it (the pelvis joint)
uint64_t g_lh_frame = 0;
// run 12 item 2, [Holsters] OwnBelt: the pieces' moves (under g_draw_mutex), the switch, and per piece the readback of
// the last player set's draw (slot 0): where the move put the bone's joint and where the drawn matrix has it (world)
body::Piece g_piece[body::kPieces];
std::atomic<bool> g_own_pieces{false};
// 2026-10-13 (the user: "Is there a box to tick that turns all of this off and just uses the standard belt?"): the
// mirrored left holster only with the belt's pieces on (Customize John's belt off: the game's own belt)
bool lh_active() { return g_left_holster.load(std::memory_order_relaxed) && g_own_pieces.load(std::memory_order_relaxed); }
struct PieceDiag {
    bool ok = false;
    float want[3] = {}, got[3] = {};
    float piv[3] = {};  // the joint before the move (world)
    float R[9] = {};    // the pelvis's frame then (columns the bind axes)
    uint64_t frame = 0;
    float loc[3] = {}, rel[9] = {};  // (the shadows round) the piece's joint and axes in the drawn pelvis's frame
    bool pose_ok = false;
};
PieceDiag g_piece_diag[body::kPieces];
std::atomic<uint64_t> g_piece_sets{0};
// "skel boneshift <bone name> <dx dy dz> | off" (test): bones of the player's set moved in the world for every draw
// of it (both halves), after transform_set: what hangs on John's attachment bones (the satchel, the sheath, the holster)
int g_bshift_bone[8] = {};
float g_bshift_d[8][3] = {};
std::atomic<int> g_bshift_n{0};
std::atomic<uint64_t> g_bshift_draws{0};
std::atomic<uint64_t> g_gshift_draws{0};
void piece_world(const body::Piece& p, const float R[9], float Rw[9], float tw[3]);
// the draw thread's last player set: each piece's joint as drawn before its move and after it (the set's space), per half
thread_local const float* t_pc_s = nullptr;
thread_local uint64_t t_pc_frame = ~0ull;
thread_local float t_pc_piv[2][body::kPieces][3], t_pc_want[2][body::kPieces][3];
thread_local bool t_pc_moved[body::kPieces] = {};
float collapse_scale();    // [Body]'s collapse (1e-7): a collapsed bone keeps its axes, shrunk (defined with the hide)
void rigid_wind(bool on);   // the cloth flutter held off around the rigid draws (defined with the wind hold)
uintptr_t set_copy(uintptr_t mset, uint8_t count);  // a private copy of a player set to draw from (0: layout not known)
void left_holster_draw(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6) {
    uint8_t count = 0;
    if (!mset || !raw(mset, &count, 1) || !count) return;
    const int pb = g_rig.piece[0], pv = g_rig.pelvis;
    const int nb = g_lh.nb;
    if (pb <= 0 || pv < 0 || pb >= count || pv >= count || nb <= 0 || static_cast<size_t>(pb) * 3 + 2 >= g_rig.jbind.size() ||
        static_cast<size_t>(pv) * 3 + 2 >= g_rig.jbind.size())
        return;
    // (2026-10-13: drawn from a private copy of the set, its records pointing at the copy: the player's set untouched)
    const uintptr_t cp = set_copy(mset, count);
    if (!cp) return;
    float* sm = reinterpret_cast<float*>(cp + 0xd4);
    body::Piece up, rp;
    {
        std::lock_guard lock(g_draw_mutex);
        up = g_lh_piece;
        rp = g_piece[0];
    }
    // 2026-10-13 (the user: the left ring "still moves around"): the copy is John's holster as the game hangs it,
    // mirrored, then the copy's own move: the right holster's move taken back out of it (it moved the left one too)
    const bool unmove = t_pc_s == reinterpret_cast<const float*>(mset + 0xd4) && t_pc_frame == g_body_frame && t_pc_moved[0];
    float delta0[3] = {}, joint0[3] = {}, tops0[2][3] = {}, plane0[2][3] = {}, piv0[3] = {}, Rp0[9] = {};
    for (int h = 0; h < 2; ++h) {
        float* base = sm + static_cast<size_t>(h) * count * 12;
        const float* Mv = base + pv * 12;
        float Mu[12];
        std::memcpy(Mu, base + pb * 12, sizeof(Mu));
        if (unmove) {  // the drawn point p = Rw (p0 - piv) + want: p0 = Rw^T (p - want) + piv
            float Rv[9], Rr[9], tr[3];
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) Rv[r * 3 + k] = Mv[r * 4 + k];
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                const float l = std::sqrt(Rv[k] * Rv[k] + Rv[3 + k] * Rv[3 + k] + Rv[6 + k] * Rv[6 + k]);
                if (l < 1e-6f) ok = false;
                else for (int r = 0; r < 3; ++r) Rv[r * 3 + k] /= l;
            }
            if (ok) {
                piece_world(rp, Rv, Rr, tr);
                const float* pv0 = t_pc_piv[h][0];
                const float* wn = t_pc_want[h][0];
                const float* Mp = base + pb * 12;
                for (int r = 0; r < 3; ++r) {
                    for (int k = 0; k < 3; ++k) Mu[r * 4 + k] = Rr[r] * Mp[k] + Rr[3 + r] * Mp[4 + k] + Rr[6 + r] * Mp[8 + k];
                    Mu[r * 4 + 3] = Rr[r] * (Mp[3] - wn[0]) + Rr[3 + r] * (Mp[7] - wn[1]) + Rr[6 + r] * (Mp[11] - wn[2]) + pv0[r];
                }
            }
        }
        const float* Mp = Mu;
        float n[3] = {Mv[0], Mv[4], Mv[8]}, c[3];  // the pelvis's bind x (John's right) in the set, its joint
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-6f) return;
        for (float& x : n) x /= nl;
        const float* jv = &g_rig.jbind[static_cast<size_t>(pv) * 3];
        for (int r = 0; r < 3; ++r) c[r] = Mv[r * 4] * jv[0] + Mv[r * 4 + 1] * jv[1] + Mv[r * 4 + 2] * jv[2] + Mv[r * 4 + 3];
        // M' = Sm (M Sb): Sb the holster's own plane (bind x = cx), Sm the body's middle (through c, normal n)
        float A[9], t[3];
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) A[r * 3 + k] = Mp[r * 4 + k];
            t[r] = Mp[r * 4 + 3];
        }
        float AD[9], t1[3];  // M Sb: mode 0 Sb = x about cx: [A D | A (2 cx e_x) + t]; 1 z about cz; 2 none
        const int mode = g_lh_mode.load(std::memory_order_relaxed);
        for (int r = 0; r < 3; ++r) {
            AD[r * 3] = mode == 0 ? -A[r * 3] : A[r * 3];
            AD[r * 3 + 1] = A[r * 3 + 1];
            AD[r * 3 + 2] = mode == 1 ? -A[r * 3 + 2] : A[r * 3 + 2];
            t1[r] = (mode == 0 ? A[r * 3] * 2.0f * g_lh.cx : mode == 1 ? A[r * 3 + 2] * 2.0f * g_lh.top[2] : 0.0f) + t[r];
        }
        float Sm[9];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) Sm[r * 3 + k] = (r == k ? 1.0f : 0.0f) - 2.0f * n[r] * n[k];
        const float cn = 2.0f * (c[0] * n[0] + c[1] * n[1] + c[2] * n[2]);
        float A2[9], t2[3];
        mul3(Sm, AD, A2);
        for (int r = 0; r < 3; ++r) t2[r] = Sm[r * 3] * t1[0] + Sm[r * 3 + 1] * t1[1] + Sm[r * 3 + 2] * t1[2] + cn * n[r];
        // the user's move of the copy about its joint, in the pelvis's frame (its columns made unit)
        const float* jp = &g_rig.jbind[static_cast<size_t>(pb) * 3];
        float jq[3], R[9], Rw[9], tw[3];
        for (int r = 0; r < 3; ++r) jq[r] = A2[r * 3] * jp[0] + A2[r * 3 + 1] * jp[1] + A2[r * 3 + 2] * jp[2] + t2[r];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) R[r * 3 + k] = Mv[r * 4 + k];
        for (int k = 0; k < 3; ++k) {
            const float l = std::sqrt(R[k] * R[k] + R[3 + k] * R[3 + k] + R[6 + k] * R[6 + k]);
            if (l < 1e-6f) return;
            for (int r = 0; r < 3; ++r) R[r * 3 + k] /= l;
        }
        piece_world(up, R, Rw, tw);
        float A3[9], t3[3];
        mul3(Rw, A2, A3);
        for (int r = 0; r < 3; ++r)
            t3[r] = Rw[r * 3] * (t2[0] - jq[0]) + Rw[r * 3 + 1] * (t2[1] - jq[1]) + Rw[r * 3 + 2] * (t2[2] - jq[2]) + jq[r] + tw[r];
        if (h == 0) {
            std::memcpy(delta0, tw, sizeof(delta0));
            float po[3] = {};
            raw(mset + 0x10, po, sizeof(po));  // (the set is the world less its header offset)
            for (int r = 0; r < 3; ++r) joint0[r] = jq[r] + tw[r] + po[r];
            for (int r = 0; r < 3; ++r) piv0[r] = jq[r] + po[r];
            std::memcpy(Rp0, R, sizeof(Rp0));
            for (int r = 0; r < 3; ++r) {
                plane0[0][r] = n[r];
                plane0[1][r] = c[r] + po[r];
                tops0[0][r] = Mp[r * 4] * g_lh.top[0] + Mp[r * 4 + 1] * g_lh.top[1] + Mp[r * 4 + 2] * g_lh.top[2] + Mp[r * 4 + 3] + po[r];
                tops0[1][r] = A3[r * 3] * g_lh.top[0] + A3[r * 3 + 1] * g_lh.top[1] + A3[r * 3 + 2] * g_lh.top[2] + t3[r] + po[r];
            }
        }
        float q[3];  // the collapse point: the copy's top
        for (int r = 0; r < 3; ++r) q[r] = A3[r * 3] * g_lh.top[0] + A3[r * 3 + 1] * g_lh.top[1] + A3[r * 3 + 2] * g_lh.top[2] + t3[r];
        const float cs = collapse_scale();  // (2026-10-13: shrunk, not zeroed: a zero normal's normalisation is NaN)
        for (int i = 0; i < nb && i < 16; ++i) {
            const int b = g_lh.bones[i];
            if (b >= count) continue;
            float* m = base + b * 12;
            for (int r = 0; r < 3; ++r) {
                for (int k = 0; k < 3; ++k) m[r * 4 + k] = b == pb ? A3[r * 3 + k] : m[r * 4 + k] * cs;
                m[r * 4 + 3] = b == pb ? t3[r] : q[r];
            }
        }
    }
    rigid_wind(true);
    o_draw_geom(shader, model, gi, cp, a5, a6);
    rigid_wind(false);
    g_lh_draws.fetch_add(1, std::memory_order_relaxed);
    if ((t_draw_pass & 7) == 1) {  // (an eye's pass)
        std::lock_guard lock(g_draw_mutex);
        std::memcpy(g_lh_delta, delta0, sizeof(g_lh_delta));
        std::memcpy(g_lh_joint, joint0, sizeof(g_lh_joint));
        std::memcpy(g_lh_piv, piv0, sizeof(g_lh_piv));
        std::memcpy(g_lh_R, Rp0, sizeof(g_lh_R));
        std::memcpy(g_lh_tops, tops0, sizeof(g_lh_tops));
        std::memcpy(g_lh_plane, plane0, sizeof(g_lh_plane));
        g_lh_frame = g_body_frame;
    }
}
bool geom_shift_draw(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6) {
    const uintptr_t want = g_gshift.model.load(std::memory_order_acquire);
    if (!want || want != model || static_cast<uint32_t>(gi & 0xffffffffull) != g_gshift.gi.load(std::memory_order_relaxed) || !mset) return false;
    uint8_t count = 0;
    if (!raw(mset, &count, 1) || !count) return false;
    float* sm = reinterpret_cast<float*>(mset + 0xd4);
    const int nb = g_gshift.nb.load(std::memory_order_relaxed);
    const bool reset = g_gshift.reset.load(std::memory_order_relaxed);
    float save[16][2][3];
    for (int i = 0; i < nb && i < 16; ++i) {
        const int b = g_gshift.bones[i];
        if (b >= count) continue;
        for (int h = 0; h < 2; ++h) {
            float* m = sm + (static_cast<size_t>(h) * count + b) * 12;
            for (int k = 0; k < 3; ++k) {
                save[i][h][k] = m[k * 4 + 3];
                m[k * 4 + 3] += g_gshift.d[k];
            }
        }
    }
    int32_t* st0 = reinterpret_cast<int32_t*>(mset + 0x84);
    int32_t* st1 = reinterpret_cast<int32_t*>(mset + 0xbc);
    if (reset) *st0 = *st1 = -1;
    o_draw_geom(shader, model, gi, mset, a5, a6);
    for (int i = 0; i < nb && i < 16; ++i) {
        const int b = g_gshift.bones[i];
        if (b >= count) continue;
        for (int h = 0; h < 2; ++h) {
            float* m = sm + (static_cast<size_t>(h) * count + b) * 12;
            for (int k = 0; k < 3; ++k) m[k * 4 + 3] = save[i][h][k];
        }
    }
    if (reset) *st0 = *st1 = -1;
    g_gshift_draws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// 2026-10-13 (the user's round 24: "Moving right holster and satchel results in the parts of the model connected to it
// being stretched. Knife sheath disappears when moving knife"): a piece moved by its bone moves its own vertices, but a
// vertex blended between it and another bone (the holster's leather into the belt's ring, the satchel's strap, the
// sheath into the boots) stretches between the two places, and a turn near 180 deg blended with an unturned bone folds
// it to nothing (cycles ST1, SH1: the holster at the user's move a sheet from the hip, the sheath gone). [Holsters]
// OwnBeltRigid: each geometry a moved piece's vertices are in is drawn twice: a copy with every bone of it but the
// piece's (its subtree) collapsed onto the piece's moved joint, so only the piece is drawn, rigid, where it was put;
// then the geometry itself with the piece's bones collapsed onto its own joint before the move, so the rest stays as
// it was and nothing is drawn between the two (blended vertices shrink toward the joint they hang from). Both halves
// (the motion vectors). The left holster's way (left_holster_draw).
struct PieceGeom {
    std::atomic<uintptr_t> model{0};
    std::atomic<uint32_t> gi{~0u};
    int piece = -1, nb = 0;
    uint8_t bones[48] = {};
};
PieceGeom g_pgeom[8];
std::atomic<int> g_pgeom_n{0};
std::atomic<bool> g_rigid_pieces{true};  // (2026-10-13: on again, the set copy's fix: the user's "Fixed")
std::atomic<uint64_t> g_rigid_draws{0}, g_pgeom_scans{0};
// ("skel pieces": the shadows round) each moved piece's geometry drawn, by the entity call's pass (t_draw_pass & 7): [0]
// drawn rigid, [1] the set not transformed for it (the game's own bones: the piece where the game has it), [2] a show mode's,
// [3] outside every entity call, drawn with the pieces moved on the game's own bones (piece_outside_draw)
std::atomic<uint64_t> g_pgeom_pass[8][4];
// (the shadows round, r239: outdoors pass 1 drew each moved piece once more with the game's own bones) the untransformed
// draws': inside an entity call or not, the player's last set or another, and their callers (up to four, RDR.exe RVAs)
std::atomic<uintptr_t> g_last_player_set{0};  // the last player set drawn ("skel setdump": its header, a test's)
// (2026-11-03, the review) that set's frame and bone count: the sun's shadow takes a set at that address as the player's only
// in that frame or the next with the same count (the sets are rebuilt every frame: another's may take the address)
std::atomic<uint64_t> g_last_player_set_frame{0};
std::atomic<uint32_t> g_last_player_set_count{0};
thread_local int t_dve_live = 0;  // hk_DrawVisEntity calls live on this thread
std::atomic<uint64_t> g_pu_in[2], g_pu_last[2], g_pu_nopc, g_pu_stale;
std::atomic<uintptr_t> g_pu_ra[4];
std::atomic<uint64_t> g_pu_ra_n[4];
void pu_note(uintptr_t mset, uintptr_t ra) {
    g_pu_in[t_dve_live > 0 ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    g_pu_last[mset == g_last_player_set.load(std::memory_order_relaxed) ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    if (!t_pc_s) g_pu_nopc.fetch_add(1, std::memory_order_relaxed);
    else if (t_pc_s == reinterpret_cast<const float*>(mset + 0xd4)) g_pu_stale.fetch_add(1, std::memory_order_relaxed);  // (this set, an older frame's)
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t r = ra - base;
    for (int i = 0; i < 4; ++i) {
        uintptr_t cur = g_pu_ra[i].load(std::memory_order_relaxed);
        if (!cur && g_pu_ra[i].compare_exchange_strong(cur, r)) cur = r;
        if (cur == r) {
            g_pu_ra_n[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
bool in_piece_set(int piece, int b) {
    for (int x : g_rig.piece_set[piece])
        if (x == b) return true;
    return false;
}
// 2026-10-13 (the user's screenshots: inside John's outline the scene drawn again, shifted: his motion vectors wrong):
// the draws here reset the player's own set's palette records (+0x84, +0xbc) to upload again, and the geometries drawn
// after them took palettes that were not their frame's. Now every draw here is made from a private copy of the set
// (its header, its records and both halves of its matrices), its records marked to upload: the player's set is never
// changed, so his other geometries draw as the game made them.
alignas(16) thread_local uint8_t t_set_copy[0xd4 + 256 * 96];
// The set's two palette records (cycle SD1, "skel setdump"): +0x60 the current half's matrices (the set's +0xd4), +0x68
// the set's +0x40, +0x84 the frame of the last upload, +0x88 its GPU address; +0x98 the previous half's (+0xd4 + count
// x 48), +0xa0 +0x40, +0xbc, +0xc0. An upload reads the matrices through the record's pointer: the copy's records point
// at the copy.
uintptr_t set_copy(uintptr_t mset, uint8_t count) {
    std::memcpy(t_set_copy, reinterpret_cast<const void*>(mset), 0xd4 + static_cast<size_t>(count) * 96);
    const uintptr_t c = reinterpret_cast<uintptr_t>(t_set_copy);
    uintptr_t cur = 0, prev = 0, hdr0 = 0, hdr1 = 0;
    std::memcpy(&cur, t_set_copy + 0x60, 8);
    std::memcpy(&hdr0, t_set_copy + 0x68, 8);
    std::memcpy(&prev, t_set_copy + 0x98, 8);
    std::memcpy(&hdr1, t_set_copy + 0xa0, 8);
    if (cur != mset + 0xd4 || prev != mset + 0xd4 + static_cast<uintptr_t>(count) * 48) return 0;  // not the layout known: not drawn from a copy
    const uintptr_t ncur = c + 0xd4, nprev = c + 0xd4 + static_cast<uintptr_t>(count) * 48;
    std::memcpy(t_set_copy + 0x60, &ncur, 8);
    std::memcpy(t_set_copy + 0x98, &nprev, 8);
    if (hdr0 == mset + 0x40) {
        const uintptr_t h = c + 0x40;
        std::memcpy(t_set_copy + 0x68, &h, 8);
    }
    if (hdr1 == mset + 0x40) {
        const uintptr_t h = c + 0x40;
        std::memcpy(t_set_copy + 0xa0, &h, 8);
    }
    *reinterpret_cast<int32_t*>(t_set_copy + 0x84) = -1;
    *reinterpret_cast<int32_t*>(t_set_copy + 0xbc) = -1;
    return c;
}
bool apply_pieces(int n, const float* s, const float* jb, std::vector<Corr>& corr, float want[body::kPieces][3], float piv_out[body::kPieces][3],
                  bool ok[body::kPieces], float R[9]);
// the rigid draws of one geometry (match: its moved pieces' g_pgeom entries): each moved piece alone at want, then the
// geometry with them collapsed onto piv (per half: [0] the current matrices, [1] the previous), from copies of the set
// (src: the matrices of both halves the copies take instead of the set's, the outside draw's corrected ones); false: not drawn
bool rigid_draws(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6, uint8_t count, const int* match, int nm,
                 const float (*want)[body::kPieces][3], const float (*piv)[body::kPieces][3], const float* src = nullptr);
std::atomic<bool> g_pieces_outside{true};  // ("skel pieces outside on|off", test: off, those draws the game's again)
std::atomic<uint32_t> g_pc_moved_mask{0};  // the moved pieces (bit per piece) as the last player set's transform_set found them
thread_local uint64_t t_dve_frame = ~0ull;  // the frame of this thread's last entity call
// ("skel pieces") as the outside draw drew each moved piece last (current half, under g_draw_mutex): its drawn joint's
// distance from where it is wanted and from the game's, its joint and axes in the drawn pelvis's frame
struct OutDiag {
    float err_mm = -1, shift_mm = -1, loc[3] = {}, rel[9] = {};
    bool pose_ok = false;
    uint64_t frame = 0;
};
OutDiag g_out_diag[body::kPieces];
std::atomic<uint64_t> g_out_thread[2];  // outside draws on a thread that has made entity calls [1], or not [0]
// the player's set's matrices while an entity call has them edited in place (set under g_draw_mutex as the edit starts,
// cleared after the restore): an outside draw of that set then is left as the game makes it (its copy would take the
// eyes' moves and move them again), counted in g_out_busy. Checked with the copy under g_draw_mutex
std::atomic<const float*> g_pset_edit{nullptr};
std::atomic<uint64_t> g_out_busy{0};
// as the outside rigid draw sent each moved piece (its alone draw's matrices, after the copy and the collapses): its joint's
// distance from where it is wanted (mm) and the frame
std::atomic<float> g_out_sent_mm[body::kPieces];
std::atomic<uint64_t> g_out_sent_frame[body::kPieces];
alignas(16) thread_local float t_out_mats[256 * 24];  // the outside draw's corrected matrices (both halves)
// piece i's joint and axes in the pelvis's frame of the matrices s (one half): loc = Rp^T (j_i - j_pelvis), rel = Rp^T Ri
bool piece_pose(const float* s, int count, int i, float loc[3], float rel[9]) {
    const int b = g_rig.piece[i], pv = g_rig.pelvis;
    if (b <= 0 || pv < 0 || b >= count || pv >= count || static_cast<size_t>(b) * 3 + 2 >= g_rig.jbind.size() ||
        static_cast<size_t>(pv) * 3 + 2 >= g_rig.jbind.size())
        return false;
    float jb[3], jp[3], Rp[9], Rb[9];
    for (int k = 0; k < 2; ++k) {
        const int x = k ? pv : b;
        const float* m = s + static_cast<size_t>(x) * 12;
        const float* j = &g_rig.jbind[static_cast<size_t>(x) * 3];
        float* o = k ? jp : jb;
        for (int r = 0; r < 3; ++r) o[r] = m[r * 4] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3];
    }
    ortho_cols(s + static_cast<size_t>(pv) * 12, Rp);
    ortho_cols(s + static_cast<size_t>(b) * 12, Rb);
    const float d[3] = {jb[0] - jp[0], jb[1] - jp[1], jb[2] - jp[2]};
    for (int c = 0; c < 3; ++c) loc[c] = Rp[c] * d[0] + Rp[3 + c] * d[1] + Rp[6 + c] * d[2];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) rel[r * 3 + c] = Rp[r] * Rb[c] + Rp[3 + r] * Rb[3 + c] + Rp[6 + r] * Rb[6 + c];
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(loc[k])) return false;
    for (int k = 0; k < 9; ++k)
        if (!std::isfinite(rel[k])) return false;
    return true;
}
// 2026-10-29 (the user: "strange shadows around John ... usually in the shape of belt items"; r239, r240): outdoors the
// sun's shadow draws the player's geometries once more outside every entity call (all from RDR.exe+0x87f61d, in the
// entity draw FUN_14087f2a0, reached without DrawVisEntity), from his set as the game left it: no transform_set, so the
// moved pieces' shadows fell where the game hangs them. Round 44 drew the moved pieces' geometries from a private copy of
// the set, each moved piece moved on the game's own bones.
// 2026-11-03 (round 73, the user: "the shadow of Johns accessories in the position John would be in if the mod wasn't
// controlling where his body faces"; three screenshots, Mexico): the rest of that shadow kept the game's facing, so the
// belt's items, off John's middle, fell beside him. Now every such draw of the player's last set is made from a private
// copy with the eyes' correction (slot 0's g_corr_world: the facing lock, the shift, the arms, the moved pieces) put into
// the copy's own offsets; the moved pieces drawn rigid as in the eyes' passes, or (OwnBeltRigid off) on their bones.
// Nothing hidden (the head, a show mode's body: their shadows kept). No correction within the last two frames (the body
// off, a cutscene): drawn as the game draws it. "skel pieces outside body off" (test): round 44's, the moved pieces only.
// False: drawn as the game draws it
bool g_corr_world_valid = false;  // (g_corr_world: slot 0 has computed it)
std::atomic<bool> g_shadow_body{true};
struct ShadowDiag {  // ("skel pieces": under g_draw_mutex) the outside draws with the eyes' correction, the last one's pelvis
    uint64_t draws = 0, stale = 0, frame = 0;
    float pelvis[3] = {}, ax[3] = {};  // its pelvis joint and first axis as drawn (world)
    float turn_deg = 0, move_mm = 0;   // that axis turned from the game's, and the joint moved from the game's
} g_shadow_diag;
struct EyePelvis {  // slot 0's pelvis as the eyes drew it (world; under g_draw_mutex), and as the game gave it (g)
    uint64_t frame = 0;
    float p[3] = {}, ax[3] = {}, g[3] = {};
} g_eye_pelvis;
float g_shadow_game_pel[3] = {};  // (under g_draw_mutex) the outside draw's pelvis as the game gave it (world)
// (2026-11-03, round 74 check 1, the user: the shadows fixed "except for the left hand holding anything but the lasso or
// foregrip of a gun"; two pictures: the hand closed on a revolver or the knife, its shadow's fingers open) the fingers' curl
// and the transplant's edits are made on the eyes' set after the correction, so the outside draw takes the hands' bones
// (each wrist's subtree) as the eyes drew them: world, slot 0's current half and slot 1's previous (under g_draw_mutex)
struct EyeHands {
    uint64_t frame = ~0ull;
    int count = 0;
    float m[2][256 * 12];
} g_eye_hands;
std::atomic<uint64_t> g_shadow_hands{0};  // outside draws given the eyes' hands
bool piece_outside_draw(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6, const int* match, int nm,
                        bool rigid) {
    uint8_t count = 0;
    if (!raw(mset, &count, 1) || !count) return false;
    const bool body_on = g_shadow_body.load(std::memory_order_relaxed);
    if (!body_on && !nm) return false;
    thread_local std::vector<Corr> corr;
    float want[2][body::kPieces][3] = {}, piv[2][body::kPieces][3] = {}, R[9];
    bool ok[2][body::kPieces] = {}, moved[body::kPieces] = {};
    float* cm = nullptr;
    uintptr_t cp = 0;
    {
        std::lock_guard lock(g_draw_mutex);  // (g_piece, g_rig: transform_set's lock; never held here, outside every entity call)
        const int n = count < g_rig.count ? count : g_rig.count;  // (the rig under the lock: an outfit's rebuild)
        if (n <= 0 || static_cast<size_t>(n) * 3 > g_rig.jbind.size()) return false;
        if (body_on && (!g_corr_world_valid || static_cast<int>(g_corr_world.size()) < n || g_corr_frame == ~0ull || g_body_frame - g_corr_frame > 2)) {
            ++g_shadow_diag.stale;
            return false;
        }
        if (g_pset_edit.load(std::memory_order_acquire) == reinterpret_cast<const float*>(mset + 0xd4)) {  // (another thread's eyes' edit)
            g_out_busy.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        cp = set_copy(mset, count);
        if (!cp) return false;
        cm = reinterpret_cast<float*>(cp + 0xd4);
        const uint32_t mk = g_pc_moved_mask.load(std::memory_order_relaxed);
        float g_pel[3] = {}, g_ax[3] = {};  // (the diagnostic: the game's pelvis, before the correction)
        const int pv = g_rig.pelvis;
        const bool pel = body_on && pv > 0 && pv < n;
        float o0[3] = {};
        std::memcpy(o0, reinterpret_cast<const void*>(cp + 0x10), sizeof(o0));
        auto pelvis_of = [&](float* p, float* ax) {
            const float* m = cm + static_cast<size_t>(pv) * 12;
            const float* j = &g_rig.jbind[static_cast<size_t>(pv) * 3];
            for (int r = 0; r < 3; ++r) {
                p[r] = m[r * 4] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3] + o0[r];
                ax[r] = m[r * 4];
            }
        };
        if (pel) {
            pelvis_of(g_pel, g_ax);
            std::memcpy(g_shadow_game_pel, g_pel, sizeof(g_pel));
        }
        for (int h = 0; h < 2; ++h) {
            float* s = cm + static_cast<size_t>(h) * count * 12;
            if (body_on) {  // the eyes' correction (the world) in this half's offset (the set is the world less it): a' = a - o + A o
                float o[3];
                std::memcpy(o, reinterpret_cast<const void*>(cp + (h ? 0x20 : 0x10)), sizeof(o));
                corr.resize(n);
                for (int b = 0; b < n; ++b) {
                    const Corr& w = g_corr_world[b];
                    Corr& c = corr[b];
                    std::memcpy(c.A, w.A, sizeof(c.A));
                    for (int k = 0; k < 3; ++k) c.a[k] = w.a[k] - o[k] + w.A[k * 3] * o[0] + w.A[k * 3 + 1] * o[1] + w.A[k * 3 + 2] * o[2];
                }
                for (int i = 0; i < body::kPieces; ++i) {  // (the moved pieces' joints as the eyes moved them, here)
                    const PieceDiag& d = g_piece_diag[i];
                    ok[h][i] = d.ok && ((mk >> i) & 1u) != 0;
                    for (int k = 0; k < 3; ++k) {
                        want[h][i][k] = d.want[k] - o[k];
                        piv[h][i][k] = d.piv[k] - o[k];
                    }
                }
            } else {
                corr.assign(n, Corr{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}});
                if (!apply_pieces(n, s, g_rig.jbind.data(), corr, want[h], piv[h], ok[h], R)) return false;
            }
            // (both ways: the moved pieces' own bones moved on the copy; rigid, the copies of rigid_draws take these)
            for (int x = 0; x < n; ++x) {  // on its bones: each bone's correction on the copy's matrix (M' = A M, t' = A t + a)
                const Corr& c = corr[x];
                float* m = s + static_cast<size_t>(x) * 12;
                float o[12];
                for (int r = 0; r < 3; ++r) {
                    for (int k = 0; k < 3; ++k) o[r * 4 + k] = c.A[r * 3] * m[k] + c.A[r * 3 + 1] * m[4 + k] + c.A[r * 3 + 2] * m[8 + k];
                    o[r * 4 + 3] = c.A[r * 3] * m[3] + c.A[r * 3 + 1] * m[7] + c.A[r * 3 + 2] * m[11] + c.a[r];
                }
                bool fin = true;
                for (float v : o) fin = fin && std::isfinite(v);
                if (fin) std::memcpy(m, o, sizeof(o));
            }
            // the hands as the eyes drew them (their fingers' curl, the transplant), into this half's offset
            if (body_on && g_eye_hands.count == count && g_eye_hands.frame == g_corr_frame) {  // (the review: the frame of g_corr_world)
                float o[3];
                std::memcpy(o, reinterpret_cast<const void*>(cp + (h ? 0x20 : 0x10)), sizeof(o));
                for (int side = 0; side < 2; ++side)
                    for (int b : g_rig.wrist_set[side]) {
                        if (b <= 0 || b >= n || b >= 256) continue;
                        const float* e = &g_eye_hands.m[h][static_cast<size_t>(b) * 12];
                        float* m = s + static_cast<size_t>(b) * 12;
                        bool fin = true;
                        for (int k = 0; k < 12; ++k) fin = fin && std::isfinite(e[k]);
                        if (!fin) continue;
                        std::memcpy(m, e, 12 * sizeof(float));
                        for (int r = 0; r < 3; ++r) m[r * 4 + 3] -= o[r];
                    }
                if (h == 0) g_shadow_hands.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (pel) {
            ShadowDiag& sd = g_shadow_diag;
            pelvis_of(sd.pelvis, sd.ax);
            const float d[3] = {sd.pelvis[0] - g_pel[0], sd.pelvis[1] - g_pel[1], sd.pelvis[2] - g_pel[2]};
            sd.move_mm = 1000.0f * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float la = std::sqrt(sd.ax[0] * sd.ax[0] + sd.ax[1] * sd.ax[1] + sd.ax[2] * sd.ax[2]),
                        lg = std::sqrt(g_ax[0] * g_ax[0] + g_ax[1] * g_ax[1] + g_ax[2] * g_ax[2]);
            const float c = la > 1e-6f && lg > 1e-6f ? (sd.ax[0] * g_ax[0] + sd.ax[1] * g_ax[1] + sd.ax[2] * g_ax[2]) / (la * lg) : 1.0f;
            sd.turn_deg = std::acos(std::fmax(-1.0f, std::fmin(1.0f, c))) * 57.29578f;
            sd.frame = g_body_frame;
            ++sd.draws;
        }
        for (int i = 0; i < body::kPieces; ++i) {
            const body::Piece& p = g_piece[i];
            moved[i] = ok[0][i] && ok[1][i] && (p.d[0] || p.d[1] || p.d[2] || p.turn[0] || p.turn[1] || p.turn[2]);
            if (!moved[i]) continue;
            OutDiag& od = g_out_diag[i];  // as drawn: the piece's joint in the corrected copy
            const int pb = g_rig.piece[i];
            const float* m = cm + static_cast<size_t>(pb) * 12;
            const float* j = &g_rig.jbind[static_cast<size_t>(pb) * 3];
            float jd[3];
            for (int r = 0; r < 3; ++r) jd[r] = m[r * 4] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3];
            od.err_mm = 1000.0f * std::sqrt((jd[0] - want[0][i][0]) * (jd[0] - want[0][i][0]) + (jd[1] - want[0][i][1]) * (jd[1] - want[0][i][1]) +
                                            (jd[2] - want[0][i][2]) * (jd[2] - want[0][i][2]));
            od.shift_mm = 1000.0f * std::sqrt((jd[0] - piv[0][i][0]) * (jd[0] - piv[0][i][0]) + (jd[1] - piv[0][i][1]) * (jd[1] - piv[0][i][1]) +
                                              (jd[2] - piv[0][i][2]) * (jd[2] - piv[0][i][2]));
            od.pose_ok = piece_pose(cm, n, i, od.loc, od.rel);
            od.frame = g_body_frame;
        }
        g_out_thread[t_dve_frame != ~0ull ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    }
    int mm[4], nmm = 0;
    for (int k = 0; k < nm; ++k)
        if (moved[g_pgeom[match[k]].piece]) mm[nmm++] = match[k];
    if (!nmm && !body_on) return false;
    for (int h = 0; h < 2; ++h)
        for (int i = 0; i < body::kPieces; ++i)
            for (int r = 0; r < 3; ++r)
                if (!std::isfinite(want[h][i][r]) || !std::isfinite(piv[h][i][r])) return false;
    if (!rigid || !nmm) {
        o_draw_geom(shader, model, gi, cp, a5, a6);
        return true;
    }
    std::memcpy(t_out_mats, cm, static_cast<size_t>(count) * 24 * sizeof(float));  // (rigid_draws' copies overwrite cm)
    return rigid_draws(shader, model, gi, mset, a5, a6, count, mm, nmm, want, piv, t_out_mats);
}
bool piece_rigid_draw(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6, uintptr_t ra) {
    if (!model || !mset) return false;
    const bool own = g_own_pieces.load(std::memory_order_relaxed);
    const bool rigid = g_rigid_pieces.load(std::memory_order_relaxed);
    const bool outside = t_dve_live == 0;  // (outside every entity call: the sun's shadow draw, piece_outside_draw)
    // (the sun's shadow of the player's last set: every geometry of it, the pieces' or not)
    bool shadow = outside && g_pieces_outside.load(std::memory_order_relaxed) && mset == g_last_player_set.load(std::memory_order_relaxed);
    if (shadow) {  // (the review) this frame's or the last's player set, its bone count the same
        uint8_t c8 = 0;
        const uint64_t sf = g_last_player_set_frame.load(std::memory_order_relaxed);
        shadow = g_body_frame - sf <= 1 && raw(mset, &c8, 1) && c8 == g_last_player_set_count.load(std::memory_order_relaxed);
    }
    if (!shadow && (!own || (!rigid && !outside))) return false;
    const int ng = own ? g_pgeom_n.load(std::memory_order_acquire) : 0;
    const uint32_t g32 = static_cast<uint32_t>(gi & 0xffffffffull);
    int match[4], nm = 0;
    for (int k = 0; k < ng && k < 8 && nm < 4; ++k) {
        const PieceGeom& e = g_pgeom[k];
        if (e.model.load(std::memory_order_acquire) == model && e.gi.load(std::memory_order_relaxed) == g32 && e.piece >= 0 &&
            e.piece < body::kPieces &&
            (outside ? ((g_pc_moved_mask.load(std::memory_order_relaxed) >> e.piece) & 1u) != 0 : t_pc_moved[e.piece]))
            match[nm++] = k;
    }
    if (!nm && !shadow) return false;
    const int ps = static_cast<int>(t_draw_pass & 7);
    if (t_cull_show) {
        if (nm) g_pgeom_pass[ps][2].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (shadow && piece_outside_draw(shader, model, gi, mset, a5, a6, match, nm, own && rigid)) {
        if (nm) g_pgeom_pass[ps][3].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!nm) return false;
    if (!rigid) {
        if (outside) {  // (left as the game draws it: counted)
            g_pgeom_pass[ps][1].fetch_add(1, std::memory_order_relaxed);
            pu_note(mset, ra);
        }
        return false;
    }
    if (outside) {  // (the outside match took every piece: from here the eyes' moved ones only, as before)
        int k2 = 0;
        for (int k = 0; k < nm; ++k)
            if (t_pc_moved[g_pgeom[match[k]].piece]) match[k2++] = match[k];
        nm = k2;
        if (!nm) return false;
    }
    if (t_pc_s != reinterpret_cast<float*>(mset + 0xd4) || t_pc_frame != g_body_frame) {
        g_pgeom_pass[ps][1].fetch_add(1, std::memory_order_relaxed);
        pu_note(mset, ra);
        return false;
    }
    uint8_t count = 0;
    if (!raw(mset, &count, 1) || !count) return false;
    if (!rigid_draws(shader, model, gi, mset, a5, a6, count, match, nm, t_pc_want, t_pc_piv)) return false;
    g_rigid_draws.fetch_add(1, std::memory_order_relaxed);
    g_pgeom_pass[ps][0].fetch_add(1, std::memory_order_relaxed);
    return true;
}
bool rigid_draws(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6, uint8_t count, const int* match, int nm,
                 const float (*want)[body::kPieces][3], const float (*piv)[body::kPieces][3], const float* src) {
    const PieceGeom& g0 = g_pgeom[match[0]];
    const int nb = g0.nb < 48 ? g0.nb : 48;
    // (the axes kept, shrunk by [Body]'s collapse, never zeroed: a zero normal's normalisation is NaN; the cloth flutter
    // held off)
    const float cs = collapse_scale();
    float* cm = nullptr;  // the copy's matrices
    auto collapse = [&](int b, int h, const float* q) {
        float* m = cm + (static_cast<size_t>(h) * count + b) * 12;
        for (int r = 0; r < 3; ++r) {
            m[r * 4] *= cs;
            m[r * 4 + 1] *= cs;
            m[r * 4 + 2] *= cs;
            m[r * 4 + 3] = q[r];
        }
    };
    rigid_wind(true);
    for (int k = 0; k < nm; ++k) {  // each moved piece alone, rigid at its move
        const int pc = g_pgeom[match[k]].piece;
        const uintptr_t cp = set_copy(mset, count);
        if (!cp) break;
        cm = reinterpret_cast<float*>(cp + 0xd4);
        if (src) std::memcpy(cm, src, static_cast<size_t>(count) * 24 * sizeof(float));
        for (int i = 0; i < nb; ++i) {
            const int b = g0.bones[i];
            if (b >= count || in_piece_set(pc, b)) continue;
            for (int h = 0; h < 2; ++h) collapse(b, h, want[h][pc]);
        }
        if (src) {  // ("skel pieces", r50 B3: the piece's joint in what is sent)
            const int pb = g_rig.piece[pc];
            if (pb > 0 && pb < count && static_cast<size_t>(pb) * 3 + 2 < g_rig.jbind.size()) {
                const float* m = cm + static_cast<size_t>(pb) * 12;
                const float* j = &g_rig.jbind[static_cast<size_t>(pb) * 3];
                float e2 = 0.0f;
                for (int r = 0; r < 3; ++r) {
                    const float v = m[r * 4] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3] - want[0][pc][r];
                    e2 += v * v;
                }
                g_out_sent_mm[pc].store(1000.0f * std::sqrt(e2), std::memory_order_relaxed);
                g_out_sent_frame[pc].store(g_body_frame, std::memory_order_relaxed);
            }
        }
        o_draw_geom(shader, model, gi, cp, a5, a6);
    }
    {  // the geometry without them
        const uintptr_t cp = set_copy(mset, count);
        if (!cp) {  // (the layout not the one known: the game's own draw)
            rigid_wind(false);
            return false;
        }
        cm = reinterpret_cast<float*>(cp + 0xd4);
        if (src) std::memcpy(cm, src, static_cast<size_t>(count) * 24 * sizeof(float));
        for (int k = 0; k < nm; ++k) {
            const int pc = g_pgeom[match[k]].piece;
            for (int i = 0; i < nb; ++i) {
                const int b = g0.bones[i];
                if (b >= count || !in_piece_set(pc, b)) continue;
                for (int h = 0; h < 2; ++h) collapse(b, h, piv[h][pc]);
            }
        }
        o_draw_geom(shader, model, gi, cp, a5, a6);
    }
    rigid_wind(false);
    return true;
}

void draw_geom_game(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6);
// ([Holsters] InteractionHighlights=parts, run 29 item 2) each geometry's draw as before, then, inside the gun's entity call
// with a place lit on it, the part's glow (glow_draw; off: one thread-local read)
void hk_draw_geom(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6) {
    draw_geom_game(shader, model, gi, mset, a5, a6);
    if (t_glow) glow_draw(shader, model, gi, mset, a5, a6);
}
void draw_geom_game(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6) {
    if (geom_shift_draw(shader, model, gi, mset, a5, a6)) return;
    if (lh_active() && model && model == g_lh.model.load(std::memory_order_acquire) &&
        static_cast<uint32_t>(gi & 0xffffffffull) == g_lh.gi.load(std::memory_order_relaxed) && !t_cull_show)
        left_holster_draw(shader, model, gi, mset, a5, a6);  // the copy first, then the holster itself as always
    if (piece_rigid_draw(shader, model, gi, mset, a5, a6, reinterpret_cast<uintptr_t>(_ReturnAddress()))) return;  // [Holsters] OwnBeltRigid (none in a show mode)
    const int show = t_cull_show;
    if (!show) return o_draw_geom(shader, model, gi, mset, a5, a6);
    // raw() (SEH only), never rd(): rd's VirtualQuery is a system call, and four of them per player geometry draw cost
    // about 22 ms a frame (run 3, research\run3\sliver.md)
    uintptr_t geoms = 0, geom = 0, pal = 0;
    uint16_t cnt = 0;
    if (!model || !raw(model + 8, &geoms, sizeof(geoms)) || !geoms || !raw(geoms + (gi & 0xffffffffull) * 8, &geom, sizeof(geom)) || !geom ||
        !raw(geom + 0x68, &pal, sizeof(pal)) || !raw(geom + 0x72, &cnt, sizeof(cnt)))
        return o_draw_geom(shader, model, gi, mset, a5, a6);
    if (g_geom_mode.load(std::memory_order_relaxed) == 2) {  // filter
        uintptr_t ib = 0;
        uint32_t icount = 0;
        const CullEntry* e = raw(geom + 0x38, &ib, sizeof(ib)) && ib && raw(ib + 8, &icount, sizeof(icount)) ? cull_entry(geom, ib, icount, show) : nullptr;
        if (e && e->kind == 1) {
            g_cull_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (e && e->kind == 3 && (g_cull_pass_mask.load(std::memory_order_relaxed) >> (t_draw_pass & 7) & 1)) {
            t_cull_recorded = false;
            t_cull_draw = &e->draw;
            o_draw_geom(shader, model, gi, mset, a5, a6);
            t_cull_draw = nullptr;
            if (!t_cull_recorded) g_cull_nodraw.fetch_add(1, std::memory_order_relaxed);  // no draw packet recorded on this thread
            return;
        }
        return o_draw_geom(shader, model, gi, mset, a5, a6);  // nothing to drop, rejected, or not built yet
    }
    GeomClass& c = t_geom_cache[(geom >> 4) & 63];
    if (c.geom != geom || c.pal != pal || c.cnt != cnt || c.show != show) {
        c.geom = geom;
        c.pal = pal;
        c.cnt = cnt;
        c.show = show;
        c.cls = classify_geom(pal, cnt, show);
    }
    g_geoms_seen[c.cls].fetch_add(1, std::memory_order_relaxed);
    if (c.cls == 2) return;  // every bone hidden: not drawn
    o_draw_geom(shader, model, gi, mset, a5, a6);
}  // seq_copy: reads that met a write, reads given up
// A read of what a sequence counter guards (odd while its writer writes; each write is a few hundred bytes): waits out
// a write in progress instead of giving up (8 quick tries could all land inside one: the aim and the held gun's
// placement then fell back for a frame, and the holsters dropped a two-handed hold), giving up only on a writer
// stalled mid-write (2048 pauses, some tens of microseconds).
bool seq_copy(const std::atomic<uint32_t>& seq, void* dst, const void* src, size_t len) {
    for (int tries = 0; tries < 2048; ++tries) {
        const uint32_t s0 = seq.load(std::memory_order_acquire);
        if (s0 & 1) {
            if (!tries) g_seq_waits.fetch_add(1, std::memory_order_relaxed);
            _mm_pause();
            continue;
        }
        std::memcpy(dst, src, len);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == s0) return true;
        if (!tries) g_seq_waits.fetch_add(1, std::memory_order_relaxed);
    }
    g_seq_misses.fetch_add(1, std::memory_order_relaxed);
    return false;
}
HandCorr g_hand_corr[2] = {};
HandCorr g_item_corr[2] = {};  // what the game holds in its hand g, as drawn (with the same counter)
body::BodyPoints g_points{};  // with the same counter: the hands' wrist targets and the holster bones, as drawn
// (2026-10-28, round 38 check 2) with the same counter: the second gun or the copy as drawn (second_drawn); ok false: none
struct SecDrawnPub {
    body::SecondDrawn d;
    bool ok = false;
} g_sec_pub;
std::atomic<uint64_t> g_sec_drawn_pubs{0}, g_sec_drawn_tw{0};
void held_delta(const HeldSample& h, int bone, Corr* w, Corr* s, const float* off = nullptr);  // (below)
// [Hands] CarryByForegrip: a put-away sample drawn by its front target's move (held_delta's first branch): already at
// this frame's target, so StaleGunFix leaves its draws alone (the review: redundant there, and wrong at its first frame)
bool put_by_target(const HeldSample& h) {
    return h.follow_j >= 0 && h.follow_j < 2 && h.pair_ok && g_frame.ik[h.follow_j] && !g_carry_olddelta.load(std::memory_order_relaxed);
}
Corr corr_rows(const float* m);
Corr corr_mul(const Corr& x, const Corr& y);
bool corr_inv(const Corr& x, Corr* o);
float g_two_diag = 0.0f;         // the two-handed turn's full angle (degrees), the latest
float g_player_offset[3] = {};   // that set's header offset (+0x10: a skinned set's matrices are relative to it)
// the first unflagged (skinned) prop's whole set, and the player set's matrix for the same bone, as drawn
float g_skinned_diag[8][12];
int g_skinned_n = 0;
float g_player_bone_diag[12];
int g_skinned_bone = -1;
float g_skinned_hdr[52];  // the set's bytes 0x00..0xd0 as floats
float g_skinned_rec[48];  // the record's first 0xc0 bytes as floats

void corr_apply(Corr& c, const float* R, const float* piv) {  // c = rot(R about piv) o c
    float nA[9];
    mul3(R, c.A, nA);
    std::memcpy(c.A, nA, sizeof(nA));
    float t[3] = {c.a[0] - piv[0], c.a[1] - piv[1], c.a[2] - piv[2]};
    for (int k = 0; k < 3; ++k) c.a[k] = R[k * 3] * t[0] + R[k * 3 + 1] * t[1] + R[k * 3 + 2] * t[2] + piv[k];
}

void corr_point(const Corr& c, const float* in, float* out) {
    for (int k = 0; k < 3; ++k) out[k] = c.A[k * 3] * in[0] + c.A[k * 3 + 1] * in[1] + c.A[k * 3 + 2] * in[2] + c.a[k];
}

// The axis-angle of a rotation matrix.
// "skel parts post" and TwoHandShotLog: the drawn gun against its first recorded frame (deg/mm over time) and the drawn
// left wrist in the drawn gun's frame (under g_draw_mutex). "The left wrist on it" is its skinning matrix's origin, the
// bind pose's origin 1.2 m from the wrist: a turn of the hand reads as a move (0.7 deg about 15 mm). Beside it
// (2026-10-10): the wrist joint itself on the gun (mm, and its turn in deg), and the front hand's snap frame of the gun
// on the drawn gun (mm, deg). And the game's own gun before any change: its object frame (the record's, which the
// samples and the snap use) on its set's root (which the mesh follows), the root on the gun hand's attachment joint,
// the object frame on that joint. The series' entries: deg/mm/left mm/clip/joint mm/joint deg/snap mm/snap deg/object
// on root mm/root on attachment mm/object on attachment mm
std::string parts_post() {
    int first = -1;
    for (int i = 0; i < g_parts_n; ++i)
        if (g_parts[i].post_ok) {
            first = i;
            break;
        }
    if (first < 0) return std::string("parts post: none");
    float worst_a = 0, worst_t = 0, worst_l = 0, worst_jm = 0, worst_jd = 0, worst_sm = 0, worst_sd = 0;
    std::string series;
    char b[480];
    float l0[3] = {}, ll[3] = {};
    float L0[12] = {}, S0[12] = {}, jp0[3] = {};
    bool have_l0 = false, have_s0 = false;
    // the game's own: the object frame on its root, the root on the gun hand's attachment, the object on the attachment
    float G0[3][12] = {};
    bool have_g0 = false;
    float worst_g[3][2] = {};
    const int ab = g_rig.att_wrist[1];
    const float* ajb = ab >= 0 && static_cast<size_t>(ab) * 3 + 2 < g_rig.jbind.size() ? &g_rig.jbind[static_cast<size_t>(ab) * 3] : nullptr;
    const int wl = g_rig.wrist[0];
    const float* jb = wl >= 0 && static_cast<size_t>(wl) * 3 + 2 < g_rig.jbind.size() ? &g_rig.jbind[static_cast<size_t>(wl) * 3] : nullptr;
    auto angle_of = [](const float* D) {
        const float Rr[9] = {D[0], D[1], D[2], D[4], D[5], D[6], D[8], D[9], D[10]};
        float ax[3], an = 0;
        to_axis_angle(Rr, ax, &an);
        return an * 57.2958f;
    };
    for (int i = first, k = 0; i < g_parts_n; ++i, ++k) {
        if (!g_parts[i].post_ok) continue;
        float D[12], ax[3], an = 0;
        rel_set(g_parts[first].post, g_parts[i].post, D);
        const float Rr[9] = {D[0], D[1], D[2], D[4], D[5], D[6], D[8], D[9], D[10]};
        to_axis_angle(Rr, ax, &an);
        const float tl = std::sqrt(D[3] * D[3] + D[7] * D[7] + D[11] * D[11]);
        worst_a = std::fmax(worst_a, an * 57.2958f);
        worst_t = std::fmax(worst_t, tl * 1000.0f);
        // the drawn left wrist in the drawn gun's frame: its move from the first frame (mm)
        float gi[12], L[12];
        float lm = 0, jm = 0, jd = 0, smm = 0, sdg = 0;
        if (inv34(g_parts[i].post, gi)) {
            mul34(gi, g_parts[i].left, L);
            if (i == first) l0[0] = L[3], l0[1] = L[7], l0[2] = L[11];
            ll[0] = L[3], ll[1] = L[7], ll[2] = L[11];
            lm = std::sqrt((L[3] - l0[0]) * (L[3] - l0[0]) + (L[7] - l0[1]) * (L[7] - l0[1]) + (L[11] - l0[2]) * (L[11] - l0[2])) * 1000.0f;
            worst_l = std::fmax(worst_l, lm);
            float jp[3] = {};
            if (jb)
                for (int r = 0; r < 3; ++r) jp[r] = L[r * 4] * jb[0] + L[r * 4 + 1] * jb[1] + L[r * 4 + 2] * jb[2] + L[r * 4 + 3];
            if (!have_l0) {
                std::memcpy(L0, L, sizeof(L0));
                std::memcpy(jp0, jp, sizeof(jp0));
                have_l0 = true;
            }
            float DL[12];
            rel_set(L0, L, DL);
            jd = angle_of(DL);
            jm = std::sqrt((jp[0] - jp0[0]) * (jp[0] - jp0[0]) + (jp[1] - jp0[1]) * (jp[1] - jp0[1]) + (jp[2] - jp0[2]) * (jp[2] - jp0[2])) * 1000.0f;
            worst_jm = std::fmax(worst_jm, jm);
            worst_jd = std::fmax(worst_jd, jd);
            if (g_parts[i].snap_ok) {
                float Sg[12], DS[12];
                mul34(gi, g_parts[i].snap, Sg);
                if (!have_s0) {
                    std::memcpy(S0, Sg, sizeof(S0));
                    have_s0 = true;
                }
                rel_set(S0, Sg, DS);
                sdg = angle_of(DS);
                smm = std::sqrt((Sg[3] - S0[3]) * (Sg[3] - S0[3]) + (Sg[7] - S0[7]) * (Sg[7] - S0[7]) + (Sg[11] - S0[11]) * (Sg[11] - S0[11])) * 1000.0f;
                worst_sm = std::fmax(worst_sm, smm);
                worst_sd = std::fmax(worst_sd, sdg);
            }
        }
        float gm[3] = {};
        if (g_parts[i].game_ok) {
            const PartsSample& q = g_parts[i];
            float A[12];  // the attachment with its origin at its joint (its skinning matrix's origin is the bind's, 1.2 m off)
            std::memcpy(A, q.att, sizeof(A));
            if (ajb)
                for (int r = 0; r < 3; ++r) A[r * 4 + 3] = q.att[r * 4] * ajb[0] + q.att[r * 4 + 1] * ajb[1] + q.att[r * 4 + 2] * ajb[2] + q.att[r * 4 + 3];
            float G[3][12];
            rel_set(q.root, q.obj, G[0]);
            rel_set(A, q.root, G[1]);
            rel_set(A, q.obj, G[2]);
            if (!have_g0) {
                std::memcpy(G0, G, sizeof(G0));
                have_g0 = true;
            }
            for (int r = 0; r < 3; ++r) {
                float DG[12];
                rel_set(G0[r], G[r], DG);
                const float* a0 = G0[r];
                const float* a1 = G[r];
                gm[r] = std::sqrt((a1[3] - a0[3]) * (a1[3] - a0[3]) + (a1[7] - a0[7]) * (a1[7] - a0[7]) + (a1[11] - a0[11]) * (a1[11] - a0[11])) * 1000.0f;
                worst_g[r][0] = std::fmax(worst_g[r][0], gm[r]);
                worst_g[r][1] = std::fmax(worst_g[r][1], angle_of(DG));
            }
        }
        if (k % 6 == 0) {
            std::snprintf(b, sizeof(b), " %.1f/%.0f/%.0f/%.1f/%.0f/%.1f/%.0f/%.1f/%.0f/%.0f/%.0f", an * 57.2958f, tl * 1000.0f, lm, g_parts[i].phase, jm,
                          jd, smm, sdg, gm[0], gm[1], gm[2]);
            series += b;
        }
    }
    std::snprintf(b, sizeof(b),
                  "parts post: worst %.2f deg %.1f mm, the left wrist on it %.1f mm (at first (%.1f %.1f %.1f) mm in the gun, at last (%.1f %.1f %.1f)); "
                  "the wrist joint on it %.1f mm %.2f deg, the snap's gun on the drawn gun %.1f mm %.2f deg; the game's gun: its object frame on "
                  "its root %.1f mm %.2f deg, its root on the hand's attachment %.1f mm %.2f deg, its object frame on the attachment %.1f mm %.2f "
                  "deg | deg/mm/left mm/clip:",
                  worst_a, worst_t, worst_l, l0[0] * 1000.0f, l0[1] * 1000.0f, l0[2] * 1000.0f, ll[0] * 1000.0f, ll[1] * 1000.0f, ll[2] * 1000.0f,
                  worst_jm, worst_jd, worst_sm, worst_sd, worst_g[0][0], worst_g[0][1], worst_g[1][0], worst_g[1][1], worst_g[2][0], worst_g[2][1]);
    return b + series;
}

void to_axis_angle(const float* R, float* axis, float* ang) {
    float c = (R[0] + R[4] + R[8] - 1) * 0.5f;
    c = c < -1 ? -1 : c > 1 ? 1 : c;
    *ang = std::acos(c);
    axis[0] = R[7] - R[5];
    axis[1] = R[2] - R[6];
    axis[2] = R[3] - R[1];
    float l = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (l < 1e-6f) {
        axis[0] = 1;
        axis[1] = axis[2] = 0;
        *ang = 0;
        return;
    }
    for (int k = 0; k < 3; ++k) axis[k] /= l;
}

// One set (current or previous), once per fill: `count` matrices (a set may have fewer than the rig's bones; nothing
// past its end is touched).
// a piece's move in the world, from its offset and turn in the pelvis's frame R (columns the bind axes: x right, y up,
// z back): the rotation Rw (about the piece's joint) and the translation tw
void piece_world(const body::Piece& p, const float R[9], float Rw[9], float tw[3]) {
    const float k = 0.01745329f, tx = p.turn[0] * k, ty = p.turn[1] * k, tz = p.turn[2] * k;
    const float Rx[9] = {1, 0, 0, 0, std::cos(tx), -std::sin(tx), 0, std::sin(tx), std::cos(tx)};
    const float Ry[9] = {std::cos(ty), 0, std::sin(ty), 0, 1, 0, -std::sin(ty), 0, std::cos(ty)};
    const float Rz[9] = {std::cos(tz), -std::sin(tz), 0, std::sin(tz), std::cos(tz), 0, 0, 0, 1};
    float t1[9], Rl[9], t2[9], Rt[9];
    mul3(Ry, Rx, t1);
    mul3(t1, Rz, Rl);  // in the bind frame: Ry(turn) Rx(tilt) Rz(lean)
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) Rt[r * 3 + c] = R[c * 3 + r];
    mul3(R, Rl, t2);
    mul3(t2, Rt, Rw);  // the same turn in the world
    const float db[3] = {p.d[0], p.d[1], -p.d[2]};  // forward is bind -z
    for (int r = 0; r < 3; ++r) tw[r] = R[r * 3] * db[0] + R[r * 3 + 1] * db[1] + R[r * 3 + 2] * db[2];
}
// the move for the set in transform_set (its frame from the drawn pelvis), for each piece: its joint as drawn (`piv`,
// before the move) and, with a move, every bone of its subtree's correction composed with it; `want` the joint's moved
// place; R the pelvis's frame. False: no pelvis
// the pelvis as drawn: bind -> world (its columns the bind axes, made unit: the set may carry a scale); false: none
bool pelvis_R_of(int n, const float* s, const std::vector<Corr>& corr, float R[9]) {
    const int pv = g_rig.pelvis;
    if (pv < 0 || pv >= n || pv >= static_cast<int>(corr.size())) return false;
    float M[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) M[r * 3 + c] = s[pv * 12 + r * 4 + c];
    mul3(corr[pv].A, M, R);
    for (int c = 0; c < 3; ++c) {
        const float l = std::sqrt(R[c] * R[c] + R[3 + c] * R[3 + c] + R[6 + c] * R[6 + c]);
        if (l < 1e-6f) return false;
        for (int r = 0; r < 3; ++r) R[r * 3 + c] /= l;
    }
    return true;
}
// 2026-10-13 (the release review: the rings left off the pieces when the belt is turned off): the pelvis's frame of
// the last player set, the belt's pieces on or off (piece_shift_world's), under g_draw_mutex
float g_pelvis_R[9] = {};
uint64_t g_pelvis_R_frame = 0;
bool g_pelvis_R_ok = false;
bool apply_pieces(int n, const float* s, const float* jb, std::vector<Corr>& corr, float want[body::kPieces][3], float piv_out[body::kPieces][3],
                  bool ok[body::kPieces], float R[9]) {
    for (int i = 0; i < body::kPieces; ++i) ok[i] = false;
    if (!pelvis_R_of(n, s, corr, R)) return false;
    for (int i = 0; i < body::kPieces; ++i) {
        const int b = g_rig.piece[i];
        const body::Piece& p = g_piece[i];
        if (b <= 0 || b >= n || g_rig.piece_set[i].empty()) continue;
        float jd[3], piv[3];  // the bone's joint as drawn (its correction), the pivot
        const float* m = s + b * 12;
        const float* j = jb + static_cast<size_t>(b) * 3;
        for (int r = 0; r < 3; ++r) jd[r] = m[r * 4] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3];
        corr_point(corr[b], jd, piv);
        std::memcpy(piv_out[i], piv, sizeof(piv));
        std::memcpy(want[i], piv, sizeof(piv));
        ok[i] = true;
        if (!(p.d[0] || p.d[1] || p.d[2] || p.turn[0] || p.turn[1] || p.turn[2])) continue;
        float Rw[9], tw[3];
        piece_world(p, R, Rw, tw);
        for (int x : g_rig.piece_set[i]) {
            if (x < 0 || x >= n) continue;
            corr_apply(corr[x], Rw, piv);
            for (int r = 0; r < 3; ++r) corr[x].a[r] += tw[r];
        }
        for (int r = 0; r < 3; ++r) want[i][r] = piv[r] + tw[r];
    }
    return true;
}
void transform_set(float* s, int count, int slot, const Frame& f, const float* off) {
    const int n = count < g_rig.count ? count : g_rig.count;
    const float* r = s;  // the root bone: never changed, the reference
    const float mr[9] = {r[0], r[1], r[2], r[4], r[5], r[6], r[8], r[9], r[10]}, tr[3] = {r[3], r[7], r[11]};
    // where joint b is drawn, in the set's space (the world less the set's header offset `off`): M_b j_b + t_b
    auto jdrawn = [&](int b, float* o) {
        const float* m = s + b * 12;
        const float* j = &g_rig.jbind[static_cast<size_t>(b) * 3];
        for (int k = 0; k < 3; ++k) o[k] = m[k * 4] * j[0] + m[k * 4 + 1] * j[1] + m[k * 4 + 2] * j[2] + m[k * 4 + 3];
    };
    g_corr.assign(n, Corr{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}});
    if (slot == 0 && g_rig.att_wrist[1] >= 0 && g_rig.att_wrist[1] < n) {  // "skel parts post": the game's gun hand attachment
        std::memcpy(g_att_game, s + g_rig.att_wrist[1] * 12, sizeof(g_att_game));
        g_att_game_ok = true;
    }
    // the knuckle check (the transplant's, after the fingers below): the game's gripping hand's knuckles as animated
    float kn_g[3] = {0, 0, 0};
    bool kn_ok = false;
    if (slot == 0 && f.xfer && f.item_side >= 0 && f.item_side < 2) {
        int c = 0;
        for (int i = 3; i < 15; i += 3) {  // finger_11, 21, 31, 41
            const int b = g_rig.finger[f.item_side][i];
            if (b < 0 || b >= n) continue;
            float p[3];
            jdrawn(b, p);
            for (int k = 0; k < 3; ++k) kn_g[k] += p[k];
            ++c;
        }
        kn_ok = c == 4;
        for (int k = 0; k < 3; ++k) kn_g[k] *= 0.25f;
    }
    if (slot == 0 && g_rig.att_wrist[1] >= 0 && g_rig.att_wrist[1] < n && g_rig.wrist[1] >= 0 && g_rig.wrist[1] < n) {
        // the shake diagnostic: the right wrist attachment (where the game hangs the gun) in the right wrist's frame,
        // as animated: its range of position (mm) and turn (deg) since the last "skel stale"
        float Rw[9], Ra[9], jw[3], ja[3], pr[3], R[9];
        ortho_cols(s + g_rig.wrist[1] * 12, Rw);
        ortho_cols(s + g_rig.att_wrist[1] * 12, Ra);
        jdrawn(g_rig.wrist[1], jw);
        jdrawn(g_rig.att_wrist[1], ja);
        for (int i = 0; i < 3; ++i) pr[i] = Rw[0 * 3 + i] * (ja[0] - jw[0]) + Rw[1 * 3 + i] * (ja[1] - jw[1]) + Rw[2 * 3 + i] * (ja[2] - jw[2]);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) R[i * 3 + j] = Rw[0 * 3 + i] * Ra[0 * 3 + j] + Rw[1 * 3 + i] * Ra[1 * 3 + j] + Rw[2 * 3 + i] * Ra[2 * 3 + j];
        float ax[3], ang = 0;
        to_axis_angle(R, ax, &ang);
        float* sd = g_shake_diag;
        for (int k = 0; k < 3; ++k) {
            sd[k] = std::fmin(sd[k], pr[k] * 1000.0f);
            sd[3 + k] = std::fmax(sd[3 + k], pr[k] * 1000.0f);
        }
        sd[6] = std::fmin(sd[6], ang * 57.2957795f);
        sd[7] = std::fmax(sd[7], ang * 57.2957795f);
    }
    float wrist_anim[2][9] = {};  // the game's wrists, before any correction (body_points)
    for (int h = 0; h < 2; ++h)
        if (slot == 0 && g_rig.wrist[h] >= 0 && g_rig.wrist[h] < n) ortho_cols(s + g_rig.wrist[h] * 12, wrist_anim[h]);
    float ro[9];
    ortho_cols(r, ro);
    // the reference for "upright": the root bone's orientation levelled (its heading kept, pitch and roll removed:
    // running pitches the root itself forward, round 6), the set's y being the world's up
    {
        float up[3] = {0, 1, 0}, z[3] = {ro[2], ro[5], ro[8]}, x[3];
        float dz = z[1];
        z[1] -= dz;
        norm(z);
        cross(up, z, x);
        norm(x);
        for (int k = 0; k < 3; ++k) {
            ro[k * 3 + 0] = x[k];
            ro[k * 3 + 1] = up[k];
            ro[k * 3 + 2] = z[k];
        }
    }
    // 1. the stance: the turn that stands a bone up (upright: its bind orientation over the root; lean: only about the
    //    root's sideways axis). The upper body (spine00's subtree) takes it plus the pitch offset, rigidly about
    //    spine00's joint; the pelvis takes its own (round 5d: with the hips in the game's running tilt and the torso
    //    upright, the stomach between them stretched), its legs keep the animation.
    float side[3] = {ro[0], ro[3], ro[6]};  // the root's x axis in the set's space
    auto stance_rot = [&](const float* u, float* rc) {
        if (f.stance == 1) {
            float uo[9], ut[9];
            ortho_cols(u, uo);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) ut[i * 3 + j] = uo[j * 3 + i];
            mul3(ro, ut, rc);
        } else {
            float up_r[3] = {ro[1], ro[4], ro[7]}, up_u[3] = {u[1], u[5], u[9]}, cr[3];
            float d = up_u[0] * side[0] + up_u[1] * side[1] + up_u[2] * side[2];
            for (int k = 0; k < 3; ++k) up_u[k] -= d * side[k];
            norm(up_u);
            cross(up_u, up_r, cr);
            float ang = std::atan2(cr[0] * side[0] + cr[1] * side[1] + cr[2] * side[2],
                                   up_u[0] * up_r[0] + up_u[1] * up_r[1] + up_u[2] * up_r[2]);
            axis_angle(side, ang, rc);
        }
    };
    // 1s. the still stance ([Body] Stance=still, 2026-10-11, the user: "a body that doesn't move at all except for the
    //     arms and legs"): every pinned bone (the torso) drawn in its bind pose under one frame F, the levelled root's
    //     axes ro with the bind origin at f.still_t (in the bind pose every skinning matrix is the root's, so each pinned
    //     bone's matrix becomes F itself); the bones below a pinned one carried with it as animated (the arms from the
    //     clavicles, then the IK; the legs from the pelvis; the attachments). Far from the animation (a ragdoll, a
    //     fall): the upright stance for that set instead.
    bool still = false;
    float still_o[3] = {0, 0, 0};  // F's origin in the set's space
    if (f.stance == 3 && f.still && g_rig.chain[0] > 0 && g_rig.chain[0] < n && static_cast<int>(g_rig.parent.size()) >= n &&
        static_cast<int>(g_rig.pin.size()) >= n) {
        const float* tw = slot ? f.still_t_prev : f.still_t;
        for (int k = 0; k < 3; ++k) still_o[k] = tw[k] - off[k];
        const float* js = &g_rig.jbind[static_cast<size_t>(g_rig.chain[0]) * 3];
        float sj[3], d2 = 0.0f;
        jdrawn(g_rig.chain[0], sj);
        for (int k = 0; k < 3; ++k) {
            const float p = ro[k * 3] * js[0] + ro[k * 3 + 1] * js[1] + ro[k * 3 + 2] * js[2] + still_o[k];
            d2 += (p - sj[k]) * (p - sj[k]);
        }
        // (run 12 item 8, cycle GF1: with the offset kept, a rear's or a spur's start lifts the riding animation over 0.5 m
        // from the pinned place and those sets took the animation's pose, a 1.3 m jump: riding with BodyWithCamera, 1.5 m)
        const float lim = f.ride_keep ? 1.5f : 0.5f;
        still = std::isfinite(d2) && d2 < lim * lim;
        if (slot == 0) (still ? g_still_sets : g_still_refused)++;
    }
    if (still) {
        std::vector<uint8_t>& carried = g_still_carried;
        carried.assign(n, 0);
        for (int b = 1; b < n; ++b) {
            if (g_rig.pin[b] || (f.still_belt && b < static_cast<int>(g_rig.belt_pin.size()) && g_rig.belt_pin[b])) {
                const float* m = s + b * 12;
                float mo[9], mt[9];
                ortho_cols(m, mo);
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) mt[i * 3 + j] = mo[j * 3 + i];
                Corr& c = g_corr[b];
                mul3(ro, mt, c.A);
                const float tb[3] = {m[3], m[7], m[11]};
                for (int k = 0; k < 3; ++k) c.a[k] = still_o[k] - (c.A[k * 3] * tb[0] + c.A[k * 3 + 1] * tb[1] + c.A[k * 3 + 2] * tb[2]);
                carried[b] = 1;
            } else {
                const int p = g_rig.parent[b];
                if (p > 0 && p < n && carried[p]) {
                    g_corr[b] = g_corr[p];
                    carried[b] = 1;
                }
            }
        }
        if (f.pitch != 0) {  // [Body] TorsoPitch on the upper body, about spine00's pinned joint
            float rp[9], piv[3];
            axis_angle(side, f.pitch, rp);
            const float* js = &g_rig.jbind[static_cast<size_t>(g_rig.chain[0]) * 3];
            for (int k = 0; k < 3; ++k) piv[k] = ro[k * 3] * js[0] + ro[k * 3 + 1] * js[1] + ro[k * 3 + 2] * js[2] + still_o[k];
            for (int b : g_rig.chain_set[0])
                if (b < n) corr_apply(g_corr[b], rp, piv);
        }
        // stage 2 ([Body] StillFeet): each leg carried with the pinned pelvis keeps the animation's own sway from the
        // hips, so standing the feet slid 25-96 mm (cycle r10d). A two-bone solve puts each foot back where the animation
        // planted it: the hip where the pinned pelvis carries it, the knee in the leg's own bend plane, the foot turned to
        // its animated orientation
        if (f.still_feet)
            for (int h = 0; h < 2; ++h) {
                const int hj = g_rig.hipj[h], kj = g_rig.knee[h], fj = g_rig.foot[h];
                if (hj < 0 || kj < 0 || fj < 0 || hj >= n || kj >= n || fj >= n || g_rig.leg_set[h][1].empty()) continue;
                float ah[3], ak[3], af[3], H[3], K0[3], F0[3];
                jdrawn(hj, ah);
                jdrawn(kj, ak);
                jdrawn(fj, af);
                corr_point(g_corr[hj], ah, H);
                corr_point(g_corr[kj], ak, K0);
                corr_point(g_corr[fj], af, F0);
                auto dist3 = [](const float* x, const float* y) {
                    return std::sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2]));
                };
                const float l1 = dist3(ak, ah), l2 = dist3(af, ak);
                float u[3] = {af[0] - H[0], af[1] - H[1], af[2] - H[2]};
                float d = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                if (!(l1 > 0.05f && l2 > 0.05f && d > 1e-3f) || d > 1.5f * (l1 + l2)) continue;  // not a leg here (a ragdoll)
                for (float& c : u) c /= d;
                d = std::clamp(d, std::fabs(l1 - l2) + 1e-3f, l1 + l2 - 1e-3f);
                // the bend plane from the carried leg: its normal, and the knee's side within it
                float a1[3] = {K0[0] - H[0], K0[1] - H[1], K0[2] - H[2]}, a2[3] = {F0[0] - H[0], F0[1] - H[1], F0[2] - H[2]}, nrm[3], v[3];
                cross(a1, a2, nrm);
                const float nl = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
                if (nl < 1e-5f) {  // a straight leg: the plane holds the body's forward
                    const float fw[3] = {-ro[2], -ro[5], -ro[8]};
                    cross(u, fw, nrm);
                }
                norm(nrm);
                cross(nrm, u, v);
                norm(v);
                if (v[0] * a1[0] + v[1] * a1[1] + v[2] * a1[2] < 0.0f)
                    for (float& c : v) c = -c;
                const float along = (l1 * l1 - l2 * l2 + d * d) / (2.0f * d);
                const float up = std::sqrt(std::fmax(0.0f, l1 * l1 - along * along));
                float K[3];
                for (int k = 0; k < 3; ++k) K[k] = H[k] + u[k] * along + v[k] * up;
                // the turn taking from -> to (unit vectors), about their common normal
                auto turn = [&](const float* from, const float* to, float* R) {
                    float f1[3] = {from[0], from[1], from[2]}, t1[3] = {to[0], to[1], to[2]}, ax[3];
                    norm(f1);
                    norm(t1);
                    cross(f1, t1, ax);
                    const float s = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                    const float c = std::clamp(f1[0] * t1[0] + f1[1] * t1[1] + f1[2] * t1[2], -1.0f, 1.0f);
                    if (s < 1e-6f) {
                        for (int k = 0; k < 9; ++k) R[k] = (k % 4 == 0) ? 1.0f : 0.0f;
                        return;
                    }
                    for (float& x : ax) x /= s;
                    axis_angle(ax, std::atan2(s, c), R);
                };
                float R1[9], R2[9];
                const float kf[3] = {K0[0] - H[0], K0[1] - H[1], K0[2] - H[2]}, kt[3] = {K[0] - H[0], K[1] - H[1], K[2] - H[2]};
                turn(kf, kt, R1);
                for (int b : g_rig.leg_set[h][0])
                    if (b < n) corr_apply(g_corr[b], R1, H);
                float F1[3];
                corr_point(g_corr[fj], af, F1);
                const float ff[3] = {F1[0] - K[0], F1[1] - K[1], F1[2] - K[2]}, ft[3] = {af[0] - K[0], af[1] - K[1], af[2] - K[2]};
                turn(ff, ft, R2);
                for (int b : g_rig.leg_set[h][1])
                    if (b < n) corr_apply(g_corr[b], R2, K);
                // the foot's own orientation back to the animation's (planted flat as the game had it)
                if (!g_rig.leg_set[h][2].empty()) {
                    // drawn A M, wanted M: the turn A^T about the foot's joint
                    float Rf[9];
                    for (int i = 0; i < 3; ++i)
                        for (int k = 0; k < 3; ++k) Rf[i * 3 + k] = g_corr[fj].A[k * 3 + i];
                    float Fc[3];
                    corr_point(g_corr[fj], af, Fc);
                    for (int b : g_rig.leg_set[h][2])
                        if (b < n) corr_apply(g_corr[b], Rf, Fc);
                }
                if (slot == 0) {
                    float Fd[3];
                    corr_point(g_corr[fj], af, Fd);
                    g_feet_err[h] = dist3(Fd, af);
                }
            }
    }
    if (f.stance && !still && g_rig.chain[0] >= 0 && g_rig.chain[0] < n) {
        float rc[9];
        stance_rot(s + g_rig.chain[0] * 12, rc);
        if (f.pitch != 0) {
            float rp[9], t9[9];
            axis_angle(side, f.pitch, rp);
            mul3(rp, rc, t9);
            std::memcpy(rc, t9, sizeof(rc));
        }
        float piv[3];
        jdrawn(g_rig.chain[0], piv);
        for (int b : g_rig.chain_set[0])
            if (b < n) corr_apply(g_corr[b], rc, piv);
    }
    if (f.stance && !still && g_rig.pelvis >= 0 && g_rig.pelvis < n) {
        float rc[9], piv[3];
        stance_rot(s + g_rig.pelvis * 12, rc);
        jdrawn(g_rig.pelvis, piv);
        for (int b : g_rig.pelvis_set)
            if (b < n) corr_apply(g_corr[b], rc, piv);
    }
    // 1b. the torso position: running pushes spine00 forward of the root; the whole body (legs too, so nothing stretches)
    //     moves horizontally so spine00's joint sits over the root as at rest (round 6: with the pivots right, the
    //     torso came forward again when running)
    if (f.lock_torso && !still && f.actor_valid && g_rig.chain[0] >= 0) {  // (still: already over the actor)
        // over the character's own position (the camera anchor's base), as at rest: the rest offset of spine00 from
        // the model's origin, turned by the levelled root
        float sj[3], sjc[3];
        jdrawn(g_rig.chain[0], sj);
        corr_point(g_corr[g_rig.chain[0]], sj, sjc);
        const float* js = &g_rig.jbind[static_cast<size_t>(g_rig.chain[0]) * 3];
        float want[3];
        for (int k = 0; k < 3; ++k)
            want[k] = f.actor_world[k] - off[k] + ro[k * 3] * js[0] + ro[k * 3 + 1] * js[1] + ro[k * 3 + 2] * js[2];
        float d[3] = {want[0] - sjc[0], 0.0f, want[2] - sjc[2]};  // horizontal only (the set is the world, y up)
        float dl = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        if (dl > 0.25f) {  // a big jump is not a lean (a ragdoll, a vault): leave it
            d[0] = d[2] = 0;
        }
        for (int b = 1; b < n; ++b)
            for (int k = 0; k < 3; ++k) g_corr[b].a[k] += d[k];
        if (slot == 0) {
            float m = std::sqrt(d[0] * d[0] + d[2] * d[2]);
            g_torso_diag[0] = m;
            if (m > g_torso_diag[1]) g_torso_diag[1] = m;
        }
    }
    // 2. the facing lock: every bone but the root turned about the world's up through the root joint
    if (f.lock && f.yaw != 0) {
        const float* wr = f.w_root;
        float upl[3] = {wr[1], wr[5], wr[9]}, ups[3];  // the world's up in the root's frame, then in the set's space
        for (int k = 0; k < 3; ++k) ups[k] = mr[k * 3] * upl[0] + mr[k * 3 + 1] * upl[1] + mr[k * 3 + 2] * upl[2];
        norm(ups);
        float ry[9];
        axis_angle(ups, f.yaw, ry);
        float rootj[3];
        jdrawn(0, rootj);
        if (still) {  // about the pinned frame's own root joint (the animated one sways)
            const float* j0 = &g_rig.jbind[0];
            for (int k = 0; k < 3; ++k) rootj[k] = ro[k * 3] * j0[0] + ro[k * 3 + 1] * j0[1] + ro[k * 3 + 2] * j0[2] + still_o[k];
        }
        for (int b = 1; b < n; ++b) corr_apply(g_corr[b], ry, rootj);
    }
    if (slot == 0 && !g_still_init) still_reset();
    if (slot == 0) {  // "skel still": the torso's, the shoulders', the head's and the feet's drawn joints against the camera
        const int bs[kStillDiag] = {g_rig.pelvis, g_rig.chain[0], g_rig.chain[3], g_rig.clav[0], g_rig.clav[1], g_rig.arm[0], g_rig.arm[1],
                                    g_rig.head, g_rig.foot[0], g_rig.foot[1], g_rig.holster[0]};  // (the holster: the pistol bone)
        if (f.cam_ok) {
            const float ch = std::cos(f.view_h), sh = std::sin(f.view_h);
            for (int i = 0; i < kStillDiag; ++i) {
                const int b = bs[i];
                if (b < 0 || b >= n) continue;
                float p[3], q[3], d[3];
                jdrawn(b, p);
                corr_point(g_corr[b], p, q);
                for (int k = 0; k < 3; ++k) d[k] = q[k] + off[k] + (f.shift ? f.shift_world[k] : 0.0f) - f.cam[12 + k];
                // in the view's heading: right (cos h, -sin h), up, forward (-sin h, -cos h)
                const float v[3] = {ch * d[0] - sh * d[2], d[1], -sh * d[0] - ch * d[2]};
                float* rg = g_still_rng[i];
                for (int k = 0; k < 3; ++k) {
                    rg[k] = std::fmin(rg[k], v[k]);
                    rg[3 + k] = std::fmax(rg[3 + k], v[k]);
                }
            }
            ++g_still_n;
        }
        // run 10 item 8b: the knees' bend as drawn (the angle at the knee from the hip to the foot; 180 straight)
        for (int h = 0; h < 2; ++h) {
            const int hj = g_rig.hipj[h], kj = g_rig.knee[h], fj = g_rig.foot[h];
            if (hj < 0 || kj < 0 || fj < 0 || hj >= n || kj >= n || fj >= n) continue;
            float ph[3], pk[3], pf[3], t[3];
            jdrawn(hj, t);
            corr_point(g_corr[hj], t, ph);
            jdrawn(kj, t);
            corr_point(g_corr[kj], t, pk);
            jdrawn(fj, t);
            corr_point(g_corr[fj], t, pf);
            float u[3] = {ph[0] - pk[0], ph[1] - pk[1], ph[2] - pk[2]}, v[3] = {pf[0] - pk[0], pf[1] - pk[1], pf[2] - pk[2]};
            norm(u);
            norm(v);
            const float c = std::clamp(u[0] * v[0] + u[1] * v[1] + u[2] * v[2], -1.0f, 1.0f);
            const float a = std::acos(c) * 57.29578f;
            g_knee_deg[h] = a;
            g_knee_rng[h][0] = std::fmin(g_knee_rng[h][0], a);
            g_knee_rng[h][1] = std::fmax(g_knee_rng[h][1], a);
            g_hip_h[h] = ph[1] - pf[1];  // the hip joint over the foot (the leg's height)
        }
    }
    // 3. the shift
    if (f.shift) {
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = mr[k * 3 + 0] * f.shift_bind[0] + mr[k * 3 + 1] * f.shift_bind[1] + mr[k * 3 + 2] * f.shift_bind[2];
        for (int b = 1; b < n; ++b)
            for (int k = 0; k < 3; ++k) g_corr[b].a[k] += d[k];
    }
    // the transplant (left-handed, the gun drawn in John's left hand d while the game holds it in its right hand g):
    // hand g's IK turn on d's target (a rotation: the right hand's bind axes and palm sign on the left target, the gun
    // on the left controller as it sits on the right one right-handed), the game's wrist joint onto d's drawn wrist;
    // then into the left fist: TransplantMirror reflects the gun across its own middle plane (it maps onto itself) and
    // the result across d's x plane through that wrist (the mirror of a right hand's hold is a left hand's), two
    // reflections, a rotation; else TransplantPin moves the attachment point across that plane (a translation only).
    // Needs d's arm solved (its drawn wrist). record: slot 0's diagnostics.
    auto make_transplant = [&](Corr& t, bool record) -> bool {
        const int g = f.item_side, d = f.gun_j;
        if (g < 0 || g == d || g_rig.wrist[g] < 0 || g_rig.wrist[g] >= n || g_rig.wrist[d] < 0 || g_rig.wrist[d] >= n || g_rig.elbow[g] < 0 ||
            !f.ik[d])
            return false;
        const float* R = f.ik_r[d];
        // hand g's bind axes on d's target (the same turn as the IK's hand_turn)
        float Md[9];
        {
            const float* be = &g_rig.jbind[static_cast<size_t>(g_rig.elbow[g]) * 3];
            const float* bw = &g_rig.jbind[static_cast<size_t>(g_rig.wrist[g]) * 3];
            float fb[3] = {bw[0] - be[0], bw[1] - be[1], bw[2] - be[2]}, pb[3] = {0, -1, 0}, tb[3];
            norm(fb);
            const float dp = pb[0] * fb[0] + pb[1] * fb[1] + pb[2] * fb[2];
            for (int k = 0; k < 3; ++k) pb[k] -= dp * fb[k];
            norm(pb);
            cross(fb, pb, tb);
            const float sgn = g ? -1.0f : 1.0f;
            float fs_[3] = {-R[2], -R[5], -R[8]}, ps_[3] = {sgn * R[0], sgn * R[3], sgn * R[6]}, ts_[3];
            norm(fs_);
            const float dq = ps_[0] * fs_[0] + ps_[1] * fs_[1] + ps_[2] * fs_[2];
            for (int k = 0; k < 3; ++k) ps_[k] -= dq * fs_[k];
            norm(ps_);
            cross(fs_, ps_, ts_);
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j) Md[k * 3 + j] = fs_[k] * fb[j] + ps_[k] * pb[j] + ts_[k] * tb[j];
        }
        float mwo[9], mwt[9], jwg[3], jwd[3], Wd[3];
        ortho_cols(s + g_rig.wrist[g] * 12, mwo);  // the game's wrist g (the set is still the animated one here)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) mwt[i * 3 + j] = mwo[j * 3 + i];
        mul3(Md, mwt, t.A);
        jdrawn(g_rig.wrist[g], jwg);
        jdrawn(g_rig.wrist[d], jwd);
        corr_point(g_corr[g_rig.wrist[d]], jwd, Wd);  // d's wrist as drawn (the reach, the stretch included)
        for (int k = 0; k < 3; ++k) t.a[k] = Wd[k] - (t.A[k * 3] * jwg[0] + t.A[k * 3 + 1] * jwg[1] + t.A[k * 3 + 2] * jwg[2]);
        float X[3] = {R[0], R[3], R[6]};
        norm(X);
        const HeldSample* hs = newest_held(f.held);
        const bool hs_ok = hs && !hs->left;
        int mode = 0;
        float shift = 0.0f;
        if (f.mirror && hs_ok) {
            // the gun's middle plane: normal its x axis, through its muzzle (on the plane) or else its origin
            const float* G = hs->game;
            float ng[3] = {G[0], G[1], G[2]}, cg[3];
            norm(ng);
            for (int k = 0; k < 3; ++k)
                cg[k] = G[12 + k] - off[k] + (hs->mo_ok ? G[k] * hs->mo[0] + G[4 + k] * hs->mo[1] + G[8 + k] * hs->mo[2] : 0.0f);
            // A = Ax tA Ag, a = Ax (tA ag + ta) + ax; Ag = I - 2 ng ng^T, ag = 2 (cg.ng) ng; Ax = I - 2 X X^T, ax = 2 (Wd.X) X
            float Ag[9], Ax[9], T1[9], A2[9], ag[3], v[3], a2[3];
            const float cn = cg[0] * ng[0] + cg[1] * ng[1] + cg[2] * ng[2], wx = Wd[0] * X[0] + Wd[1] * X[1] + Wd[2] * X[2];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    Ag[i * 3 + j] = (i == j ? 1.0f : 0.0f) - 2.0f * ng[i] * ng[j];
                    Ax[i * 3 + j] = (i == j ? 1.0f : 0.0f) - 2.0f * X[i] * X[j];
                }
                ag[i] = 2.0f * cn * ng[i];
            }
            mul3(t.A, Ag, T1);
            mul3(Ax, T1, A2);
            for (int k = 0; k < 3; ++k) v[k] = t.A[k * 3] * ag[0] + t.A[k * 3 + 1] * ag[1] + t.A[k * 3 + 2] * ag[2] + t.a[k];
            for (int k = 0; k < 3; ++k) a2[k] = Ax[k * 3] * v[0] + Ax[k * 3 + 1] * v[1] + Ax[k * 3 + 2] * v[2] + 2.0f * wx * X[k];
            std::memcpy(t.A, A2, sizeof(A2));
            std::memcpy(t.a, a2, sizeof(a2));
            mode = 4;
        } else if (f.pin) {
            // the pin (round 8): John's left fist is the mirror of the transplanted right fist across d's x plane through
            // Wd, so a gun symmetric about its own middle plane, lying square in the hand, is mirrored by moving it -2e
            // along x, e measured on that middle plane: at the muzzle (the tune's MuzzleOffset) when the gun's x lies
            // along the plane's normal, else at the fist's centre, else at the wrist attachment
            float p[3] = {};
            if (hs_ok && hs->mo_ok) {
                const float* G = hs->game;
                float m[3], gx[3] = {G[0], G[1], G[2]}, xg[3];
                for (int k = 0; k < 3; ++k) m[k] = G[12 + k] - off[k] + G[k] * hs->mo[0] + G[4 + k] * hs->mo[1] + G[8 + k] * hs->mo[2];
                norm(gx);
                for (int k = 0; k < 3; ++k) xg[k] = t.A[k * 3] * gx[0] + t.A[k * 3 + 1] * gx[1] + t.A[k * 3 + 2] * gx[2];
                if (std::fabs(xg[0] * X[0] + xg[1] * X[1] + xg[2] * X[2]) >= 0.9f) {
                    corr_point(t, m, p);
                    mode = 1;
                }
            }
            if (!mode) {
                float c[3] = {0, 0, 0};
                int nf = 0;
                for (int i = 0; i < 15; ++i) {
                    const int fb = g_rig.finger[g][i];
                    if (fb < 0 || fb >= n) continue;
                    float j[3], q[3];
                    jdrawn(fb, j);
                    corr_point(t, j, q);
                    for (int k = 0; k < 3; ++k) c[k] += q[k];
                    ++nf;
                }
                if (nf >= 5) {
                    for (int k = 0; k < 3; ++k) p[k] = c[k] / static_cast<float>(nf);
                    mode = 2;
                }
            }
            if (!mode && g_rig.att_wrist[g] >= 0 && g_rig.att_wrist[g] < n) {
                float pa[3];
                jdrawn(g_rig.att_wrist[g], pa);
                corr_point(t, pa, p);
                mode = 3;
            }
            if (mode) {
                const float e = (p[0] - Wd[0]) * X[0] + (p[1] - Wd[1]) * X[1] + (p[2] - Wd[2]) * X[2];
                for (int k = 0; k < 3; ++k) t.a[k] -= 2.0f * e * X[k];
                shift = -2.0f * e;
            }
        }
        if (record) {
            g_xfer_shift = shift;
            g_xfer_pin_mode = mode;
            // the grip check: John's drawn wrist d in the drawn gun's frame against the game's wrist g in the game's gun
            // frame mirrored across the gun's middle plane (x' = 2 x0 - x; x0 the muzzle's x)
            g_xfer_grip_err = -1.0f;
            if (hs_ok) {
                const float* G = hs->game;
                float Gx[3][3], go[3];
                for (int c = 0; c < 3; ++c) {
                    float l = std::sqrt(G[c * 4] * G[c * 4] + G[c * 4 + 1] * G[c * 4 + 1] + G[c * 4 + 2] * G[c * 4 + 2]);
                    l = l > 1e-6f ? l : 1.0f;
                    for (int k = 0; k < 3; ++k) Gx[c][k] = G[c * 4 + k] / l;
                }
                for (int k = 0; k < 3; ++k) go[k] = G[12 + k] - off[k];
                float rg[3], rd[3], dgo[3], dax[3][3];
                for (int c = 0; c < 3; ++c) rg[c] = Gx[c][0] * (jwg[0] - go[0]) + Gx[c][1] * (jwg[1] - go[1]) + Gx[c][2] * (jwg[2] - go[2]);
                rg[0] = 2.0f * (hs->mo_ok ? hs->mo[0] : 0.0f) - rg[0];
                corr_point(t, go, dgo);
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k) dax[c][k] = t.A[k * 3] * Gx[c][0] + t.A[k * 3 + 1] * Gx[c][1] + t.A[k * 3 + 2] * Gx[c][2];
                for (int c = 0; c < 3; ++c) rd[c] = dax[c][0] * (Wd[0] - dgo[0]) + dax[c][1] * (Wd[1] - dgo[1]) + dax[c][2] * (Wd[2] - dgo[2]);
                g_xfer_grip_err = std::sqrt((rd[0] - rg[0]) * (rd[0] - rg[0]) + (rd[1] - rg[1]) * (rd[1] - rg[1]) + (rd[2] - rg[2]) * (rd[2] - rg[2]));
            }
        }
        return true;
    };
    // 4. the arms on the controllers: two-bone IK in the set's space (the world less `off`), then the hand turned
    bool pub_gun = false;  // run 6 item 9g: the drawn long gun's frame for the body points (world), set below
    float pub_gun_R[9] = {}, pub_gun_o[3] = {};
    bool pub_mgun = false;  // (step 7, r80 G4) the mirrored long gun's frame
    float pub_mgun_R[9] = {}, pub_mgun_o[3] = {};
    bool pub_fore = false;  // the foregrip point for the body points (world), set below
    float pub_fore_p[3] = {};
    int grasp_h = -1;  // (2026-10-28) [Reload] PartHandSnap: John's hand on a part (its fingers closed on it, below), its weight
    float grasp_b = 0.0f;
    if (f.ik[0] || f.ik[1]) {
        auto to_set = [&](const float* x, float* y, bool point) {
            for (int k = 0; k < 3; ++k) y[k] = x[k] - (point ? off[k] : 0.0f);
        };
        // the hand's turn for bind hand hb on a target turn rw9: its bind axes (the fingers along the bind forearm,
        // elbow -> wrist; the palm down, -y) to the target's (the fingers along the grip's -z, the palm toward -x for a
        // right hand, +x for a left): Md = [fs ps ts] [fb pb tb]^T, the drawn wrist's turn
        auto hand_turn = [&](int hb, const float* rw9, float* Md) {
            const float* be = &g_rig.jbind[static_cast<size_t>(g_rig.elbow[hb]) * 3];
            const float* bw = &g_rig.jbind[static_cast<size_t>(g_rig.wrist[hb]) * 3];
            float fb[3] = {bw[0] - be[0], bw[1] - be[1], bw[2] - be[2]}, pb[3] = {0, -1, 0}, tb[3];
            norm(fb);
            const float dp = pb[0] * fb[0] + pb[1] * fb[1] + pb[2] * fb[2];
            for (int k = 0; k < 3; ++k) pb[k] -= dp * fb[k];
            norm(pb);
            cross(fb, pb, tb);
            const float sgn = hb ? -1.0f : 1.0f;
            float fs_[3] = {-rw9[2], -rw9[5], -rw9[8]}, ps_[3] = {sgn * rw9[0], sgn * rw9[3], sgn * rw9[6]}, ts_[3];
            norm(fs_);
            const float dq = ps_[0] * fs_[0] + ps_[1] * fs_[1] + ps_[2] * fs_[2];
            for (int k = 0; k < 3; ++k) ps_[k] -= dq * fs_[k];
            norm(ps_);
            cross(fs_, ps_, ts_);
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j) Md[k * 3 + j] = fs_[k] * fb[j] + ps_[k] * pb[j] + ts_[k] * tb[j];
        };
        bool ik_done[2] = {false, false};
        // each hand's bind axes as columns (the fingers along the bind forearm, the palm down, their cross; hand_turn's)
        auto bind_axes = [&](int hb, float* B) {
            const float* be = &g_rig.jbind[static_cast<size_t>(g_rig.elbow[hb]) * 3];
            const float* bw = &g_rig.jbind[static_cast<size_t>(g_rig.wrist[hb]) * 3];
            float fb[3] = {bw[0] - be[0], bw[1] - be[1], bw[2] - be[2]}, pb[3] = {0, -1, 0}, tb[3];
            norm(fb);
            const float dp = pb[0] * fb[0] + pb[1] * fb[1] + pb[2] * fb[2];
            for (int k = 0; k < 3; ++k) pb[k] -= dp * fb[k];
            norm(pb);
            cross(fb, pb, tb);
            for (int k = 0; k < 3; ++k) {
                B[k * 3] = fb[k];
                B[k * 3 + 1] = pb[k];
                B[k * 3 + 2] = tb[k];
            }
        };
        // the foregrip on the drawn long gun (published; the snap target), found after the gun hand is solved
        bool fore_ok = false, snap_ok = false, barrel_ok = false;
        float fore_p[3] = {}, fore_O[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        float barrel_p[3] = {}, barrel_O[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // [Reload] BarrelHandSnap
        bool part_ok = false;
        float part_pt[3] = {};  // [Reload] PartHandSnap: the gripped part's handle, as drawn this frame
        float part_G[9] = {};   // (2026-10-28) the drawn gun's axes then (the hand's turn on the part)
        // (2026-10-28) [Reload] PartHandSnap on [Reload] CarriedBolt's bolt: the handle and the carried gun's axes from the
        // carrying hand's target of this frame (actions.cpp measures them in that target's frame), the free gun hand drawn
        // on it as the off hand on the bolt in hand
        bool cpart_ok = false;
        float cpart_pt[3] = {}, cpart_G[9] = {};
        const int cpart_h = f.carry_j >= 0 && f.carry_j <= 1 ? 1 - f.carry_j : -1;
        bool cpart_R_ok = true;  // (a rotation, or none this frame: each axis unit, square to the others, right-handed)
        {
            const float* Q = f.cpart_R;
            for (int c = 0; c < 3; ++c) {
                const float l = Q[c] * Q[c] + Q[3 + c] * Q[3 + c] + Q[6 + c] * Q[6 + c];
                const int d = (c + 1) % 3;
                const float dt = Q[c] * Q[d] + Q[3 + c] * Q[3 + d] + Q[6 + c] * Q[6 + d];
                cpart_R_ok = cpart_R_ok && l > 0.95f && l < 1.05f && std::fabs(dt) < 0.03f;
            }
            const float det = Q[0] * (Q[4] * Q[8] - Q[5] * Q[7]) - Q[1] * (Q[3] * Q[8] - Q[5] * Q[6]) + Q[2] * (Q[3] * Q[7] - Q[4] * Q[6]);
            cpart_R_ok = cpart_R_ok && det > 0.9f && det < 1.1f;
        }
        if (f.cpart_blend > 0.0f && cpart_h >= 0 && f.cpart_j == f.carry_j && f.ik[f.carry_j] && cpart_R_ok && std::isfinite(f.cpart_p[0] + f.cpart_p[1] + f.cpart_p[2])) {
            const float* Rc = f.ik_r[f.carry_j];  // columns: the target's axes
            for (int k = 0; k < 3; ++k)
                cpart_pt[k] = f.ik_t[f.carry_j][k] - off[k] + Rc[k * 3] * f.cpart_p[0] + Rc[k * 3 + 1] * f.cpart_p[1] + Rc[k * 3 + 2] * f.cpart_p[2];
            mul3(Rc, f.cpart_R, cpart_G);
            cpart_ok = true;
        }
        // (2026-10-28) the hand's turn on the part it grips: as it took hold (its turn then in the handle's frame, the gun's
        // axes turned by the handle's turn about the gun's z), kept while it grips, by the weight from the controller's own.
        // Md: the drawn turn; Rt: the target's axes it amounts to (the interaction spot's frame). [0] the part in hand, [1]
        // the carried bolt; taken by the frame's first set (every set of a frame alike), dropped with the weight
        struct PartHold {
            bool ok = false;
            float rel[9] = {};
            float R[9] = {};  // [1]: the carried gun's axes in the carrying hand's frame it was taken with
        };
        static PartHold s_hold[2];
        if (f.part_blend <= 0.0f) s_hold[0].ok = false;
        if (f.cpart_blend <= 0.0f) s_hold[1].ok = false;
        if (s_hold[1].ok && cpart_ok) {  // (the review) another carry's frame (its gun's frame made again): the hold taken again
            float dmax = 0.0f;
            for (int i = 0; i < 9; ++i) dmax = std::fmax(dmax, std::fabs(f.cpart_R[i] - s_hold[1].R[i]));
            if (dmax > 0.02f) s_hold[1].ok = false;
        }
        if (!s_hold[1].ok) std::memcpy(s_hold[1].R, f.cpart_R, sizeof(s_hold[1].R));
        auto hold_turn = [&](PartHold& ph, int h, const float* G, float turn_deg, float wt, float* Md, float* Rt) {
            float M0[9], P[9], Rz[9], Mt[9], M0t[9], D[9], ax[3], ang = 0.0f, Dp[9];
            const float az[3] = {0.0f, 0.0f, 1.0f};
            hand_turn(h, f.ik_r[h], M0);
            axis_angle(az, turn_deg * 0.0174532925f, Rz);
            mul3(G, Rz, P);  // the handle's frame
            if (!ph.ok) {  // rel = P^T M0
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) ph.rel[i * 3 + j] = P[0 * 3 + i] * M0[0 * 3 + j] + P[1 * 3 + i] * M0[1 * 3 + j] + P[2 * 3 + i] * M0[2 * 3 + j];
                ph.ok = true;
            }
            mul3(P, ph.rel, Mt);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) M0t[i * 3 + j] = M0[j * 3 + i];
            mul3(Mt, M0t, D);  // from the controller's turn to the hold's
            to_axis_angle(D, ax, &ang);
            axis_angle(ax, ang * (wt > 1.0f ? 1.0f : wt), Dp);
            mul3(Dp, M0, Md);
            mul3(Dp, f.ik_r[h], Rt);
        };
        const HeldSample* hs = newest_held(f.held);
        if (hs && hs->left) hs = nullptr;  // the snap is onto the gun in the game's right hand
        static int grip_weapon = -1;  // the weapon g_grip_cur eased toward (another: it starts over)
        if (hs && slot == 0 && f.weapon != grip_weapon) {
            grip_weapon = f.weapon;
            g_grip_cur.valid = false;
        }
        // the gun hand first: the front hand may snap onto its gun. John's right, or with the gun drawn in John's left
        // (the transplant, round 9) his left: the right hand then takes the left hand's grip mirrored across the gun's
        // middle plane, on the transplanted gun
        const bool mir = f.mirror && f.xfer && f.gun_j == 0 && f.item_side == 1;
        const int gh = mir ? 0 : 1, fh = 1 - gh;
        Corr xt;  // the transplant, once the gun hand is solved
        bool xt_ok = false;
        for (int pass = 0; pass < 2; ++pass) {
            const int h = pass == 0 ? gh : fh;
            if (mir && h == fh && !xt_ok) xt_ok = make_transplant(xt, false);
            // ([Hands] CarryByForegrip: none on the carried gun's put-away: it is drawn at the carrying hand, not this gun hand's)
            // (2026-11-02, GOAL-older-items item 20, the review: the slide pistols were never in the long guns' block below) a
            // pistol's gripped slide ([Reload] PistolSlides): the drawn gun as the long guns' (the gun hand's correction on the
            // game's gun, or on its placement undone), its handle there, for PartHandSnap's hold. (2026-11-03, round 73 check 6,
            // the user: "no pose when it is held by left hand and operated by right hand") drawn in John's left (the mirrored
            // transplant): the transplant's gun, the handle across the gun's middle plane, as the long guns' foregrip
            if (h == fh && !f.long_gun && hs && f.part_blend > 0.0f && !f.carry_put && ((!f.xfer && f.gun_j == 1) || (mir && xt_ok)) &&
                g_rig.wrist[1] >= 0 && g_rig.wrist[1] < n) {
                float Gm[9], gm_o[3], Gs[9], gs_o[3];
                const float* srcm = hs->game;
                for (int c = 0; c < 3; ++c) {
                    const float* row = hs->m + c * 4;
                    const float* rowg = srcm + c * 4;
                    float l = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]), lg = std::sqrt(rowg[0] * rowg[0] + rowg[1] * rowg[1] + rowg[2] * rowg[2]);
                    l = l > 1e-6f ? l : 1.0f;
                    lg = lg > 1e-6f ? lg : 1.0f;
                    for (int k = 0; k < 3; ++k) Gm[k * 3 + c] = row[k] / l, Gs[k * 3 + c] = rowg[k] / lg;
                }
                for (int k = 0; k < 3; ++k) gm_o[k] = hs->m[12 + k] - off[k], gs_o[k] = srcm[12 + k] - off[k];
                if (hs->placed) unplace(*hs, off, Gm, gm_o, Gs, gs_o);  // (as SnapToDrawnGun: the placement undone, as held_delta draws it)
                const int rb = g_rig.att_wrist[1] >= 0 && g_rig.att_wrist[1] < n ? g_rig.att_wrist[1] : g_rig.wrist[1];
                const Corr& cr = mir ? xt : g_corr[rb];
                float Gd[9], gd_o[3];
                mul3(cr.A, Gs, Gd);
                corr_point(cr, gs_o, gd_o);
                float pp[3] = {f.part_p[0], f.part_p[1], f.part_p[2]};
                if (mir) pp[0] = 2.0f * (hs->mo_ok ? hs->mo[0] : 0.0f) - pp[0];  // across the gun's middle plane
                for (int k = 0; k < 3; ++k) part_pt[k] = gd_o[k] + Gd[k * 3] * pp[0] + Gd[k * 3 + 1] * pp[1] + Gd[k * 3 + 2] * pp[2];
                std::memcpy(part_G, Gd, sizeof(part_G));
                part_ok = true;
            }
            if (h == fh && f.long_gun && hs && !f.carry_put && ((!f.xfer && f.gun_j == 1) || (mir && xt_ok)) && g_rig.wrist[0] >= 0 && g_rig.wrist[0] < n &&
                g_rig.wrist[1] >= 0 && g_rig.wrist[1] < n && g_rig.elbow[0] >= 0 && g_rig.elbow[1] >= 0) {
                // the gun's frames, each a 3x3 (columns its x y z axes, normalised) and an origin, in the set's space
                auto frame_of = [&](const float* m, float* Ax, float* o) {
                    for (int c = 0; c < 3; ++c) {
                        const float* row = m + c * 4;
                        float l = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                        l = l > 1e-6f ? l : 1.0f;
                        for (int k = 0; k < 3; ++k) Ax[k * 3 + c] = row[k] / l;
                    }
                    for (int k = 0; k < 3; ++k) o[k] = m[12 + k] - off[k];
                };
                float Gm[9], gm_o[3], Gg[9], gg_o[3];
                frame_of(hs->m, Gm, gm_o);     // the one the game's hand IK used (the placed prop's, or the game's)
                frame_of(hs->game, Gg, gg_o);  // the game's own (W +0x80)
                // learn (once a frame): the animated left wrist in that gun's frame, when it is on the gun
                static uint64_t learn_frame = ~0ull;
                if (slot == 0 && learn_frame != g_body_frame) {
                    learn_frame = g_body_frame;
                    float L[3], Ol[9], rel[3], rO[9];
                    jdrawn(g_rig.wrist[0], L);
                    ortho_cols(s + g_rig.wrist[0] * 12, Ol);
                    const float dv[3] = {L[0] - gm_o[0], L[1] - gm_o[1], L[2] - gm_o[2]};
                    for (int i = 0; i < 3; ++i) rel[i] = Gm[0 * 3 + i] * dv[0] + Gm[1 * 3 + i] * dv[1] + Gm[2 * 3 + i] * dv[2];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) rO[i * 3 + j] = Gm[0 * 3 + i] * Ol[0 * 3 + j] + Gm[1 * 3 + i] * Ol[1 * 3 + j] + Gm[2 * 3 + i] * Ol[2 * 3 + j];
                    float da = 0, dh = 0;
                    for (int k = 0; k < 3; ++k) {
                        da += (rel[k] - hs->ik[k]) * (rel[k] - hs->ik[k]);
                        dh += (rel[k] - hs->ik_hold[k]) * (rel[k] - hs->ik_hold[k]);
                    }
                    const bool has_ik = hs->ik[0] != 0.0f || hs->ik[1] != 0.0f || hs->ik[2] != 0.0f;
                    static int pair_weapon = -1, pair_frames = 0;  // the actor state's weapon and the sample's gun, together
                    static uintptr_t pair_W = 0;
                    if (f.weapon != pair_weapon || hs->W != pair_W) {
                        pair_weapon = f.weapon;
                        pair_W = hs->W;
                        pair_frames = 0;
                    } else if (pair_frames < 1000) {
                        ++pair_frames;
                    }
                    const bool keep = f.two_blend > 0.0f && g_same_frame_cfg.load(std::memory_order_relaxed);  // TwoHandedSteady: held
                    // the pose's hysteresis (run 8 item 2): the learned pose changes only when the other offset is nearer
                    // by half the two offsets' distance, at least 1 cm (2026-10-09: 1 cm alone let the game's wrist sway
                    // carry a lowered long gun's hand across the middle every 0.4 s); either rule's new pose only once
                    // wanted for 300 ms (take_pose)
                    const bool hyst = g_sawed_grip.load(std::memory_order_relaxed);
                    int learned = da < dh ? 1 : 0;
                    float sep = 0.0f;
                    for (int k = 0; k < 3; ++k) sep += (hs->ik[k] - hs->ik_hold[k]) * (hs->ik[k] - hs->ik_hold[k]);
                    const float margin = std::fmax(0.01f, 0.5f * std::sqrt(sep));
                    if (hyst && learned != g_grip_pose && std::fabs(std::sqrt(da) - std::sqrt(dh)) < margin) learned = g_grip_pose;
                    const int flag_pose = hs->aiming ? 1 : 0;
                    // a new pose taken once it has been wanted for 300 ms on end, whichever rule wants it (2026-10-09: a long
                    // gun held lowered about 30 degrees, the game's hand at the learning's 6 cm edge: the learned pose and the
                    // aim flag's in turn, a flip every frame or two, the ring 35-49 mm each time)
                    static int pose_cand = -1;
                    static double pose_since = 0.0, last_learn_ms = -1e9;
                    auto take_pose = [&](int want) {
                        if (hyst && want != g_grip_pose) {
                            if (want != pose_cand) {
                                pose_cand = want;
                                pose_since = log::now_ms();
                            }
                            if (log::now_ms() - pose_since < 300.0) return;
                        }
                        pose_cand = -1;
                        g_grip_pose = want;
                    };
                    if (keep) {
                        // the grip and its pose kept while the front hand holds the gun
                    } else if (pair_frames >= 5 && has_ik && std::fmin(da, dh) < 0.06f * 0.06f) {
                        if (f.two_blend > 0.0f) {  // "skel lag": learning while the front hand holds the gun
                            ++g_two_learns;
                            if (learned != g_grip_pose) ++g_two_flips;
                        }
                        take_pose(learned);
                        last_learn_ms = log::now_ms();
                        const int slot_pose = da < dh ? 1 : 0;  // the sample is the nearer pose's, whichever is used
                        const FingerRel fr_now = finger_rel(s, n, 0);  // the game's left hand's fingers in this hold
                        GripRel scratch;
                        const bool known_w = f.weapon >= 0 && f.weapon < kGripW;
                        GripRel& gr = known_w ? g_grips[f.weapon][slot_pose] : scratch;
                        // [Reload] SteadyRing: the mean of the first 30 samples, then kept (the game's wrist sways about the
                        // grip: re-learned each frame, the grip and its ring followed the sway, 1-4 cm)
                        int dummy_n = 0;
                        int& n = known_w ? g_grip_n[f.weapon][slot_pose] : dummy_n;
                        const bool steady = g_steady_grip.load(std::memory_order_relaxed);
                        if (!steady || !gr.valid || n < 30) {
                            if (steady && gr.valid && n > 0) {
                                const float w = 1.0f / static_cast<float>(n + 1);
                                for (int k = 0; k < 3; ++k) gr.p[k] += (rel[k] - gr.p[k]) * w;
                            } else {
                                std::memcpy(gr.p, rel, sizeof(rel));
                            }
                            std::memcpy(gr.O, rO, sizeof(rO));
                            gr.W = hs->W;
                            std::memcpy(gr.ik, hs->ik, 12);
                            std::memcpy(gr.ik + 3, hs->ik_hold, 12);
                            gr.valid = true;
                            if (n < 1000) ++n;
                            g_grip_tpl[slot_pose] = gr;
                            if (known_w) {
                                uint32_t bits = 1u << slot_pose;
                                if (fr_now.valid) {
                                    g_front_fingers[f.weapon][slot_pose] = fr_now;
                                    bits |= 4u << slot_pose;
                                }
                                g_front_seen[f.weapon].fetch_or(bits, std::memory_order_relaxed);
                            }
                        }
                        g_grip_learns.fetch_add(1, std::memory_order_relaxed);
                    } else if (!has_ik || std::fmin(da, dh) >= 0.06f * 0.06f) {
                        const bool own = f.weapon >= 0 && f.weapon < kGripW && (g_grips[f.weapon][0].valid || g_grips[f.weapon][1].valid);
                        // no grip learned this frame: the game's aim state picks the pose; a gun with no grip of its own
                        // (it borrows one) keeps the aiming pose (run 8 item 2: the aim flag slid its ring 3-10 cm)
                        const int pose = own || !g_sawed_grip.load(std::memory_order_relaxed) ? flag_pose : 1;
                        // the aim flag only once nothing was learned for a second: a hand at the 6 cm edge keeps its learned
                        // pose (2026-10-09: the flag's and the learned pose in turn, the ring 45 mm)
                        if (!(hyst && log::now_ms() - last_learn_ms < 1000.0)) {
                            if (f.two_blend > 0.0f && pose != g_grip_pose) ++g_two_flips;
                            take_pose(pose);
                        }
                    }
                }
                built_in_grips();
                const GripRel* want = nullptr;
                int src = 2;
                // [Weapon.<Gun>] FrontHandPose: the gun's fixed hold, else the game's state's
                const int fixed_pose = f.weapon >= 0 && f.weapon < kGripW ? g_front_pose[f.weapon].load(std::memory_order_relaxed) : 0;
                const int pose_use = fixed_pose == 1 ? 0 : fixed_pose == 2 ? 1 : g_grip_pose;
                if (f.weapon >= 0 && f.weapon < kGripW) {
                    if (g_grips[f.weapon][pose_use].valid)
                        want = &g_grips[f.weapon][pose_use], src = 0;
                    else if (g_grips[f.weapon][1 - pose_use].valid)
                        want = &g_grips[f.weapon][1 - pose_use], src = 0;
                }
                // the Sawed-off (15): the game never holds it two-handed, so it learns no grip; the Double-barrel's
                // as learned in the simulator (the same bones and IK offsets), before the template (the last long
                // gun's, in that gun's own frame: the ring sat elsewhere each session)
                if (!want && f.weapon == 15 && g_sawed_grip.load(std::memory_order_relaxed)) want = &g_grip_sawed[pose_use], src = 3;
                if (!want && g_fallback_cfg.load(std::memory_order_relaxed)) {
                    if (g_grip_tpl[pose_use].valid)
                        want = &g_grip_tpl[pose_use], src = 1;
                    else if (g_grip_tpl[1 - pose_use].valid)
                        want = &g_grip_tpl[1 - pose_use], src = 1;
                    else
                        want = &g_grip_builtin[pose_use], src = 2;
                }
                // a fallback: the hand kept where it took hold along the barrel (its z in the gun's frame, at the hold's start)
                static bool z_locked = false;
                static float lock_z = 0.0f;
                GripRel fbk;
                if (want && src != 0) {
                    if (slot == 0) {
                        if (f.two_blend <= 0.0f) {
                            z_locked = false;
                        } else if (!z_locked) {
                            float Gq[9], gq_o[3], Gdq[9], gdq_o[3], Tq[3], rt[3];
                            frame_of(hs->game, Gq, gq_o);
                            if (fixed_grip_on() && g_snap_drawn_cfg.load(std::memory_order_relaxed)) unplace(*hs, off, Gm, gm_o, Gq, gq_o);  // the drawn gun's
                            const int rb0 = g_rig.att_wrist[1] >= 0 && g_rig.att_wrist[1] < n ? g_rig.att_wrist[1] : g_rig.wrist[1];
                            const Corr& c0 = mir ? xt : g_corr[rb0];  // the drawn gun
                            mul3(c0.A, Gq, Gdq);
                            corr_point(c0, gq_o, gdq_o);
                            for (int k = 0; k < 3; ++k) Tq[k] = f.ik_t[fh][k] - off[k] - gdq_o[k];
                            for (int i = 0; i < 3; ++i) rt[i] = Gdq[0 * 3 + i] * Tq[0] + Gdq[1 * 3 + i] * Tq[1] + Gdq[2 * 3 + i] * Tq[2];
                            // short of the muzzle (6 cm in from the gun's MuzzleOffset: the Sawed-off's 0.65 m reached
                            // past its barrels), else a rifle's 0.65 m
                            float zmin = -0.65f;
                            if (g_sawed_grip.load(std::memory_order_relaxed) && hs->mo_ok && hs->mo[2] < -0.15f && hs->mo[2] + 0.06f > zmin)
                                zmin = hs->mo[2] + 0.06f;
                            lock_z = rt[2] < zmin ? zmin : rt[2] > -0.08f ? -0.08f : rt[2];
                            z_locked = true;
                        }
                    }
                    if (z_locked) {
                        fbk = *want;
                        fbk.p[2] = lock_z;
                        want = &fbk;
                    }
                }
                if (slot == 0) {
                    g_grip_src = want ? src : -1;
                    g_grip_src_pub.store(g_grip_src, std::memory_order_relaxed);
                    g_grip_pose_pub.store(pose_use, std::memory_order_relaxed);
                }
                if (want && slot == 0 && g_grip_cur_frame != g_body_frame) {
                    g_grip_cur_frame = g_body_frame;
                    if (!g_grip_cur.valid) {
                        g_grip_cur = *want;
                    } else {
                        constexpr float k = 0.25f;
                        for (int i = 0; i < 3; ++i) g_grip_cur.p[i] += (want->p[i] - g_grip_cur.p[i]) * k;
                        float ct[9], rd[9], ax[3], ang = 0, rp[9], on[9];
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j) ct[i * 3 + j] = g_grip_cur.O[j * 3 + i];
                        mul3(want->O, ct, rd);
                        to_axis_angle(rd, ax, &ang);
                        axis_angle(ax, ang * k, rp);
                        mul3(rp, g_grip_cur.O, on);
                        std::memcpy(g_grip_cur.O, on, sizeof(on));
                        g_grip_cur.W = want->W;
                        std::memcpy(g_grip_cur.ik, want->ik, sizeof(g_grip_cur.ik));
                    }
                }
                const GripRel* use = want && g_grip_cur.valid ? &g_grip_cur : nullptr;
                if (slot == 0) {  // "skel lag": the snap point's drift in the gun's frame while held
                    static bool held_before = false;
                    static float p0[3] = {};
                    const bool held_now = use && f.two_blend > 0.0f;
                    if (held_now && (!held_before || g_two_drift_rebase)) {
                        std::memcpy(p0, use->p, sizeof(p0));
                        g_two_drift_rebase = false;
                    }
                    if (held_now)
                        g_two_drift_max = std::fmax(g_two_drift_max, std::sqrt((use->p[0] - p0[0]) * (use->p[0] - p0[0]) + (use->p[1] - p0[1]) * (use->p[1] - p0[1]) +
                                                                             (use->p[2] - p0[2]) * (use->p[2] - p0[2])));
                    held_before = held_now;
                }
                if (use) {
                    // the drawn gun: the gun hand's correction (solved) on the game's gun, or the transplant
                    const int rb = g_rig.att_wrist[1] >= 0 && g_rig.att_wrist[1] < n ? g_rig.att_wrist[1] : g_rig.wrist[1];
                    const Corr& cr = mir ? xt : g_corr[rb];
                    float Gd[9], gd_o[3];
                    // [Reload] SnapToDrawnGun: with FixedGunGrip, the gun as drawn (the sample's matrix with its placement at
                    // the drawn hand undone, as held_delta draws it) instead of the game's own
                    const float* Gs = Gg;
                    const float* gs_o = gg_o;
                    float Gu[9], gu_o[3];
                    if (fixed_grip_on() && g_snap_drawn_cfg.load(std::memory_order_relaxed)) {
                        unplace(*hs, off, Gm, gm_o, Gu, gu_o);
                        Gs = Gu;
                        gs_o = gu_o;
                        if (slot == 0) g_snap_drawn_frames.fetch_add(1, std::memory_order_relaxed);
                    }
                    mul3(cr.A, Gs, Gd);
                    corr_point(cr, gs_o, gd_o);
                    {  // [Reload] LeverParts: the drawn gun held still in the gun hand's target through John's fire
                       // clip, as its draw is (this frame's: the clip swings it fast). Kept per set (2026-10-10: one for
                       // both, the previous frame's set wrote it last, its gun this frame's against last frame's offset:
                       // through each clip the front hand held off the drawn gun by the player's motion in a frame, on
                       // horseback up to 80 mm)
                        static float G2s[2][12];
                        static bool g2_oks[2] = {};
                        const int g2i = slot && !g_clip_shared_test.load(std::memory_order_relaxed) ? 1 : 0;
                        float* G2 = G2s[g2i];
                        bool& g2_ok = g2_oks[g2i];
                        // (2026-11-02, GOAL-offhand-changes C34c; round 68 check 5, the user's screenshots: a bolt action drawn by
                        // the left hand, the right on its foregrip: after a shot the right hand worked the bolt) the gun held
                        // still through John's fire clip on the gun hand's target, whichever hand that is (it was John's right
                        // alone, not the transplant's left: the front hand's snap followed the game's swinging gun)
                        static int g2_gh[2] = {-1, -1};
                        if (g2_gh[g2i] != gh) g2_ok = false, g2_gh[g2i] = gh;  // (a hand change: not the other hand's hold)
                        if (f.ik[gh] && (worked_flags() & kSteady)) {
                            float T[12], F[12], ti[12], X[12];
                            for (int i = 0; i < 3; ++i) {
                                for (int j = 0; j < 3; ++j) {
                                    T[i * 4 + j] = f.ik_r[gh][i * 3 + j];
                                    F[i * 4 + j] = Gd[i * 3 + j];
                                }
                                T[i * 4 + 3] = f.ik_t[gh][i] - off[i];
                                F[i * 4 + 3] = gd_o[i];
                            }
                            const bool clip = in_fire_clip();
                            const float fade = clip ? 0.0f : static_cast<float>((log::now_ms() - g_steady[gh].clip_end_ms) / 150.0);
                            if (!clip && fade >= 1.0f && inv34(T, ti)) {
                                mul34(ti, F, G2);
                                g2_ok = true;
                            } else if (g2_ok && steady_x(T, G2, F, fade, X)) {
                                float F2[12];
                                mul34(X, F, F2);
                                for (int i = 0; i < 3; ++i) {
                                    for (int j = 0; j < 3; ++j) Gd[i * 3 + j] = F2[i * 4 + j];
                                    gd_o[i] = F2[i * 4 + 3];
                                }
                            }
                        } else {
                            g2_ok = false;
                        }
                    }
                    if (!mir && slot == 0) {  // run 6 item 9g: the drawn gun's own frame, for its parts' zones (world)
                        pub_gun = true;
                        std::memcpy(pub_gun_R, Gd, sizeof(pub_gun_R));
                        for (int k = 0; k < 3; ++k) pub_gun_o[k] = gd_o[k] + off[k];
                    } else if (mir && slot == 0) {  // (step 7, r80 G4) mirrored in John's left hand: the transplant's frame
                        pub_mgun = true;
                        std::memcpy(pub_mgun_R, Gd, sizeof(pub_mgun_R));
                        for (int k = 0; k < 3; ++k) pub_mgun_o[k] = gd_o[k] + off[k];
                    }
                    // the grip moved by the offset: right (+x), up (+y), forward (-z); [Reload] PumpParts: back with the
                    // fore-end as drawn
                    float p[3] = {use->p[0] + f.fore_off[0], use->p[1] + f.fore_off[1], use->p[2] - f.fore_off[2] + (f.weapon == 17 ? kPumpSlide * f.pump : 0.0f)};
                    if (mir) p[0] = 2.0f * (hs->mo_ok ? hs->mo[0] : 0.0f) - p[0];  // across the gun's middle plane
                    for (int k = 0; k < 3; ++k) fore_p[k] = gd_o[k] + Gd[k * 3] * p[0] + Gd[k * 3 + 1] * p[1] + Gd[k * 3 + 2] * p[2];
                    mul3(Gd, use->O, fore_O);
                    if (mir) {
                        // the left hand's grip turn mirrored for the right hand: its finger and palm directions (F = O B0)
                        // reflected across the gun's middle plane, onto the right hand's bind axes (a rotation)
                        float B0[9], B1[9], F[9], ng[3] = {Gd[0], Gd[3], Gd[6]}, fs[3], ps[3], ts[3];
                        bind_axes(0, B0);
                        bind_axes(1, B1);
                        mul3(fore_O, B0, F);
                        norm(ng);
                        for (int k = 0; k < 3; ++k) {
                            fs[k] = F[k * 3];
                            ps[k] = F[k * 3 + 1];
                        }
                        const float dfs = fs[0] * ng[0] + fs[1] * ng[1] + fs[2] * ng[2], dps = ps[0] * ng[0] + ps[1] * ng[1] + ps[2] * ng[2];
                        for (int k = 0; k < 3; ++k) {
                            fs[k] -= 2.0f * dfs * ng[k];
                            ps[k] -= 2.0f * dps * ng[k];
                        }
                        cross(fs, ps, ts);
                        for (int k = 0; k < 3; ++k)
                            for (int j = 0; j < 3; ++j) fore_O[k * 3 + j] = fs[k] * B1[j * 3] + ps[k] * B1[j * 3 + 1] + ts[k] * B1[j * 3 + 2];
                    }
                    fore_ok = true;
                    snap_ok = f.snap && f.two_blend > 0.0f;
                    // [Reload] BarrelHandSnap: the grip point and the hand's turn carried through the barrels' turn as
                    // drawn (open_gun's Q_p: kBreakOpenDeg down about x at the hinge, by the drawn opening)
                    if (!mir && f.barrel_blend > 0.0f && (f.weapon == 15 || f.weapon == 16)) {
                        const float hp_[3] = {0.0f, 0.0403f, -0.0629f}, th = -actions::kBreakOpenDeg * f.barrel_open * 0.0174532925f;
                        const float c = std::cos(th), sn = std::sin(th);
                        const float Q[9] = {1, 0, 0, 0, c, -sn, 0, sn, c};
                        float pb[3], d[3] = {p[0] - hp_[0], p[1] - hp_[1], p[2] - hp_[2]}, QO[9];
                        for (int k = 0; k < 3; ++k) pb[k] = Q[k * 3] * d[0] + Q[k * 3 + 1] * d[1] + Q[k * 3 + 2] * d[2] + hp_[k];
                        for (int k = 0; k < 3; ++k) barrel_p[k] = gd_o[k] + Gd[k * 3] * pb[0] + Gd[k * 3 + 1] * pb[1] + Gd[k * 3 + 2] * pb[2];
                        mul3(Q, use->O, QO);
                        mul3(Gd, QO, barrel_O);
                        barrel_ok = true;
                    }
                    if (!mir && f.part_blend > 0.0f) {  // [Reload] PartHandSnap: the part's handle in this frame's drawn gun
                        for (int k = 0; k < 3; ++k) part_pt[k] = gd_o[k] + Gd[k * 3] * f.part_p[0] + Gd[k * 3 + 1] * f.part_p[1] + Gd[k * 3 + 2] * f.part_p[2];
                        std::memcpy(part_G, Gd, sizeof(part_G));
                        part_ok = true;
                    }
                }
            }
            if (!f.ik[h] || g_rig.arm[h] < 0 || g_rig.elbow[h] < 0 || g_rig.wrist[h] < 0 || g_rig.arm[h] >= n || g_rig.elbow[h] >= n ||
                g_rig.wrist[h] >= n)
                continue;
            float js[3], je[3], jw[3], S[3], E[3], W[3];
            jdrawn(g_rig.arm[h], js);
            jdrawn(g_rig.elbow[h], je);
            jdrawn(g_rig.wrist[h], jw);
            corr_point(g_corr[g_rig.arm[h]], js, S);
            corr_point(g_corr[g_rig.elbow[h]], je, E);
            corr_point(g_corr[g_rig.wrist[h]], jw, W);
            float l1 = 0, l2 = 0;
            for (int k = 0; k < 3; ++k) {
                l1 += (E[k] - S[k]) * (E[k] - S[k]);
                l2 += (W[k] - E[k]) * (W[k] - E[k]);
            }
            l1 = std::sqrt(l1);
            l2 = std::sqrt(l2);
            if (l1 < 0.05f || l2 < 0.05f) continue;
            float T[3], pole[3];
            to_set(f.ik_t[h], T, true);
            if (f.coll_hand_on[h])  // [Hands] GunCollision (2026-10-29): an empty hand drawn off a gun held still (its IK only)
                for (int k = 0; k < 3; ++k) T[k] += f.coll_hand[h][k];
            to_set(f.ik_pole[h], pole, false);
            if (h == fh && fore_ok && slot == 0) {
                pub_fore = true;
                for (int k = 0; k < 3; ++k) pub_fore_p[k] = fore_p[k] + off[k];
            }
            if (h == fh && barrel_ok)  // [Reload] BarrelHandSnap: the off hand onto the open barrels, by its weight
                for (int k = 0; k < 3; ++k) T[k] += (barrel_p[k] - T[k]) * f.barrel_blend;
            // [Reload] PartHandSnap: the off hand's interaction spot onto the part it grips, by its weight; (2026-10-28) the
            // hand turned to its hold on the part (the spot in that turn's frame), and the free gun hand on the carried bolt
            int part_k = h == fh && part_ok ? 0 : h == cpart_h && cpart_ok && h != f.carry_j ? 1 : -1;
            // (the review: the hold only on a bolt, in hand or carried; the other parts' hand at the controller's turn, as before)
            const bool part_hold = part_k == 1 || (part_k == 0 && f.part_bolt);
            float part_Md[9] = {}, part_Rt[9] = {}, part_off[3] = {};  // (part_off: the wrist to the spot or the grasp point, drawn)
            if (part_k >= 0) {
                const float* ppt = part_k ? cpart_pt : part_pt;
                const float pb = part_k ? f.cpart_blend : f.part_blend;
                if (part_hold)
                    hold_turn(s_hold[part_k], h, part_k ? cpart_G : part_G, part_k ? f.cpart_turn : f.part_turn, pb, part_Md, part_Rt);
                else
                    std::memcpy(part_Rt, f.ik_r[h], sizeof(part_Rt));
                float go[3], w[3], gq[3];
                holster::interact_offset(go);
                const float o[3] = {(h == 0 ? -1.0f : 1.0f) * go[0], go[1], -go[2]};  // as holster::grab_point
                const float* R = part_Rt;
                // (r230) a bolt: its knob in the closed hand, the pose's grasp point on it (the hand's turn takes it from the
                // wrist); without a pose, and the other parts, the interaction spot as before
                FingerRel gscr;
                int gsrc = 0;
                const FingerRel* gp = part_hold ? grasp_pose(h, &gscr, &gsrc) : nullptr;
                if (gp && grasp_point(h, *gp, gq))
                    for (int k = 0; k < 3; ++k) part_off[k] = part_Md[k * 3] * gq[0] + part_Md[k * 3 + 1] * gq[1] + part_Md[k * 3 + 2] * gq[2];
                else
                    for (int k = 0; k < 3; ++k) part_off[k] = R[k * 3] * o[0] + R[k * 3 + 1] * o[1] + R[k * 3 + 2] * o[2];
                for (int k = 0; k < 3; ++k) w[k] = ppt[k] - part_off[k];
                for (int k = 0; k < 3; ++k) T[k] += (w[k] - T[k]) * pb;
                if (part_hold) {
                    grasp_h = h;  // its fingers closed on the bolt (below, after the other finger poses; both sets)
                    grasp_b = pb;
                }
            }
            const float T_ctl[3] = {T[0], T[1], T[2]};  // the controller's target, before the snap
            // [Hands] CarryByForegrip: the carrying hand at its learned hold, before and instead of the snap (the review:
            // in the put-away's first second the game's gun is still in hand and two-handed, its foregrip placed from this
            // very hand; snapped to it the hand would chase itself)
            const bool carrying = h == f.carry_j && g_carry_rel_ok[h];
            if (h == fh && snap_ok && !carrying)  // the front hand onto the gun's grip, by the two-handed weight
                for (int k = 0; k < 3; ++k) T[k] += (fore_p[k] - T[k]) * f.two_blend;
            // the snapped hold learned (its turn below), or the carrying hand drawn at it
            float M_ctl[9];
            bool carry_learn = false, carry_held = false;
            if (h == fh && f.carry_j < 0 && f.two_blend > 0.0f && !snap_ok && g_carry_rel_ok[h]) {
                g_carry_rel_ok[h] = false;  // two-handed without the snap: no hold to keep (the controller's own)
                ++g_carry_clears;
            }
            // (2026-10-30, round 55, item 8; r297: the carried gun's place on the bone came out the same moving or still,
            // yet the carrying hand ended up to 9 degrees and 4 cm off its controller) learned only while the front controller
            // is still in its own tracking space, and the gun hand's controller too (the review: the drawn gun's turn follows
            // the gun hand; a swing about the foregrip keeps the front hand still), both under kHoldStillSpeed and
            // kHoldStillTurn for kHoldStillMs on end: moving, the drawn gun's grip lags
            // the controller, and the hold learned then, kept at a moving let-go, drew the carrying hand off it. The last still
            // hold stays until then
            // (the review) judged once a sample (a frame apart at least), the verdict kept for that frame's other draws; and
            // a new grip (the two-handed weight rising from 0, or another gun) learns its own hold at its first frame
            bool hold_still = false;
            if (h == fh && slot == 0) {
                constexpr double kHoldStillSpeed = 0.15, kHoldStillTurn = 0.52;  // m/s, rad/s (30 degrees a second)
                static double s_hold_ms[2] = {0.0, 0.0};
                static float s_hold_p[2][2][3] = {}, s_hold_q[2][2][4] = {};  // [the front hand][its controller, the gun hand's]
                static bool s_still[2] = {false, false};
                static double s_still_from[2] = {-1.0, -1.0};  // (r298) still since: a pause of a stepped move is no rest
                constexpr double kHoldStillMs = 250.0;
                static float s_blend_was[2] = {0.0f, 0.0f};
                static int s_weapon_was[2] = {-1, -1};
                if (f.carry_j < 0 && ((f.two_blend > 0.0f && s_blend_was[h] <= 0.0f) || f.weapon != s_weapon_was[h]) && g_carry_rel_ok[h]) {
                    g_carry_rel_ok[h] = false;  // (a new grip: no hold of the last one kept)
                    ++g_carry_clears;
                }
                s_blend_was[h] = f.two_blend;
                s_weapon_was[h] = f.weapon;
                const double now_h = log::now_ms(), dt = (now_h - s_hold_ms[h]) * 0.001;
                if (dt > 0.008 || dt < 0.0) {
                    bool still = dt > 0.0 && dt < 0.2;
                    for (int c = 0; c < 2; ++c) {
                        const hands::Hand hc = hands::get(f.ctrl[c == 0 ? h : 1 - h]);
                        float* sp = s_hold_p[h][c];
                        float* sq = s_hold_q[h][c];
                        if (still) {
                            const double dx = hc.pos[0] - sp[0], dy = hc.pos[1] - sp[1], dz = hc.pos[2] - sp[2];
                            const double dq = std::fabs(hc.rot[0] * sq[0] + hc.rot[1] * sq[1] + hc.rot[2] * sq[2] + hc.rot[3] * sq[3]);
                            const double turn = 2.0 * std::acos(dq > 1.0 ? 1.0 : dq);
                            still = std::sqrt(dx * dx + dy * dy + dz * dz) / dt < kHoldStillSpeed && turn / dt < kHoldStillTurn;
                        }
                        std::memcpy(sp, hc.pos, sizeof(float) * 3);
                        std::memcpy(sq, hc.rot, sizeof(float) * 4);
                    }
                    if (!still) s_still_from[h] = -1.0;
                    else if (s_still_from[h] < 0.0) s_still_from[h] = now_h;
                    s_still[h] = still && now_h - s_still_from[h] >= kHoldStillMs;
                    s_hold_ms[h] = now_h;
                }
                hold_still = s_still[h];
            }
            if (h == fh && snap_ok && !carrying && f.carry_j < 0 && f.two_blend >= 0.999f && slot == 0 && (hold_still || !g_carry_rel_ok[h])) {
                hand_turn(h, f.ik_r[h], M_ctl);
                const float d[3] = {T[0] - T_ctl[0], T[1] - T_ctl[1], T[2] - T_ctl[2]};
                for (int i = 0; i < 3; ++i) g_carry_prel[h][i] = M_ctl[i] * d[0] + M_ctl[3 + i] * d[1] + M_ctl[6 + i] * d[2];  // M^T d
                carry_learn = true;
                ++g_carry_learns;
            } else if (carrying) {
                ++g_carry_holds;
                hand_turn(h, f.ik_r[h], M_ctl);
                const float* p = g_carry_prel[h];
                for (int k = 0; k < 3; ++k) T[k] += M_ctl[k * 3] * p[0] + M_ctl[k * 3 + 1] * p[1] + M_ctl[k * 3 + 2] * p[2];
                carry_held = true;
            }
            float D[3] = {T[0] - S[0], T[1] - S[1], T[2] - S[2]};
            float dl = std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
            if (dl < 1e-4f) continue;
            float u[3] = {D[0] / dl, D[1] / dl, D[2] / dl};
            float lo = std::fabs(l1 - l2) + 1e-3f, hi = l1 + l2 - 1e-3f;
            float d = dl < lo ? lo : dl > hi ? hi : dl;
            float Tc[3] = {S[0] + u[0] * d, S[1] + u[1] * d, S[2] + u[2] * d};  // the target the arm can reach
            if (slot == 0) {
                float* dg = g_ik_diag[h];
                for (int k = 0; k < 3; ++k) {
                    dg[k] = S[k];
                    dg[3 + k] = E[k];
                    dg[6 + k] = W[k];
                    dg[9 + k] = T[k];
                }
                dg[12] = l1;
                dg[13] = l2;
                dg[14] = dl;
                dg[15] = f.ik_t[h][1];
            }
            float ca = (l1 * l1 + d * d - l2 * l2) / (2 * l1 * d);
            ca = ca < -1 ? -1 : ca > 1 ? 1 : ca;
            float sa = std::sqrt(1 - ca * ca);
            float pp = pole[0] * u[0] + pole[1] * u[1] + pole[2] * u[2];
            float pv[3] = {pole[0] - pp * u[0], pole[1] - pp * u[1], pole[2] - pp * u[2]};
            // ArmTwist's weight: the bend plane is undefined with the reach along the pole, so the IK roll fades out
            // between 25 and 10 degrees from it (the animation's roll then), never spinning the arm about itself
            float tw = 0.0f;
            {
                const float pl = std::sqrt(pole[0] * pole[0] + pole[1] * pole[1] + pole[2] * pole[2]);
                const float pvs = pl > 1e-6f ? std::sqrt(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]) / pl : 0.0f;
                tw = (pvs - 0.17f) / (0.42f - 0.17f);
                tw = tw < 0.0f ? 0.0f : tw > 1.0f ? 1.0f : tw;
                tw = tw * tw * (3.0f - 2.0f * tw);
            }
            norm(pv);
            float E2[3];
            for (int k = 0; k < 3; ++k) E2[k] = S[k] + u[k] * l1 * ca + pv[k] * l1 * sa;
            auto from_to = [&](const float* a0, const float* b0, float* R) {
                float a1[3] = {a0[0], a0[1], a0[2]}, b1[3] = {b0[0], b0[1], b0[2]}, ax[3];
                norm(a1);
                norm(b1);
                cross(a1, b1, ax);
                float sn = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                float cs = a1[0] * b1[0] + a1[1] * b1[1] + a1[2] * b1[2];
                if (sn < 1e-6f) {
                    float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                    std::memcpy(R, id, sizeof(id));
                    return;
                }
                for (int k = 0; k < 3; ++k) ax[k] /= sn;
                axis_angle(ax, std::atan2(sn, cs), R);
            };
            // the upper arm: E -> E2 about the shoulder
            float v1[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]}, v2[3] = {E2[0] - S[0], E2[1] - S[1], E2[2] - S[2]}, R1[9];
            from_to(v1, v2, R1);
            for (int b : g_rig.arm_set[h]) corr_apply(g_corr[b], R1, S);
            // [Hands] ArmTwist (round 8: left-handed, John's empty right forearm took the game's gun-hand animation's
            // roll and swivelled with every movement): each arm bone's elbow hinge (in the bind pose the upper arm x
            // the model's forward: the elbow bends forward) is turned about the bone's own axis onto the bend plane's
            // normal nh = pv x u, so the roll comes from the IK alone; the forearm's roll bone then takes
            // ForearmTwistShare of the hand's twist about the forearm (after the hand is set).
            const bool twist = g_twist_cfg.load(std::memory_order_relaxed);
            float nb[3] = {0, 0, 0}, nh[3] = {0, 0, 0};
            auto twist_about = [&](const float* axis, const float* from, const float* to) {  // the signed angle
                float px[3], py[3], c[3];
                const float dx = from[0] * axis[0] + from[1] * axis[1] + from[2] * axis[2], dy = to[0] * axis[0] + to[1] * axis[1] + to[2] * axis[2];
                for (int k = 0; k < 3; ++k) {
                    px[k] = from[k] - dx * axis[k];
                    py[k] = to[k] - dy * axis[k];
                }
                cross(px, py, c);
                return std::atan2(c[0] * axis[0] + c[1] * axis[1] + c[2] * axis[2], px[0] * py[0] + px[1] * py[1] + px[2] * py[2]);
            };
            auto hinge_of = [&](int b, float* o) {  // bone b's bind hinge as drawn now
                float rb[9], R[9];
                ortho_cols(s + b * 12, rb);
                mul3(g_corr[b].A, rb, R);
                for (int k = 0; k < 3; ++k) o[k] = R[k * 3] * nb[0] + R[k * 3 + 1] * nb[1] + R[k * 3 + 2] * nb[2];
            };
            auto roll_onto = [&](int b, const float* axis, const std::vector<int>* set, const float* pivot) {
                float hc[3], Rt[9];
                hinge_of(b, hc);
                const float ang = tw * twist_about(axis, hc, nh);
                if (slot == 0 && b == g_rig.elbow[h]) {
                    float* fr = g_fore_roll[h];
                    fr[0] = ang * 57.2957795f;
                    fr[1] = std::fmin(fr[1], fr[0]);
                    fr[2] = std::fmax(fr[2], fr[0]);
                }
                axis_angle(axis, ang, Rt);
                if (set) {
                    for (int c : *set) corr_apply(g_corr[c], Rt, pivot);
                } else {
                    float j[3], jd[3];
                    jdrawn(b, j);
                    corr_point(g_corr[b], j, jd);
                    corr_apply(g_corr[b], Rt, jd);
                }
            };
            if (twist) {
                const float* ja = &g_rig.jbind[static_cast<size_t>(g_rig.arm[h]) * 3];
                const float* jeb = &g_rig.jbind[static_cast<size_t>(g_rig.elbow[h]) * 3];
                float ab[3] = {jeb[0] - ja[0], jeb[1] - ja[1], jeb[2] - ja[2]}, fw[3] = {0.0f, 0.0f, -g_cfg_fwd_sign};
                cross(ab, fw, nb);
                cross(pv, u, nh);
                norm(nb);
                norm(nh);
                float ax[3] = {E2[0] - S[0], E2[1] - S[1], E2[2] - S[2]};
                norm(ax);
                roll_onto(g_rig.arm[h], ax, &g_rig.arm_set[h], S);
                if (g_rig.armroll[h] >= 0 && g_rig.armroll[h] < n) roll_onto(g_rig.armroll[h], ax, nullptr, nullptr);
            }
            // the forearm: the wrist where R1 put it -> the target, about the new elbow
            float W1[3];
            corr_point(g_corr[g_rig.wrist[h]], jw, W1);
            float v3[3] = {W1[0] - E2[0], W1[1] - E2[1], W1[2] - E2[2]}, v4[3] = {Tc[0] - E2[0], Tc[1] - E2[1], Tc[2] - E2[2]}, R2[9];
            from_to(v3, v4, R2);
            for (int b : g_rig.elbow_set[h]) corr_apply(g_corr[b], R2, E2);
            float fax[3] = {Tc[0] - E2[0], Tc[1] - E2[1], Tc[2] - E2[2]};
            norm(fax);
            if (twist) roll_onto(g_rig.elbow[h], fax, &g_rig.elbow_set[h], E2);
            // the hand: its bind axes (the fingers along the bind forearm, elbow -> wrist; the palm down, -y) to the
            // controller's (the fingers along the grip's -z, the palm toward -x for the right hand, +x for the left)
            const float* mw = s + g_rig.wrist[h] * 12;
            float Md[9], Mc[9], Mct[9], R3[9];
            hand_turn(h, f.ik_r[h], Md);
            if (part_k >= 0 && part_hold) std::memcpy(Md, part_Md, sizeof(Md));  // (2026-10-28) PartHandSnap: its hold on the bolt
            if (h == fh && barrel_ok) {  // [Reload] BarrelHandSnap: the hand turned with the barrels' grip
                float Mt[9], Rd[9], ax[3], ang = 0, Rp[9], Mn[9];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) Mt[i * 3 + j] = Md[j * 3 + i];
                mul3(barrel_O, Mt, Rd);
                to_axis_angle(Rd, ax, &ang);
                axis_angle(ax, ang * f.barrel_blend, Rp);
                mul3(Rp, Md, Mn);
                std::memcpy(Md, Mn, sizeof(Md));
            }
            if (h == fh && snap_ok && !carrying) {  // the hand turned to the game's grip (the drawn wrist is then exactly Md)
                if (f.two_blend >= 0.999f) {
                    std::memcpy(Md, fore_O, sizeof(Md));
                } else {
                    float Mt[9], Rd[9], ax[3], ang = 0, Rp[9], Mn[9];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) Mt[i * 3 + j] = Md[j * 3 + i];
                    mul3(fore_O, Mt, Rd);  // from the controller's turn to the grip's
                    to_axis_angle(Rd, ax, &ang);
                    axis_angle(ax, ang * f.two_blend, Rp);
                    mul3(Rp, Md, Mn);
                    std::memcpy(Md, Mn, sizeof(Md));
                }
            }
            if (carry_learn) {  // the snapped turn in the controller's frame: M_ctl^T Md
                float Mt[9];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) Mt[i * 3 + j] = M_ctl[j * 3 + i];
                mul3(Mt, Md, g_carry_orel[h]);
                g_carry_rel_ok[h] = true;
            } else if (carry_held) {
                mul3(M_ctl, g_carry_orel[h], Md);
            }
            float mwo[9], tmp[9];
            ortho_cols(mw, mwo);
            mul3(g_corr[g_rig.wrist[h]].A, mwo, tmp);  // the wrist's current orientation, all corrections so far
            std::memcpy(Mc, tmp, sizeof(Mc));
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) Mct[i * 3 + j] = Mc[j * 3 + i];
            mul3(Md, Mct, R3);
            for (int b : g_rig.wrist_set[h]) corr_apply(g_corr[b], R3, Tc);
            static float wr_prev[2][2] = {};  // the hand's twist about the forearm, last (per hand and set), for its unwrap
            static bool wr_have[2][2] = {};
            const int si = slot ? 1 : 0;
            if (twist && g_rig.wristroll[h] >= 0 && g_rig.wristroll[h] < n) {  // the forearm's roll bone: onto the hinge, then a share
                const float share = g_twist_share.load(std::memory_order_relaxed);
                float hh[3], hw[3], Rt[9], j[3], jd[3];
                for (int k = 0; k < 3; ++k) hh[k] = Md[k * 3] * nb[0] + Md[k * 3 + 1] * nb[1] + Md[k * 3 + 2] * nb[2];
                hinge_of(g_rig.wristroll[h], hw);  // its own animated roll, absolute (as the other arm bones)
                float th = twist_about(fax, nh, hh);
                if (wr_have[h][si]) {  // nearest to the last, wrapped only past +-225 degrees (no flip at 180)
                    th = wr_prev[h][si] + std::remainder(th - wr_prev[h][si], 6.2831853f);
                    if (th > 3.9269908f) th -= 6.2831853f;
                    else if (th < -3.9269908f) th += 6.2831853f;
                }
                wr_prev[h][si] = th;
                wr_have[h][si] = true;
                axis_angle(fax, tw * (twist_about(fax, hw, nh) + share * th), Rt);
                jdrawn(g_rig.wristroll[h], j);
                corr_point(g_corr[g_rig.wristroll[h]], j, jd);
                corr_apply(g_corr[g_rig.wristroll[h]], Rt, jd);
                if (slot == 0) g_twists.fetch_add(1, std::memory_order_relaxed);
            } else {
                wr_have[h][si] = false;
            }
            // hands only: the hand stays on the controller past the arm's reach (the arm is hidden, nothing stretches
            // in view; left-handed, John's gun hand reaches across to the left controller)
            float ks = 1.0f;  // the arm's stretch (1: none)
            if (f.show == 2 && d != dl) {
                for (int b : g_rig.wrist_set[h])
                    for (int k = 0; k < 3; ++k) g_corr[b].a[k] += T[k] - Tc[k];
            } else if (f.stretch_max > 1.0f && dl > d) {
                // the arm stretch (round 7): past John's reach (the arm straight) the upper arm and the forearm both
                // lengthen by ks along their own axes, about their joints, K = I + (ks - 1) u u^T (the girth kept); the
                // hand only moves, so it keeps its size and turn and the aim and the props on it stay right. The wrist
                // ends at S + ks (Tc - S): on the target until the cap.
                ks = dl / d < f.stretch_max ? dl / d : f.stretch_max;
                auto lengthen = [&](const std::vector<int>& def, const std::vector<int>& att, const float* p, const float* q) {
                    float ax[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]}, K[9];
                    norm(ax);
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) K[r * 3 + c] = (r == c ? 1.0f : 0.0f) + (ks - 1.0f) * ax[r] * ax[c];
                    for (int b : def) corr_apply(g_corr[b], K, p);
                    for (int b : att) {  // moved where the stretch takes its joint, never scaled
                        float j[3], y[3];
                        jdrawn(b, j);
                        corr_point(g_corr[b], j, y);
                        const float along = (ks - 1.0f) * ((y[0] - p[0]) * ax[0] + (y[1] - p[1]) * ax[1] + (y[2] - p[2]) * ax[2]);
                        for (int k = 0; k < 3; ++k) g_corr[b].a[k] += along * ax[k];
                    }
                };
                lengthen(g_rig.seg_def[h][0], g_rig.seg_att[h][0], S, E2);  // the upper arm about the shoulder
                float dE[3], E3[3], W3[3];
                for (int k = 0; k < 3; ++k) {
                    dE[k] = (ks - 1.0f) * (E2[k] - S[k]);
                    E3[k] = E2[k] + dE[k];
                    W3[k] = Tc[k] + dE[k];
                }
                for (int b : g_rig.elbow_set[h])
                    for (int k = 0; k < 3; ++k) g_corr[b].a[k] += dE[k];  // the forearm and the hand go with the elbow
                lengthen(g_rig.seg_def[h][1], g_rig.seg_att[h][1], E3, W3);  // the forearm about the moved elbow
                for (int b : g_rig.wrist_set[h])
                    for (int k = 0; k < 3; ++k) g_corr[b].a[k] += (ks - 1.0f) * (Tc[k] - E2[k]);  // the hand to its end
                if (slot == 0) g_stretches.fetch_add(1, std::memory_order_relaxed);
            }
            ik_done[h] = true;
            if (slot == 0 && part_k >= 0 && (part_k ? f.cpart_blend : f.part_blend) >= 0.999f) {
                // (2026-10-28) "skel partsnap": the drawn hand's spot (a bolt: the grasp point) against the handle
                float dwp[3], e2 = 0.0f;
                corr_point(g_corr[g_rig.wrist[h]], jw, dwp);
                const float* ppt = part_k ? cpart_pt : part_pt;
                for (int k = 0; k < 3; ++k) {
                    const float e = dwp[k] + part_off[k] - ppt[k];
                    e2 += e * e;
                }
                g_part_spot_miss[part_k] = std::sqrt(e2);
                g_part_spot_max[part_k] = std::fmax(g_part_spot_max[part_k], g_part_spot_miss[part_k]);
                ++g_part_spot_n[part_k];
                if (part_k) ++g_cpart_snaps;
            }
            if (slot == 0) {  // "skel lag": the drawn wrist against its target (the reach and the stretch's cap)
                float dwp[3];
                corr_point(g_corr[g_rig.wrist[h]], jw, dwp);
                const float miss = std::sqrt((dwp[0] - T[0]) * (dwp[0] - T[0]) + (dwp[1] - T[1]) * (dwp[1] - T[1]) + (dwp[2] - T[2]) * (dwp[2] - T[2]));
                g_ik_miss_max[h] = std::fmax(g_ik_miss_max[h], miss);
                static uint64_t counted[2] = {~0ull, ~0ull};  // once a frame (slot 0 runs for every player-set draw)
                if (counted[h] != g_body_frame) {
                    counted[h] = g_body_frame;
                    if (miss > 0.01f) ++g_ik_miss_frames[h];
                    ++g_ik_frames[h];
                    if (g_carry_trace_on.load(std::memory_order_relaxed)) {  // ("skel carry trace": the drawn wrist, carry_drawn_note)
                        CarryDrawn& cw = g_carry_wrist[h];
                        cw.frame = g_body_frame;
                        for (int k = 0; k < 3; ++k) cw.w_t[k] = dwp[k] + off[k];
                        std::memcpy(cw.w_r, Md, sizeof(cw.w_r));
                        cw.w_miss = miss;
                        for (int k = 0; k < 3; ++k) cw.w_s[k] = S[k] + off[k];
                        cw.w_dl = dl;
                        cw.w_len = l1 + l2;
                        cw.w_ks = ks;
                        cw.w_d = d;
                        cw.w_ok = true;
                    }
                }
            }
            if (slot == 0) {
                g_ik_diag[h][19] = ks;
                corr_point(g_corr[g_rig.wrist[h]], jw, g_ik_diag[h] + 16);
                if (h == fh) {  // [Reload] BarrelHandSnap: its miss, and the snap's own jump: the weight's change in a frame times
                                // the turned grip point's distance from the controller's target (what the ease alone moves the hand)
                    static float prev_w = 0.0f;
                    static uint64_t prev_frame = 0;
                    const float* dw = g_ik_diag[fh] + 16;
                    if (part_ok && f.part_blend >= 0.999f) {  // "skel partsnap": the drawn wrist against the part's handle
                        ++g_part_snaps;
                        g_part_miss = std::sqrt((dw[0] - part_pt[0]) * (dw[0] - part_pt[0]) + (dw[1] - part_pt[1]) * (dw[1] - part_pt[1]) +
                                                (dw[2] - part_pt[2]) * (dw[2] - part_pt[2]));
                        g_part_miss_max = std::fmax(g_part_miss_max, g_part_miss);
                    }
                    if (barrel_ok) {
                        ++g_barrel_snaps;
                        if (f.barrel_blend >= 0.999f)
                            g_barrel_miss_max = std::fmax(g_barrel_miss_max, std::sqrt((dw[0] - barrel_p[0]) * (dw[0] - barrel_p[0]) +
                                                                                       (dw[1] - barrel_p[1]) * (dw[1] - barrel_p[1]) +
                                                                                       (dw[2] - barrel_p[2]) * (dw[2] - barrel_p[2])));
                    }
                    if (prev_frame != g_body_frame) {
                        if (barrel_ok && prev_frame + 1 == g_body_frame) {
                            float tt[3];
                            to_set(f.ik_t[fh], tt, true);
                            const float gap = std::sqrt((barrel_p[0] - tt[0]) * (barrel_p[0] - tt[0]) + (barrel_p[1] - tt[1]) * (barrel_p[1] - tt[1]) +
                                                        (barrel_p[2] - tt[2]) * (barrel_p[2] - tt[2]));
                            g_barrel_jump_max = std::fmax(g_barrel_jump_max, std::fabs(f.barrel_blend - prev_w) * gap);
                        }
                        prev_w = f.barrel_blend;
                        prev_frame = g_body_frame;
                    }
                }
                if (h == fh && snap_ok) {
                    const float* dw = g_ik_diag[fh] + 16;
                    g_snap_miss = std::sqrt((dw[0] - fore_p[0]) * (dw[0] - fore_p[0]) + (dw[1] - fore_p[1]) * (dw[1] - fore_p[1]) +
                                            (dw[2] - fore_p[2]) * (dw[2] - fore_p[2]));
                    if (f.two_blend >= 0.999f) g_two_miss_max = std::fmax(g_two_miss_max, g_snap_miss);
                    g_snaps.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    // what the game holds in each hand, as drawn: the wrist's correction, or left-handed the transplant onto John's
    // gun hand (make_transplant, above the IK)
    Corr item[2] = {{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}}, {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}}};
    bool xfer_ok = false;
    for (int g = 0; g < 2; ++g)
        if (g_rig.wrist[g] >= 0 && g_rig.wrist[g] < n) item[g] = g_corr[g_rig.wrist[g]];
    if (f.xfer && f.item_side >= 0 && (f.ik[0] || f.ik[1])) {
        Corr t;
        if (make_transplant(t, slot == 0)) {
            item[f.item_side] = t;
            xfer_ok = true;
            if (slot == 0) g_xfers.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // the two-handed measurement ("skel two"): the game's animated left wrist and palm (this set, before any
    // correction) in the held gun's frame, against the game's own gun matrix and the placed one
    if (slot == 0 && f.held_any && g_rig.wrist[0] >= 0 && g_rig.wrist[0] < n && g_rig.elbow[0] >= 0) {
        const HeldSample* hs = newest_held(f.held);
        if (hs) {
            float L[3], Ol[9], fb[3];
            jdrawn(g_rig.wrist[0], L);
            ortho_cols(s + g_rig.wrist[0] * 12, Ol);
            const float* be = &g_rig.jbind[static_cast<size_t>(g_rig.elbow[0]) * 3];
            const float* bw = &g_rig.jbind[static_cast<size_t>(g_rig.wrist[0]) * 3];
            for (int k = 0; k < 3; ++k) fb[k] = bw[k] - be[k];
            norm(fb);
            float Lw[3], Pw[3];
            for (int k = 0; k < 3; ++k) {
                Lw[k] = L[k] + off[k];
                Pw[k] = Lw[k] + (Ol[k * 3] * fb[0] + Ol[k * 3 + 1] * fb[1] + Ol[k * 3 + 2] * fb[2]) * 0.08f;
            }
            auto in_gun = [&](const float* g, const float* x, float* r) {  // the gun's axes: its rows a b c
                const float d[3] = {x[0] - g[12], x[1] - g[13], x[2] - g[14]};
                for (int i = 0; i < 3; ++i) {
                    const float* row = g + i * 4;
                    const float l = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                    r[i] = l > 1e-6f ? (row[0] * d[0] + row[1] * d[1] + row[2] * d[2]) / l : 0.0f;
                }
            };
            TwoDiag& t = g_two_diag_m;
            in_gun(hs->game, Lw, t.wrist_game);
            in_gun(hs->game, Pw, t.palm_game);
            in_gun(hs->m, Lw, t.wrist_placed);
            in_gun(hs->m, Pw, t.palm_placed);
            std::memcpy(t.ik, hs->ik, sizeof(t.ik));
            std::memcpy(t.ik_hold, hs->ik_hold, sizeof(t.ik_hold));
            t.aiming = hs->aiming;
            t.placed = hs->placed;
            t.frame = g_body_frame;
        }
    }
    // run 12 item 2, [Holsters] OwnBelt: the belt's pieces moved and turned (both halves), before the holster points
    float piece_want[body::kPieces][3] = {}, piece_piv[body::kPieces][3] = {}, piece_R[9] = {};
    bool piece_ok[body::kPieces] = {};
    const bool pieces_ok = g_own_pieces.load(std::memory_order_relaxed) && apply_pieces(n, s, g_rig.jbind.data(), g_corr, piece_want, piece_piv, piece_ok, piece_R);
    if (slot == 0) {  // (the pelvis's frame kept, the pieces on or off: the linked rings' moves with the belt turned off)
        float pr[9];
        const bool pok = pieces_ok ? (std::memcpy(pr, piece_R, sizeof(pr)), true) : pelvis_R_of(n, s, g_corr, pr);
        if (pok) {  // (transform_set runs under g_draw_mutex: its caller's lock)
            std::memcpy(g_pelvis_R, pr, sizeof(g_pelvis_R));
            g_pelvis_R_frame = g_body_frame;
            g_pelvis_R_ok = true;
        }
    }
    if (pieces_ok && (slot == 0 || slot == 1)) {  // [Holsters] OwnBeltRigid: the joints for this set's geometry draws (piece_rigid_draw)
        if (slot == 0) {
            t_pc_s = s;
            t_pc_frame = g_body_frame;
        }
        for (int i = 0; i < body::kPieces; ++i) {
            const body::Piece& p = g_piece[i];
            if (slot == 0) t_pc_moved[i] = piece_ok[i] && (p.d[0] || p.d[1] || p.d[2] || p.turn[0] || p.turn[1] || p.turn[2]);
            std::memcpy(t_pc_piv[slot][i], piece_piv[i], sizeof(t_pc_piv[slot][i]));
            std::memcpy(t_pc_want[slot][i], piece_want[i], sizeof(t_pc_want[slot][i]));
        }
        if (slot == 0) {
            uint32_t mk = 0;
            for (int i = 0; i < body::kPieces; ++i)
                if (t_pc_moved[i]) mk |= 1u << i;
            g_pc_moved_mask.store(mk, std::memory_order_relaxed);
        }
    } else if (slot == 0) {
        t_pc_s = nullptr;
        g_pc_moved_mask.store(0, std::memory_order_relaxed);
    }
    if (pieces_ok && slot == 0) {
        for (int i = 0; i < body::kPieces; ++i) {
            PieceDiag& d = g_piece_diag[i];
            d.ok = piece_ok[i];
            if (!piece_ok[i]) continue;
            for (int k = 0; k < 3; ++k) {
                d.want[k] = piece_want[i][k] + off[k];  // (the set is the world less off)
                d.piv[k] = piece_piv[i][k] + off[k];
            }
            std::memcpy(d.R, piece_R, sizeof(d.R));
        }
        g_piece_sets.fetch_add(1, std::memory_order_relaxed);
    }
    // the holster bones and the root as drawn (corrected), before the apply step below rewrites the set (a hidden
    // bone's matrix is then its collapse point): for the holsters (body_points)
    float holster_pt[body::kHolsterBones][3] = {}, root_corr[3] = {};
    bool holster_ok[body::kHolsterBones] = {};
    if (slot == 0) {
        for (int i = 0; i < body::kHolsterBones; ++i) {
            const int hb = g_rig.holster[i];
            holster_ok[i] = hb >= 0 && hb < n;
            if (!holster_ok[i]) continue;
            float j[3];
            jdrawn(hb, j);
            corr_point(g_corr[hb], j, holster_pt[i]);
        }
        float j[3];
        jdrawn(0, j);
        corr_point(g_corr[0], j, root_corr);
    }
    // apply; the head bones collapse to the head joint as the head bone's correction puts it
    float ph[3], phc[3];
    jdrawn(g_rig.head, ph);
    corr_point(g_corr[g_rig.head], ph, phc);
    // Body shown (round 6): forearms and hands, or the hands only. A hidden bone of an arm (its clavicle's subtree)
    // collapses to that side's first shown joint (the elbow or the wrist, where it is drawn now), so the vertices it
    // shares with the shown part pull to the cut instead of stretching; every other hidden bone (the root too) to the
    // midpoint of the two cuts (round 6c: to the root joint, a vertex of the torso and an arm sat on a line down from
    // the elbow and drew a sliver). All hidden points lie on the one line between the cuts, so a mix of them has no
    // area, but for three terms per vertex (round 7: "a slight sliver from elbow to elbow"): the collapsed matrix's
    // own scale (a hidden vertex is the targets' mix plus scale x A M v, v about 1.4 m from the model's origin: at the
    // old 1e-4, 0.1 mm or so across the line; now [Body] CollapseScale, 1e-7: under the float step), the character
    // shaders' cloth flutter (they normalise the skinned normal, undoing the scale, and push the vertex along it by up
    // to 0.7 x the wind: held at no wind for these draws, [Body] HiddenNoFlutter), and blend weights that do not add
    // up to one (unverified).
    float side_pt[2][3] = {}, root_pt[3];
    jdrawn(0, root_pt);
    if (f.show) {
        bool have[2] = {false, false};
        for (int h = 0; h < 2; ++h) {
            int cut = f.show == 1 ? g_rig.elbow[h] : g_rig.wrist[h];
            if (cut < 0 || cut >= n) continue;
            float j[3];
            jdrawn(cut, j);
            corr_point(g_corr[cut], j, side_pt[h]);
            have[h] = true;
        }
        if (have[0] && have[1])
            for (int k = 0; k < 3; ++k) root_pt[k] = 0.5f * (side_pt[0][k] + side_pt[1][k]);
        if (slot == 0 && have[0] && have[1]) {  // "skel cuts" (the caller holds g_draw_mutex)
            for (int k = 0; k < 3; ++k) {
                g_cuts[0][k] = side_pt[0][k] + off[k];
                g_cuts[1][k] = side_pt[1][k] + off[k];
                g_cuts[2][k] = root_pt[k] + off[k];
            }
            g_cuts_ok = true;
        }
    }
    const float eps = f.collapse;
    for (int b = (f.show && f.collapse_root) ? 0 : 1; b < n; ++b) {
        float* m = s + b * 12;
        const Corr& c = g_corr[b];  // the root's is the identity
        const float mb[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
        float nm[9];
        mul3(c.A, mb, nm);
        const float* target = phc;
        bool collapse = f.hide && g_rig.in_head[b];
        if (f.show && !(f.show == 1 ? g_rig.in_elbow[b] : g_rig.in_wrist[b])) {
            collapse = true;
            target = g_rig.side[b] >= 0 ? side_pt[g_rig.side[b]] : root_pt;
        }
        float t[3] = {m[3], m[7], m[11]};
        for (int k = 0; k < 3; ++k) {
            const float sc = collapse ? eps : 1.0f;
            m[k * 4 + 0] = nm[k * 3 + 0] * sc;
            m[k * 4 + 1] = nm[k * 3 + 1] * sc;
            m[k * 4 + 2] = nm[k * 3 + 2] * sc;
            m[k * 4 + 3] = collapse ? target[k] : c.A[k * 3] * t[0] + c.A[k * 3 + 1] * t[1] + c.A[k * 3 + 2] * t[2] + c.a[k];
        }
    }
    // the fingers while transplanting: the gun hand takes the gripping hand's curl, the other the game's empty hand's
    // (or the bind pose's, open): each finger's matrix relative to its wrist, conjugated by the rest pose's mirror
    // (M' = Q M Q, a rotation; t' = Q (M m + t) + m, with Q = I - 2 n n^T and m = 2 c n). [Hands] CopyGrip: while the
    // copy is out, the hand holding it takes the gripping hand's curl the same way (without the transplant, the free
    // hand; with it, the gripping hand keeps its own)
    // (2026-10-30, round 55, the user: "In left handed mode, grabbed ammo is in middle of hand, give it a mirrored position of
    // right handed mode") the transplant: the game holds the sidearm in John's right, so his left is the game's free hand,
    // in its pinch; learned here as the right-handed layout learns it (below), before the transplant's fingers replace it,
    // for the round held in John's right (mirrored there, below)
    if (slot == 0 && xfer_ok && f.item_side == 1 && !f.long_gun && f.weapon >= 0 && f.weapon <= 7) {
        const FingerRel live = finger_rel(s, n, 0);
        if (live.valid && live.has[2] && live.has[5]) {
            float g2 = 0.0f;
            for (int k = 0; k < 3; ++k) g2 += (live.t[2][k] - live.t[5][k]) * (live.t[2][k] - live.t[5][k]);
            if (g2 < 0.035f * 0.035f) {
                if (!g_round_fingers[0].valid) g_round_pinch_learned.fetch_add(1, std::memory_order_relaxed);
                g_round_fingers[0] = live;
            }
        }
    }
    const bool xf_fingers = xfer_ok && f.fingers;
    if (g_rig.mirror_ok && (xf_fingers || f.copy_grip)) {
        struct Rg {
            float M[9], t[3];
        };
        auto get = [&](int b, Rg& r) {
            const float* m = s + b * 12;
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) r.M[i * 3 + k] = m[i * 4 + k];
                r.t[i] = m[i * 4 + 3];
            }
        };
        auto put = [&](int b, const Rg& r) {
            float* m = s + b * 12;
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) m[i * 4 + k] = r.M[i * 3 + k];
                m[i * 4 + 3] = r.t[i];
            }
        };
        auto compose = [](const Rg& a, const Rg& b, Rg& o) {  // a after b
            mul3(a.M, b.M, o.M);
            for (int k = 0; k < 3; ++k) o.t[k] = a.M[k * 3] * b.t[0] + a.M[k * 3 + 1] * b.t[1] + a.M[k * 3 + 2] * b.t[2] + a.t[k];
        };
        auto inverse = [](const Rg& a, Rg& o) {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) o.M[i * 3 + j] = a.M[j * 3 + i];
            for (int k = 0; k < 3; ++k) o.t[k] = -(o.M[k * 3] * a.t[0] + o.M[k * 3 + 1] * a.t[1] + o.M[k * 3 + 2] * a.t[2]);
        };
        const float* nn = g_rig.mir_n;
        float Q[9], mm[3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) Q[i * 3 + j] = (i == j ? 1.0f : 0.0f) - 2.0f * nn[i] * nn[j];
            mm[i] = 2.0f * g_rig.mir_c * nn[i];
        }
        auto mirrored = [&](const Rg& x, Rg& o) {
            float t1[9];
            mul3(Q, x.M, t1);
            mul3(t1, Q, o.M);
            float v[3];
            for (int k = 0; k < 3; ++k) v[k] = x.M[k * 3] * mm[0] + x.M[k * 3 + 1] * mm[1] + x.M[k * 3 + 2] * mm[2] + x.t[k];
            for (int k = 0; k < 3; ++k) o.t[k] = Q[k * 3] * v[0] + Q[k * 3 + 1] * v[1] + Q[k * 3 + 2] * v[2] + mm[k];
        };
        const int g = f.item_side, d = xf_fingers ? f.gun_j : 1 - f.gun_j;
        // (2026-11-01, GOAL-offhand-changes C34; the user: a bolt action held by the left hand, the right hand plays the bolt-working
        // animation after a shot) the game's empty hand works the action in John's fire clip (the game's own cycle stopped for
        // the mod's): through that clip the free hand keeps the fingers it had before the shot (kept from slot 0's frames)
        static Rg s_free[15];
        static bool s_free_ok = false;
        const bool hold_free = xf_fingers && !f.copy_grip && f.long_gun && (worked_flags() & kStopFireClip) && in_fire_clip();
        // (the review) kept only from this transplant's frames: one begun inside a fire clip (a shot one-handed, then the gun
        // into the left hand) has none, and takes the game's
        static uint64_t s_free_frame = 0;
        if (slot == 0) {
            if (s_free_frame + 1 < g_body_frame || !xf_fingers || f.copy_grip) s_free_ok = false;
            s_free_frame = g_body_frame;
        }
        bool ok = g_rig.wrist[0] >= 0 && g_rig.wrist[1] >= 0 && g_rig.wrist[0] < n && g_rig.wrist[1] < n && g >= 0 && g < 2 && d >= 0 && d < 2 &&
                  d != g;
        Rg w[2], wi[2], rel[2][15];
        for (int h = 0; ok && h < 2; ++h) {
            get(g_rig.wrist[h], w[h]);
            const float cl = std::sqrt(w[h].M[0] * w[h].M[0] + w[h].M[3] * w[h].M[3] + w[h].M[6] * w[h].M[6]);
            if (cl < 0.5f) ok = false;  // a collapsed (hidden) hand
            inverse(w[h], wi[h]);
            for (int i = 0; ok && i < 15; ++i) {
                const int fb = g_rig.finger[h][i];
                if (fb < 0 || fb >= n) continue;
                Rg fm;
                get(fb, fm);
                compose(wi[h], fm, rel[h][i]);
            }
        }
        for (int i = 0; ok && i < 15; ++i) {
            const int fd = g_rig.finger[d][i], fg = g_rig.finger[g][i];
            if (fd < 0 || fg < 0 || fd >= n || fg >= n) continue;
            Rg mrel, o;
            mirrored(rel[g][i], mrel);  // the gripping hand's curl onto the gun hand
            // (2026-11-01, GOAL-offhand-changes C3c; round 66 check 8, the user: each time the gun in hand fires the off hand's
            // thumb touches its hammer) [Hands] CopyGrip: through the gun in hand's fire clip the game's own thumb re-cocks
            // its hammer, and the copy's hand took it mirrored: its thumb kept as before the shot meanwhile
            if (f.copy_grip && !xf_fingers && i < 3) {
                static Rg s_cthumb[3];
                static bool s_cthumb_ok = false;
                static uint64_t s_cthumb_frame = 0;
                if (slot == 0 && i == 0) {  // (the review: kept only from this grip's frames, as s_free)
                    if (s_cthumb_frame + 1 < g_body_frame) s_cthumb_ok = false;
                    s_cthumb_frame = g_body_frame;
                }
                if (in_fire_clip()) {
                    if (s_cthumb_ok) mrel = s_cthumb[i];
                } else if (slot == 0) {
                    s_cthumb[i] = mrel;
                    if (i == 2) s_cthumb_ok = true;
                }
            }
            compose(w[d], mrel, o);
            put(fd, o);
            if (!xf_fingers || f.copy_grip) continue;  // the copy: the gripping hand keeps its own grip
            if (f.fingers == 2) {
                put(fg, w[g]);  // open: the bind pose's fingers
            } else {
                mirrored(rel[d][i], mrel);  // the game's empty hand's pose onto the other hand
                if (hold_free && s_free_ok) mrel = s_free[i];  // (C34) as before the shot
                else if (slot == 0 && !hold_free) {
                    s_free[i] = mrel;
                    if (i == 0) s_free_ok = true;  // (the review: set with the store)
                }
                compose(w[g], mrel, o);
                put(fg, o);
            }
        }
    }
    // the fingers fr (relative to the wrist) on John's hand `hand`, from its live ones by b (1: fr's exactly); false
    // without that hand's fingers in this set (a collapsed, hidden, hand). (2026-10-28: either hand; John's left before)
    auto put_fingers = [&](int hand, const FingerRel* fr, float b) {
        const int wb = hand >= 0 && hand < 2 ? g_rig.wrist[hand] : -1;
        if (wb < 0 || wb >= n) return false;
        const FingerRel live = finger_rel(s, n, hand);
        float* w = s + wb * 12;
        float WR[9], wt[3];
        for (int i = 0; i < 3; ++i) {
            for (int k = 0; k < 3; ++k) WR[i * 3 + k] = w[i * 4 + k];
            wt[i] = w[i * 4 + 3];
        }
        // (2026-11-02, GOAL-offhand-changes C3c, C34b; round 67 checks 2 and 7, the user's pictures: the second gun's thumb and
        // the fingers on a bolt drawn stretched far out of the hand) the pose's turns only: each joint's drawn point (a skinning
        // matrix: the joint at M jbind + t) at its parent's along the hand's own live bone, turned as its parent is drawn (a pose
        // learned with its bones stretched, the thumb's IK, a mirrored hold, no longer stretches the drawn hand); a finger's
        // first joint where the hand has it (the review: t is no joint's place, the point M jb + t is)
        float pM[9] = {}, pq[3] = {}, plM[9] = {}, pql[3] = {};
        bool chain = false;
        for (int i = 0; live.valid && i < 15; ++i) {
            const int fb = g_rig.finger[hand][i];
            const bool first = i == 0 || i % 3 == 0;  // (thumb 0-2, then each finger's three)
            if (fb < 0 || fb >= n || !fr->has[i] || !live.has[i] || static_cast<size_t>(fb) * 3 + 2 >= g_rig.jbind.size()) {
                chain = false;
                continue;
            }
            const float* jb = &g_rig.jbind[static_cast<size_t>(fb) * 3];
            float M[9], t[3];
            if (b >= 1.0f) {
                std::memcpy(M, fr->M[i], sizeof(M));
            } else {  // from the live finger toward the hold's: its turn by the blend
                float lt[9], d[9], ax[3], ang = 0.0f, rp[9];
                for (int a = 0; a < 3; ++a)
                    for (int c = 0; c < 3; ++c) lt[a * 3 + c] = live.M[i][c * 3 + a];
                mul3(fr->M[i], lt, d);
                to_axis_angle(d, ax, &ang);
                axis_angle(ax, ang * b, rp);
                mul3(rp, live.M[i], M);
            }
            float ql[3], qn[3];  // the live joint's point, the new one (the wrist's frame)
            for (int k = 0; k < 3; ++k) ql[k] = live.M[i][k * 3] * jb[0] + live.M[i][k * 3 + 1] * jb[1] + live.M[i][k * 3 + 2] * jb[2] + live.t[i][k];
            if (first || !chain) {
                std::memcpy(qn, ql, sizeof(qn));
            } else {  // the live bone from its parent's joint, in the parent's live frame, put on the parent as drawn now
                const float dl[3] = {ql[0] - pql[0], ql[1] - pql[1], ql[2] - pql[2]};
                float lo[3];
                for (int k = 0; k < 3; ++k) lo[k] = plM[0 * 3 + k] * dl[0] + plM[1 * 3 + k] * dl[1] + plM[2 * 3 + k] * dl[2];
                for (int k = 0; k < 3; ++k) qn[k] = pq[k] + pM[k * 3] * lo[0] + pM[k * 3 + 1] * lo[1] + pM[k * 3 + 2] * lo[2];
            }
            for (int k = 0; k < 3; ++k) t[k] = qn[k] - (M[k * 3] * jb[0] + M[k * 3 + 1] * jb[1] + M[k * 3 + 2] * jb[2]);
            std::memcpy(pM, M, sizeof(pM));
            std::memcpy(pq, qn, sizeof(pq));
            std::memcpy(plM, live.M[i], sizeof(plM));
            std::memcpy(pql, ql, sizeof(pql));
            chain = true;
            float* m = s + fb * 12;  // F = W rel
            for (int a = 0; a < 3; ++a) {
                for (int c = 0; c < 3; ++c) m[a * 4 + c] = WR[a * 3 + 0] * M[0 * 3 + c] + WR[a * 3 + 1] * M[1 * 3 + c] + WR[a * 3 + 2] * M[2 * 3 + c];
                m[a * 4 + 3] = WR[a * 3 + 0] * t[0] + WR[a * 3 + 1] * t[1] + WR[a * 3 + 2] * t[2] + wt[a];
            }
        }
        return live.valid;
    };
    {  // [Weapon.<Gun>] FrontHandPose: a fixed hold's own fingers on the front hand (John's left; not with the gun in his
       // left, the transplant), blended from the game's by the two-handed hold
        const bool mir_now = f.mirror && f.xfer && f.gun_j == 0 && f.item_side == 1;
        const int fp = f.weapon >= 0 && f.weapon < kGripW ? g_front_pose[f.weapon].load(std::memory_order_relaxed) : 0;
        const FingerRel* fr = fp > 0 ? &g_front_fingers[f.weapon][fp - 1] : nullptr;
        const float b = f.two_blend > 1.0f ? 1.0f : f.two_blend;
        const int wb = g_rig.wrist[0];
        const int wb1 = g_rig.wrist[1];
        if (wb >= 0 && wb < n && f.carry_j == 0 && g_carry_fingers.valid) {
            // [Hands] CarryByForegrip: the carrying hand keeps the fingers it had on the foregrip (the game's unarmed ones
            // are open: the user would see the gun float under an open hand)
            if (put_fingers(0, &g_carry_fingers, 1.0f) && slot == 0) g_carry_finger_frames.fetch_add(1, std::memory_order_relaxed);
        } else if (wb1 >= 0 && wb1 < n && f.carry_j == 1 && g_carry_fingers.valid && g_rig.mirror_ok) {
            // (GOAL-offhand-changes C16) John's right carrying (a gun drawn into his left): those fingers mirrored
            const FingerRel cmr = mirrored_rel(g_carry_fingers);
            if (put_fingers(1, &cmr, 1.0f) && slot == 0) g_carry_finger_frames.fetch_add(1, std::memory_order_relaxed);
        } else if (fr && fr->valid && f.long_gun && !mir_now && !f.xfer && f.gun_j == 1 && b > 0.0f && wb >= 0 && wb < n) {
            // the hold's fingers (fr) on John's left hand's, from the live ones by b (1: the hold's exactly)
            if (put_fingers(0, fr, b) && slot == 0) g_front_finger_frames.fetch_add(1, std::memory_order_relaxed);
        }
        // the fingers as drawn on the foregrip, fully two-handed (the hold's or the game's own), for a hand-over
        if (wb >= 0 && wb < n && f.carry_j < 0 && f.long_gun && !mir_now && !f.xfer && f.gun_j == 1 && f.two_blend >= 0.999f && slot == 0) {
            const FingerRel now = finger_rel(s, n, 0);
            if (now.valid) g_carry_fingers = now;
        } else if (wb1 >= 0 && wb1 < n && f.carry_j < 0 && f.long_gun && f.xfer && xfer_ok && f.gun_j == 0 && f.two_blend >= 0.999f && slot == 0 &&
                   g_rig.mirror_ok) {
            // (GOAL-offhand-changes C16, the review) the transplant: John's right on the foregrip, kept in the left's form
            // (mirrored back), for his right's carry (mirrored again there)
            const FingerRel now = finger_rel(s, n, 1);
            if (now.valid) g_carry_fingers = mirrored_rel(now);
        }
    }
    {  // (cycle r171) the round's pinch on the free hand of a long gun (g_round_fingers)
        const int fh = 1 - f.gun_j, rh = f.round_hand;  // (the review: once a draw, so slot 0 and slot 1 agree)
        if (slot == 0) g_round_flat[0] = g_round_flat[1] = g_round_pinch_put[0] = g_round_pinch_put[1] = false;
        if (fh >= 0 && fh < 2) {
            // a sidearm in hand: its free hand's pinch learned (2026-10-27: not while that hand holds a second gun, its
            // grip's tips no pinch; round 55: not transplanting, learned above then)
            if (slot == 0 && !f.xfer && !f.long_gun && f.weapon >= 0 && f.weapon <= 7 && f.sec_j != fh) {
                const FingerRel live = finger_rel(s, n, fh);
                if (live.valid && live.has[2] && live.has[5]) {
                    float g2 = 0.0f;
                    for (int k = 0; k < 3; ++k) g2 += (live.t[2][k] - live.t[5][k]) * (live.t[2][k] - live.t[5][k]);
                    if (g2 < 0.035f * 0.035f) {
                        if (!g_round_fingers[fh].valid) g_round_pinch_learned.fetch_add(1, std::memory_order_relaxed);
                        g_round_fingers[fh] = live;
                    }
                }
            }
            const int wbh = g_rig.wrist[fh];
            // (2026-10-27, [Hands] DualWieldCrossReload: not on the hand holding the second gun, which may hold the gun in
            // hand's rounds now: it keeps its gun's grip, the round drawn at that grip's thumb and index tips)
            // (round 55) transplanting (the left-handed layout), any gun: the free hand was given the game's empty hand, open
            FingerRel mir_pinch;
            const FingerRel* pinch = g_round_fingers[fh].valid ? &g_round_fingers[fh] : nullptr;
            if (xfer_ok && !pinch && g_rig.mirror_ok && g_round_fingers[1 - fh].valid) {
                mir_pinch = mirrored_rel(g_round_fingers[1 - fh]);
                pinch = &mir_pinch;
            }
            // (the review: transplanting, only once a pinch is known: else the transplanted tips as before, never the flat path)
            if (rh == fh && ((f.long_gun && (!f.xfer || xfer_ok)) || (xfer_ok && pinch)) && f.carry_j != fh && f.sec_j != fh && f.two_blend < 0.5f) {
                bool put_ok = false;
                if (pinch && pinch->valid && wbh >= 0 && wbh < n) {  // the pinch's fingers, F = W rel (as put_fingers)
                    const FingerRel& fr = *pinch;
                    const float* w = s + wbh * 12;
                    float WR[9], wt[3];
                    for (int i = 0; i < 3; ++i) {
                        for (int k = 0; k < 3; ++k) WR[i * 3 + k] = w[i * 4 + k];
                        wt[i] = w[i * 4 + 3];
                    }
                    const float cl = std::sqrt(WR[0] * WR[0] + WR[3] * WR[3] + WR[6] * WR[6]);
                    for (int i = 0; cl > 0.5f && i < 15; ++i) {  // (not on a collapsed, hidden, hand)
                        const int fb = g_rig.finger[fh][i];
                        if (fb < 0 || fb >= n || !fr.has[i]) continue;
                        float* m = s + fb * 12;
                        for (int a = 0; a < 3; ++a) {
                            for (int c = 0; c < 3; ++c)
                                m[a * 4 + c] = WR[a * 3 + 0] * fr.M[i][0 * 3 + c] + WR[a * 3 + 1] * fr.M[i][1 * 3 + c] + WR[a * 3 + 2] * fr.M[i][2 * 3 + c];
                            m[a * 4 + 3] = WR[a * 3 + 0] * fr.t[i][0] + WR[a * 3 + 1] * fr.t[i][1] + WR[a * 3 + 2] * fr.t[i][2] + wt[a];
                        }
                        put_ok = true;
                    }
                }
                if (slot == 0) {
                    g_round_pinch_put[fh] = put_ok;
                    g_round_flat[fh] = !(pinch && pinch->valid);  // (the review: no pinch learned, not a hidden hand's frame)
                    if (put_ok) g_round_pinch_frames.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    {  // (2026-10-28) [Reload] PartHandSnap: the hand on a bolt closes on it, by its weight. (r228: the learned pinch first,
       // its fingers flat, opened the hand on the knob: index and middle 7.5 degrees against the game's own 48) The game's
       // own hand closed on the gun first: the foregrip's fingers (g_carry_fingers, John's left; mirrored for his right),
       // the knob in the fist; only without them that hand's pinch (else the other hand's mirrored)
        int src = 0;
        if (grasp_h >= 0 && grasp_b > 0.0f) {
            FingerRel mir, tight;
            const FingerRel* fr = grasp_pose(grasp_h, &mir, &src);
            // (round 40) the foregrip's fingers closed further round the bolt's knob (curled(); 1: as learned)
            if (const float ck = g_grasp_curl_k.load(std::memory_order_relaxed); fr && std::fabs(ck - 1.0f) > 1e-3f) {
                tight = curled(*fr, grasp_h, ck, 1.75f);
                fr = &tight;
            }
            const bool put = g_grasp_cfg.load(std::memory_order_relaxed);  // ("skel grasp off": the test's control)
            // ("skel grasp open|pinch", the session, a test's sensitivity control: another pose put instead, the measures still
            // against the one chosen above; open: the bind pose's fingers, each on its wrist as at rest)
            const int force = g_grasp_force.load(std::memory_order_relaxed);
            const FingerRel* put_fr = fr;
            FingerRel alt;
            if (fr && force == 1) {
                alt.valid = true;
                for (int i = 0; i < 15; ++i) {
                    alt.has[i] = true;
                    for (int k = 0; k < 9; ++k) alt.M[i][k] = k % 4 == 0 ? 1.0f : 0.0f;
                }
                put_fr = &alt;
            } else if (fr && force == 2) {
                if (g_round_fingers[grasp_h].valid) put_fr = &g_round_fingers[grasp_h];
                else if (g_rig.mirror_ok && g_round_fingers[1 - grasp_h].valid) alt = mirrored_rel(g_round_fingers[1 - grasp_h]), put_fr = &alt;
                else put_fr = nullptr;  // (no pinch learned: nothing put, the control reads the game's own)
            }
            if (put_fr && put && put_fingers(grasp_h, put_fr, grasp_b) && slot == 0) ++g_grasp_frames;
            if (fr && slot == 0) {  // "skel partsnap": the index and middle fingers' curl as drawn, the drawn fingers against the
                                    // pose (mean degrees), the pose's own index and middle curl
                const FingerRel live = finger_rel(s, n, grasp_h);
                auto turn_of = [](const float* M) {
                    return std::acos(std::clamp((M[0] + M[4] + M[8] - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.29578f;
                };
                float cl = 0.0f, cs = 0.0f, er = 0.0f;
                int nc = 0, ne = 0;
                for (int i = 0; live.valid && i < 15; ++i) {
                    if (!live.has[i] || !fr->has[i]) continue;
                    float Mt[9], D[9];
                    for (int a = 0; a < 3; ++a)
                        for (int c = 0; c < 3; ++c) Mt[a * 3 + c] = fr->M[i][c * 3 + a];
                    mul3(live.M[i], Mt, D);
                    er += turn_of(D);
                    ++ne;
                    if (i >= 3 && i <= 8) {  // finger_11 - 13, finger_21 - 23
                        cl += turn_of(live.M[i]);
                        cs += turn_of(fr->M[i]);
                        ++nc;
                    }
                }
                g_grasp_curl = nc ? cl / nc : -1.0f;
                g_grasp_src_curl = nc ? cs / nc : -1.0f;
                g_grasp_err = ne ? er / ne : -1.0f;
            }
        }
        if (slot == 0) {
            g_grasp_src = src;
            g_grasp_hand = grasp_h;
            if (!src) g_grasp_curl = g_grasp_src_curl = g_grasp_err = -1.0f;
        }
    }
    {  // [Reload] ThumbCock (2026-10-28): the gun hand's thumb onto the single action's hammer spur through the stick's
       // cock, by its weight. The spur on the drawn gun (the item's sample through held_delta's correction on this set, as
       // its draws take it) at the hammer's drawn turn (its kGunModels row's); the thumb's tip (past thumb_03's joint along
       // that bone) turned toward it about thumb_02, then thumb_01, a few rounds (each joint's bones about it: M' = R M,
       // t' = R (t - p) + p), each joint's turn capped. After every other finger pose (the transplant's too: its gun hand)
        const int tj = f.gun_j;
        // (the review) the gun in hand's own sample: the newest neither a second gun's nor the carried gun's put-away, on the
        // game's hand that holds the item; none while transplanting without the transplant made
        // (as newest_held: the latest build's sample or none, never an older one)
        const HeldSample* th = f.thumb_w >= 0 && !(f.xfer && !xfer_ok) ? newest_held(f.held) : nullptr;
        if (th && (th->sec || th->follow_j >= 0 || (f.item_side >= 0 && (th->left ? 0 : 1) != f.item_side))) th = nullptr;
        const GunPart* hp = th ? hammer_part(f.thumb_w) : nullptr;
        const int ss = spur_slot(f.thumb_w);
        const int it_side = th ? (th->left ? 0 : 1) : -1;
        int tb[3] = {-1, -1, -1};
        bool bones_ok = tj >= 0 && tj < 2 && g_rig.wrist[tj] >= 0 && g_rig.wrist[tj] < n;
        for (int i = 0; bones_ok && i < 3; ++i) {
            tb[i] = g_rig.finger[tj][i];
            bones_ok = tb[i] >= 0 && tb[i] < n;
        }
        const float* wm = bones_ok ? s + g_rig.wrist[tj] * 12 : nullptr;
        bool done = false;
        // (2026-10-30, round 55, the user: "After firing revolver, Johns thumb touches the top of the hammer, need his thumb to
        // remain still until thumb stick is used to cocking") the single action's thumb kept at its rest in the grip (its
        // three bones relative to the wrist, learned with the hammer cocked, no shot within kThumbRestMs and no cock under
        // way) from the shot until the hammer is cocked again: the game's fire clip moves it nowhere (r294: the tip 11 cm off
        // its rest for some 0.3 s as the hammer fell); the stick's cock below then takes it from there to the spur
        {
            constexpr double kThumbRestMs = 800.0;
            static FingerRel s_rest[2];
            static int s_rest_w[2] = {-1, -1};
            const double since = actions::since_shot_ms();
            if (bones_ok && f.thumb_w >= 0 && !(f.xfer && !xfer_ok)) {
                // (the review: the shot's draw can come before the frame end counts it: not while the fire clip runs)
                if (slot == 0 && f.thumb_wt <= 0.001f && f.thumb_hammer >= 0.999f && since > kThumbRestMs && holster::fire_clip_phase() < 0.0f) {
                    FingerRel r = finger_rel(s, n, tj);
                    if (r.valid && r.has[0] && r.has[1] && r.has[2]) {
                        for (int i = 3; i < 15; ++i) r.has[i] = false;  // (the thumb's alone)
                        s_rest[tj] = r;
                        s_rest_w[tj] = f.thumb_w;
                    }
                }
                if (s_rest[tj].valid && s_rest_w[tj] == f.thumb_w && (f.thumb_hammer < 0.999f || since <= kThumbRestMs) &&
                    put_fingers(tj, &s_rest[tj], 1.0f) && slot == 0)
                    g_thumb_rest_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (th && !th->sec && hp && ss >= 0 && it_side >= 0 && bones_ok && f.ik[tj] && std::sqrt(wm[0] * wm[0] + wm[4] * wm[4] + wm[8] * wm[8]) > 0.5f) {
            // the drawn gun: the item's hand's correction now less its placement's (held_delta's) on the sample's matrix
            const Corr& nc = item[it_side];
            double ua[3];
            float ut[9], cA[9], ca[3], Gm[9], go[3], Gd[9], gd_o[3];
            for (int k = 0; k < 3; ++k)
                ua[k] = th->ad[k] - off[k] + (static_cast<double>(th->A[k * 3]) * off[0] + static_cast<double>(th->A[k * 3 + 1]) * off[1] +
                                              static_cast<double>(th->A[k * 3 + 2]) * off[2]);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) ut[i * 3 + j] = th->A[j * 3 + i];
            mul3(nc.A, ut, cA);
            for (int k = 0; k < 3; ++k)
                ca[k] = static_cast<float>(nc.a[k] - (static_cast<double>(cA[k * 3]) * ua[0] + static_cast<double>(cA[k * 3 + 1]) * ua[1] +
                                                      static_cast<double>(cA[k * 3 + 2]) * ua[2]));
            for (int c = 0; c < 3; ++c) {  // the sample's axes (its rows) as columns, its origin in the set's space
                const float* row = th->m + c * 4;
                float l = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                l = l > 1e-6f ? l : 1.0f;
                for (int k = 0; k < 3; ++k) Gm[k * 3 + c] = row[k] / l;
            }
            for (int k = 0; k < 3; ++k) go[k] = th->m[12 + k] - off[k];
            mul3(cA, Gm, Gd);
            for (int k = 0; k < 3; ++k) gd_o[k] = cA[k * 3] * go[0] + cA[k * 3 + 1] * go[1] + cA[k * 3 + 2] * go[2] + ca[k];
            // the spur: its offset from the hammer's pivot turned as the hammer is drawn (part_apply's turn), on the gun
            const float ha = hp->deg * f.thumb_hammer * 0.0174532925f, hc = std::cos(ha), hsn = std::sin(ha);
            float Q[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            if (hp->axis == 0) Q[4] = hc, Q[5] = -hsn, Q[7] = hsn, Q[8] = hc;
            else if (hp->axis == 1) Q[0] = hc, Q[2] = hsn, Q[6] = -hsn, Q[8] = hc;
            else Q[0] = hc, Q[1] = -hsn, Q[3] = hsn, Q[4] = hc;
            const float* sp = g_spur[ss];
            float sm[3], spur[3];
            for (int i = 0; i < 3; ++i) sm[i] = hp->pivot[i] + Q[i * 3] * sp[0] + Q[i * 3 + 1] * sp[1] + Q[i * 3 + 2] * sp[2];
            if (f.kick_on[0] && f.ik[tj]) {  // (r230) the gun as drawn: its kick and the cock's roll in the hand (kick_draw's root X)
                const float* X = f.kick_l[0];
                const float s0[3] = {sm[0], sm[1], sm[2]};
                for (int i = 0; i < 3; ++i) sm[i] = X[i * 4] * s0[0] + X[i * 4 + 1] * s0[1] + X[i * 4 + 2] * s0[2] + X[i * 4 + 3];
            }
            for (int k = 0; k < 3; ++k) spur[k] = gd_o[k] + Gd[k * 3] * sm[0] + Gd[k * 3 + 1] * sm[1] + Gd[k * 3 + 2] * sm[2];
            // the tip: thumb_03's joint plus the tip's length along its own bone (the bind's 02 -> 03 line turned by thumb_03's
            // matrix: r228, along the 02 -> 03 line drawn, a turn of thumb_03 moved nothing, so only two joints worked)
            float tdir[3];
            {
                const float* b1 = &g_rig.jbind[static_cast<size_t>(tb[1]) * 3];
                const float* b2 = &g_rig.jbind[static_cast<size_t>(tb[2]) * 3];
                for (int k = 0; k < 3; ++k) tdir[k] = b2[k] - b1[k];
                norm(tdir);
            }
            auto tip = [&](float* E) {
                float j2[3], d[3];
                jdrawn(tb[2], j2);
                const float* m = s + tb[2] * 12;
                for (int k = 0; k < 3; ++k) d[k] = m[k * 4] * tdir[0] + m[k * 4 + 1] * tdir[1] + m[k * 4 + 2] * tdir[2];
                norm(d);
                for (int k = 0; k < 3; ++k) E[k] = j2[k] + d[k] * g_thumb_tip;
            };
            float used[3] = {}, side_used = 0.0f, ext_held = 0.0f;  // (ext_held: round 40's limit, the backward turn refused)
            const float wt = f.thumb_wt < 0.0f ? 0.0f : f.thumb_wt > 1.0f ? 1.0f : f.thumb_wt;
            float E0[3];
            tip(E0);
            // (the review) nothing drawn from a non-finite gun, spur or thumb (the matrices' sums: any NaN or infinity shows)
            float fin = spur[0] + spur[1] + spur[2] + E0[0] + E0[1] + E0[2] + gd_o[0] + gd_o[1] + gd_o[2];
            for (int i = 0; i < 9; ++i) fin += Gd[i];
            for (int i = 0; i < 3; ++i)
                for (int c = 0; c < 12; ++c) fin += s[tb[i] * 12 + c];
            const bool finite = std::isfinite(fin);
            if (wt > 0.001f && finite) {
                float T[3];
                for (int k = 0; k < 3; ++k) T[k] = E0[k] + (spur[k] - E0[k]) * wt;  // from the thumb's own place by the weight
                // the turn each joint has left: thumb_01 45 degrees, thumb_02 70, thumb_03 80 (the last knuckle's bend)
                const float cap[3] = {0.785f, 1.22f, 1.40f};
                float left[3] = {cap[0], cap[1], cap[2]};
                // (the review) thumb_02 and thumb_03 bend mostly in the thumb's own bend plane, as the game draws it (the normal
                // of its two last bones; a straight thumb: no plane, no cap): each one's turn out of it at most 15 degrees
                float nb[3] = {0.0f, 0.0f, 0.0f}, side_left[3] = {0.0f, 0.26f, 0.26f};
                {
                    float j0[3], j1[3], j2[3], a[3], b[3];
                    jdrawn(tb[0], j0);
                    jdrawn(tb[1], j1);
                    jdrawn(tb[2], j2);
                    for (int k = 0; k < 3; ++k) {
                        a[k] = j1[k] - j0[k];
                        b[k] = j2[k] - j1[k];
                    }
                    cross(a, b, nb);
                    const float nl = std::sqrt(nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2]);
                    const float al = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), bl = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
                    if (nl > 0.05f * al * bl && nl > 1e-9f)
                        for (float& x : nb) x /= nl;
                    else
                        nb[0] = nb[1] = nb[2] = 0.0f;
                }
                float nbj[3][3] = {};  // each joint's plane (carried by the bones above it as they turn)
                for (int jj = 0; jj < 3; ++jj) std::memcpy(nbj[jj], nb, sizeof(nb));
                {  // (the review) thumb_03's own: its bone's line against thumb_02's, (j2 - j1) x d; the thumb's plane without a bend there
                    float j1[3], j2[3], E[3], a[3], d[3], n3[3];
                    jdrawn(tb[1], j1);
                    jdrawn(tb[2], j2);
                    tip(E);
                    for (int k = 0; k < 3; ++k) {
                        a[k] = j2[k] - j1[k];
                        d[k] = E[k] - j2[k];
                    }
                    cross(a, d, n3);
                    const float nl = std::sqrt(n3[0] * n3[0] + n3[1] * n3[1] + n3[2] * n3[2]);
                    const float al = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (nl > 0.05f * al * dl && nl > 1e-9f)
                        for (int k = 0; k < 3; ++k) nbj[2][k] = n3[k] / nl;
                }
                // (round 40, the user: "the tip of John's thumb bends backward") thumb_02 and thumb_03 straighten at most to
                // straight: each one's turn against its bend (about its plane's normal, negative) at most the bend it has, less
                // 3 degrees, plus what it bent further this frame. A straight joint (no plane) is not held
                float ext_left[3] = {0.0f, 0.0f, 0.0f};
                {
                    float j0[3], j1[3], j2[3], E[3], a[3], b[3], d[3];
                    jdrawn(tb[0], j0);
                    jdrawn(tb[1], j1);
                    jdrawn(tb[2], j2);
                    tip(E);
                    for (int k = 0; k < 3; ++k) {
                        a[k] = j1[k] - j0[k];
                        b[k] = j2[k] - j1[k];
                        d[k] = E[k] - j2[k];
                    }
                    auto bend = [](const float* u, const float* w) {
                        const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                        if (ul < 1e-6f || wl < 1e-6f) return 0.0f;
                        return std::acos(std::clamp((u[0] * w[0] + u[1] * w[1] + u[2] * w[2]) / (ul * wl), -1.0f, 1.0f));
                    };
                    ext_left[1] = std::fmax(0.0f, bend(a, b) - 0.052f);
                    ext_left[2] = std::fmax(0.0f, bend(b, d) - 0.052f);
                }
                for (int it = 0; it < 6; ++it)
                    for (int jj = 2; jj >= 0; --jj) {
                        float p[3], E[3], a0[3], b0[3], ax[3], R[9];
                        jdrawn(tb[jj], p);
                        tip(E);
                        for (int k = 0; k < 3; ++k) {
                            a0[k] = E[k] - p[k];
                            b0[k] = T[k] - p[k];
                        }
                        norm(a0);
                        norm(b0);
                        cross(a0, b0, ax);
                        const float sn = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                        if (sn < 1e-6f || left[jj] <= 0.0f) continue;
                        float ang = std::atan2(sn, a0[0] * b0[0] + a0[1] * b0[1] + a0[2] * b0[2]);
                        ang = ang > left[jj] ? left[jj] : ang;
                        left[jj] -= ang;
                        for (float& x : ax) x /= sn;
                        const float* nj = nbj[jj];
                        if (jj >= 1 && (nj[0] != 0.0f || nj[1] != 0.0f || nj[2] != 0.0f)) {  // the turn out of the bend plane capped
                            float v[3], vs[3];
                            float along = (ax[0] * nj[0] + ax[1] * nj[1] + ax[2] * nj[2]) * ang;
                            for (int k = 0; k < 3; ++k) {
                                v[k] = ax[k] * ang;
                                vs[k] = v[k] - along * nj[k];
                            }
                            if (along < 0.0f && -along > ext_left[jj]) {  // (round 40) no straighter than straight
                                ext_held += -along - ext_left[jj];
                                along = -ext_left[jj];
                            }
                            ext_left[jj] += along;  // (bent further: more to straighten back; straightened: less)
                            const float sl = std::sqrt(vs[0] * vs[0] + vs[1] * vs[1] + vs[2] * vs[2]);
                            const float kk = sl > side_left[jj] ? side_left[jj] / sl : 1.0f;
                            for (int k = 0; k < 3; ++k) v[k] = along * nj[k] + vs[k] * kk;
                            const float took = sl < side_left[jj] ? sl : side_left[jj];
                            side_left[jj] -= took;
                            side_used += took;
                            const float a2 = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                            left[jj] += ang - a2;  // (only what turned is spent)
                            ang = a2;
                            if (ang < 1e-6f) continue;
                            for (int k = 0; k < 3; ++k) ax[k] = v[k] / ang;
                        }
                        used[jj] = cap[jj] - left[jj];
                        axis_angle(ax, ang, R);
                        for (int q = jj + 1; q < 3; ++q) {  // (the planes below turn with the bones: the review)
                            const float n0[3] = {nbj[q][0], nbj[q][1], nbj[q][2]};
                            for (int k = 0; k < 3; ++k) nbj[q][k] = R[k * 3] * n0[0] + R[k * 3 + 1] * n0[1] + R[k * 3 + 2] * n0[2];
                        }
                        for (int i = jj; i < 3; ++i) {
                            float* m = s + tb[i] * 12;
                            const float M[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                            const float t[3] = {m[3] - p[0], m[7] - p[1], m[11] - p[2]};
                            float Mn[9];
                            mul3(R, M, Mn);
                            for (int k = 0; k < 3; ++k) {
                                for (int c = 0; c < 3; ++c) m[k * 4 + c] = Mn[k * 3 + c];
                                m[k * 4 + 3] = R[k * 3] * t[0] + R[k * 3 + 1] * t[1] + R[k * 3 + 2] * t[2] + p[k];
                            }
                        }
                    }
            }
            if (slot == 0 && finite) {  // "skel thumb": the tip against the spur (world), the tip in the wrist's frame; the trace
                float E[3], jw[3], Rw[9];
                tip(E);
                jdrawn(g_rig.wrist[tj], jw);
                ortho_cols(wm, Rw);
                ThumbLast& L = g_thumb_last;
                L.ok = true;
                L.weapon = f.thumb_w;
                L.wt = wt;
                L.hammer = f.thumb_hammer;
                L.dist = std::sqrt((E[0] - spur[0]) * (E[0] - spur[0]) + (E[1] - spur[1]) * (E[1] - spur[1]) + (E[2] - spur[2]) * (E[2] - spur[2]));
                for (int k = 0; k < 3; ++k) {
                    L.spur[k] = spur[k] + off[k];
                    L.tip[k] = E[k] + off[k];
                    L.gun_o[k] = gd_o[k] + off[k];
                    L.tipw[k] = Rw[0 * 3 + k] * (E[0] - jw[0]) + Rw[1 * 3 + k] * (E[1] - jw[1]) + Rw[2 * 3 + k] * (E[2] - jw[2]);
                    L.spurw[k] = Rw[0 * 3 + k] * (spur[0] - jw[0]) + Rw[1 * 3 + k] * (spur[1] - jw[1]) + Rw[2 * 3 + k] * (spur[2] - jw[2]);
                    L.used[k] = used[k] * 57.29578f;
                }
                L.side = side_used * 57.29578f;
                L.held = ext_held * 57.29578f;
                L.ms = log::now_ms();
                static uint64_t rec_frame = ~0ull;
                if (g_thumb_rec_on && rec_frame != g_body_frame) {
                    rec_frame = g_body_frame;
                    ThumbRec& rec = g_thumb_rec[g_thumb_rec_n++ % kThumbRecN];
                    rec.ms = L.ms;
                    rec.wt = wt;
                    rec.hammer = L.hammer;
                    rec.dist = L.dist;
                    std::memcpy(rec.tipw, L.tipw, sizeof(rec.tipw));
                    std::memcpy(rec.spurw, L.spurw, sizeof(rec.spurw));
                    std::memcpy(rec.used, L.used, sizeof(rec.used));
                    rec.side = L.side;
                }
            }
            done = finite;
        }
        if (slot == 0 && !done) g_thumb_last.ok = false;
        // (C3) the gun in hand's thumb full on its spur: kept for that gun; the second gun's (or the copy's) cock: its hand's
        // thumb given that pose (that hand's own, else the other's mirrored), by its weight
        if (done && slot == 0 && f.thumb_wt >= 0.98f && f.thumb_w >= 0 && f.thumb_w < kThumbSnapW && tj >= 0 && tj < 2) {
            FingerRel r = finger_rel(s, n, tj);
            if (r.valid && r.has[0] && r.has[1] && r.has[2]) {
                for (int i = 3; i < 15; ++i) r.has[i] = false;  // (the thumb's alone)
                g_thumb_snap[f.thumb_w][tj] = r;
                g_thumb_snaps.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const int sw = f.sthumb_w, sj = f.sthumb_j;
        if (sw >= 0 && sw < kThumbSnapW && sj >= 0 && sj < 2 && f.sthumb_wt > 0.001f) {
            FingerRel smr;
            const FingerRel* sr = g_thumb_snap[sw][sj].valid ? &g_thumb_snap[sw][sj] : nullptr;
            if (!sr && g_rig.mirror_ok && g_thumb_snap[sw][1 - sj].valid) {
                smr = mirrored_rel(g_thumb_snap[sw][1 - sj]);
                sr = &smr;
            }
            // (C3c; round 66 check 8, the user: worse with C3b's chain blend, the thumb stretched far below the gun) put_fingers'
            // blend again (C3b reverted)
            if (sr && put_fingers(sj, sr, std::fmin(1.0f, f.sthumb_wt)) && slot == 0) g_sthumb_frames.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (slot == 0) {  // "skel fingers": each hand's fingers' mean turn from its wrist (degrees), as drawn this frame
        for (int h = 0; h < 2; ++h) {
            const int wb = g_rig.wrist[h];
            if (wb < 0 || wb >= n) continue;
            const float* wm = s + wb * 12;
            float sum = 0.0f;
            int k = 0;
            for (int i = 0; i < 15; ++i) {
                const int fb = g_rig.finger[h][i];
                if (fb < 0 || fb >= n) continue;
                const float* fm = s + fb * 12;
                float wf = 0.0f;  // the trace of W^T F (the wrist's and the finger's axes as columns)
                for (int a = 0; a < 3; ++a)
                    for (int c = 0; c < 3; ++c) wf += wm[c * 4 + a] * fm[c * 4 + a];
                const float l = std::sqrt(wm[0] * wm[0] + wm[4] * wm[4] + wm[8] * wm[8]) * std::sqrt(fm[0] * fm[0] + fm[4] * fm[4] + fm[8] * fm[8]);
                if (l < 1e-6f) continue;
                sum += std::acos(std::fmax(-1.0f, std::fmin(1.0f, (wf / l - 1.0f) * 0.5f))) * 57.2958f;
                ++k;
            }
            g_curl[h].store(k ? sum / k : -1.0f, std::memory_order_relaxed);
        }
    }
    // the knuckle check: John's drawn hand's knuckles in the drawn gun's frame against the game's gripping hand's in the
    // game's gun frame, mirrored across the gun's middle plane (x' = 2 x0 - x)
    if (slot == 0) {
        float err = -1.0f;
        const HeldSample* hs = newest_held(f.held);
        const int d = f.gun_j;
        if (kn_ok && xfer_ok && hs && !hs->left && d >= 0 && d < 2) {
            float kd[3] = {0, 0, 0};
            int c = 0;
            for (int i = 3; i < 15; i += 3) {
                const int b = g_rig.finger[d][i];
                if (b < 0 || b >= n) continue;
                float p[3];
                jdrawn(b, p);  // the set now holds the drawn matrices
                for (int k = 0; k < 3; ++k) kd[k] += p[k];
                ++c;
            }
            if (c == 4) {
                for (int k = 0; k < 3; ++k) kd[k] *= 0.25f;
                const float* G = hs->game;
                float Gx[3][3], go[3], dgo[3], rg[3], rd[3];
                for (int row = 0; row < 3; ++row) {
                    float l = std::sqrt(G[row * 4] * G[row * 4] + G[row * 4 + 1] * G[row * 4 + 1] + G[row * 4 + 2] * G[row * 4 + 2]);
                    l = l > 1e-6f ? l : 1.0f;
                    for (int k = 0; k < 3; ++k) Gx[row][k] = G[row * 4 + k] / l;
                }
                for (int k = 0; k < 3; ++k) go[k] = G[12 + k] - off[k];
                const Corr& t = item[f.item_side];
                corr_point(t, go, dgo);
                for (int row = 0; row < 3; ++row) {
                    float ax[3];
                    for (int k = 0; k < 3; ++k) ax[k] = t.A[k * 3] * Gx[row][0] + t.A[k * 3 + 1] * Gx[row][1] + t.A[k * 3 + 2] * Gx[row][2];
                    rg[row] = Gx[row][0] * (kn_g[0] - go[0]) + Gx[row][1] * (kn_g[1] - go[1]) + Gx[row][2] * (kn_g[2] - go[2]);
                    rd[row] = ax[0] * (kd[0] - dgo[0]) + ax[1] * (kd[1] - dgo[1]) + ax[2] * (kd[2] - dgo[2]);
                }
                rg[0] = 2.0f * (hs->mo_ok ? hs->mo[0] : 0.0f) - rg[0];
                err = std::sqrt((rd[0] - rg[0]) * (rd[0] - rg[0]) + (rd[1] - rg[1]) * (rd[1] - rg[1]) + (rd[2] - rg[2]) * (rd[2] - rg[2]));
            }
        }
        g_xfer_knuckle_err = err;
    }
    // the world versions (props): the set is the world less `off`, so T_world(x) = A x + a + off - A off
    if (slot == 0) {
        g_corr_world.resize(n);
        for (int b = 0; b < n; ++b) {
            const Corr& t = g_corr[b];
            Corr& w = g_corr_world[b];
            std::memcpy(w.A, t.A, sizeof(w.A));
            for (int k = 0; k < 3; ++k)
                w.a[k] = t.a[k] + off[k] - (t.A[k * 3] * off[0] + t.A[k * 3 + 1] * off[1] + t.A[k * 3 + 2] * off[2]);
        }
        g_corr_world_valid = true;
        g_corr_frame = g_body_frame;
        for (int h = 0; h < 2; ++h) {  // [Hands] StaleGunFix: the targets it was made with
            g_corr_ik_ok[h] = f.ik[h];
            std::memcpy(g_corr_ikt[h], f.ik_t[h], sizeof(g_corr_ikt[h]));
            std::memcpy(g_corr_ikr[h], f.ik_r[h], sizeof(g_corr_ikr[h]));
        }
        g_corr_set = g_corr;
        for (int g = 0; g < 2; ++g) {  // for the held item's draws (this thread)
            g_item_set[g] = item[g];
            Corr& iw = g_item_world[g];
            std::memcpy(iw.A, item[g].A, sizeof(iw.A));
            for (int k = 0; k < 3; ++k)
                iw.a[k] = item[g].a[k] + off[k] - (item[g].A[k * 3] * off[0] + item[g].A[k * 3 + 1] * off[1] + item[g].A[k * 3 + 2] * off[2]);
        }
        g_item_xfer = xfer_ok;
        // the drawn barrel (the game's gun -z through the item's correction) in the gun hand's target frame, this frame
        const HeldSample* hb = newest_held(f.held);
        const int bside = hb ? (hb->left ? 0 : 1) : -1, bj = f.gun_j;
        // ([Hands] CarryByForegrip: none from the carried gun's put-away, placed at the front hand: not this hand's barrel)
        if (hb && hb->follow_j < 0 && bside >= 0 && bj >= 0 && bj < 2 && f.ik[bj]) {
            float gz[3] = {-hb->game[8], -hb->game[9], -hb->game[10]}, dw[3];
            if (fixed_grip_on() && g_snap_drawn_cfg.load(std::memory_order_relaxed)) {
                // [Reload] SnapToDrawnGun: the drawn gun's barrel (the sample's, its placement's turn undone)
                const float mz[3] = {-hb->m[8], -hb->m[9], -hb->m[10]};
                for (int k = 0; k < 3; ++k) gz[k] = hb->A[k] * mz[0] + hb->A[3 + k] * mz[1] + hb->A[6 + k] * mz[2];
            }
            norm(gz);
            const Corr& c = item[bside];
            for (int k = 0; k < 3; ++k) dw[k] = c.A[k * 3] * gz[0] + c.A[k * 3 + 1] * gz[1] + c.A[k * 3 + 2] * gz[2];
            const float* R = f.ik_r[bj];  // columns: the target's axes
            for (int k = 0; k < 3; ++k) g_bt_body[k] = R[0 * 3 + k] * dw[0] + R[1 * 3 + k] * dw[1] + R[2 * 3 + k] * dw[2];
            g_bt_body_frame = g_body_frame;
            g_bt_body_ok = true;
        } else {
            g_bt_body_ok = false;
        }
    }
    // published for the other threads once a frame, from its first current set (every set of a frame gives the same):
    // a write holds the readers off while it lasts, and with 20-40 a frame a reader met one now and then
    if (slot == 0 && g_pub_frame != g_body_frame) {
        g_pub_frame = g_body_frame;
        const double now = log::now_ms();
        g_hand_seq.fetch_add(1, std::memory_order_acq_rel);
        for (int h = 0; h < 2; ++h) {
            const int wb = g_rig.wrist[h];
            if (wb < 0 || wb >= n) continue;
            std::memcpy(g_hand_corr[h].A, g_corr_world[wb].A, sizeof(g_hand_corr[h].A));
            std::memcpy(g_hand_corr[h].a, g_corr_world[wb].a, sizeof(g_hand_corr[h].a));
            for (int k = 0; k < 3; ++k)  // the world translation in double: a + off - A off
                g_hand_corr[h].ad[k] = static_cast<double>(g_corr[wb].a[k]) + off[k] -
                                       (static_cast<double>(g_corr[wb].A[k * 3]) * off[0] + static_cast<double>(g_corr[wb].A[k * 3 + 1]) * off[1] +
                                        static_cast<double>(g_corr[wb].A[k * 3 + 2]) * off[2]);
            g_hand_corr[h].ms = now;
            g_hand_corr[h].ik = f.ik[h];
        }
        g_points.ms = now;
        std::memcpy(g_points.cam, f.cam, sizeof(g_points.cam));
        g_points.cam_ok = f.cam_ok;
        g_points.fore_ok = pub_fore;
        g_points.gun_frame_ok = pub_gun;
        std::memcpy(g_points.gun_frame_R, pub_gun_R, sizeof(g_points.gun_frame_R));
        std::memcpy(g_points.gun_frame_o, pub_gun_o, sizeof(g_points.gun_frame_o));
        g_points.gun_mframe_ok = pub_mgun;
        std::memcpy(g_points.gun_mframe_R, pub_mgun_R, sizeof(g_points.gun_mframe_R));
        std::memcpy(g_points.gun_mframe_o, pub_mgun_o, sizeof(g_points.gun_mframe_o));
        std::memcpy(g_points.fore, pub_fore_p, sizeof(g_points.fore));
        g_points.gun = f.gun_j;
        g_points.frame = g_body_frame;
        std::memcpy(g_points.ctrl, f.ctrl, sizeof(g_points.ctrl));
        for (int g = 0; g < 2; ++g) {
            std::memcpy(g_item_corr[g].A, g_item_world[g].A, sizeof(g_item_corr[g].A));
            std::memcpy(g_item_corr[g].a, g_item_world[g].a, sizeof(g_item_corr[g].a));
            for (int k = 0; k < 3; ++k)  // the world translation in double: a + off - A off
                g_item_corr[g].ad[k] = static_cast<double>(item[g].a[k]) + off[k] -
                                       (static_cast<double>(item[g].A[k * 3]) * off[0] + static_cast<double>(item[g].A[k * 3 + 1]) * off[1] +
                                        static_cast<double>(item[g].A[k * 3 + 2]) * off[2]);
            g_item_corr[g].ms = now;
            g_item_corr[g].ik = f.ik[xfer_ok && g == f.item_side ? f.gun_j : g];
            // item 1b: the game matrix this correction is drawn on (the newest build's sample, when it is this hand's)
            const HeldSample* hg = newest_held(f.held);
            g_item_corr[g].gun_ok = hg && !hg->sec && hg->left == (g == 0);
            g_item_corr[g].W = g_item_corr[g].gun_ok ? hg->W : 0;
            if (g_item_corr[g].gun_ok) std::memcpy(g_item_corr[g].gun, hg->game, sizeof(g_item_corr[g].gun));
            g_item_corr[g].frame = g_pub_frame;
        }
        for (int h = 0; h < 2; ++h) {
            g_points.hand_ok[h] = f.ik[h];
            std::memcpy(g_points.hand[h], f.ik_t[h], sizeof(g_points.hand[h]));
            std::memcpy(g_points.target_rot[h], f.ik_r[h], sizeof(g_points.target_rot[h]));
            std::memcpy(g_points.wrist_anim[h], wrist_anim[h], sizeof(g_points.wrist_anim[h]));
            const int wb = g_rig.wrist[h];
            if (wb >= 0 && wb < n) mul3(g_corr[wb].A, wrist_anim[h], g_points.wrist_drawn[h]);
        }
        for (int i = 0; i < body::kHolsterBones; ++i) {
            g_points.bone_ok[i] = holster_ok[i];
            for (int k = 0; k < 3; ++k) g_points.bone[i][k] = holster_pt[i][k] + off[k];  // the set is the world less `off`
        }
        for (int k = 0; k < 3; ++k) g_points.root[k] = root_corr[k] + off[k];
        for (int h = 0; h < 2; ++h) {  // the set holds the drawn matrices here
            const int tb = g_rig.finger[h][2], ib = g_rig.finger[h][5];
            g_points.round_flat[h] = g_round_flat[h];  // (cycle r171)
            g_points.round_pinch[h] = g_round_pinch_put[h];
            g_points.tips_ok[h] = tb >= 0 && tb < n && ib >= 0 && ib < n;
            if (!g_points.tips_ok[h]) continue;
            jdrawn(tb, g_points.thumb_tip[h]);
            jdrawn(ib, g_points.index_tip[h]);
            for (int k = 0; k < 3; ++k) {
                g_points.thumb_tip[h][k] += off[k];
                g_points.index_tip[h][k] += off[k];
            }
        }
        for (int h = 0; h < 2; ++h) {  // (2026-10-29, [Hands] GunCollision) each arm as drawn, and its hand's move then
            const int eb = g_rig.elbow[h], wb = g_rig.wrist[h];
            float kc[3] = {0.0f, 0.0f, 0.0f};
            int nk = 0;
            for (int i = 3; i < 15; i += 3) {  // the knuckles (finger_11, 21, 31, 41)
                const int b = g_rig.finger[h][i];
                if (b < 0 || b >= n) continue;
                float p[3];
                jdrawn(b, p);
                for (int k = 0; k < 3; ++k) kc[k] += p[k];
                ++nk;
            }
            g_points.limb_ok[h] = eb >= 0 && eb < n && wb >= 0 && wb < n && nk > 0;
            if (!g_points.limb_ok[h]) continue;
            jdrawn(eb, g_points.elbow_d[h]);
            jdrawn(wb, g_points.wrist_d[h]);
            for (int k = 0; k < 3; ++k) {
                g_points.elbow_d[h][k] += off[k];
                g_points.wrist_d[h][k] += off[k];
                g_points.knuckle_d[h][k] = kc[k] / static_cast<float>(nk) + off[k];
                g_points.hand_push[h][k] = f.coll_hand_on[h] ? f.coll_hand[h][k] : 0.0f;
            }
        }
        // (2026-10-28, round 38 check 2, the user: "works for the main gun, second guns reticle still all over the place")
        // the second gun (DualWield's, another slot) or the copy (DualWieldCopy) as drawn this frame: its newest placed
        // sample with its hand's motion since that placement (held_delta, as its draws take it: the placement's
        // correction undone, this frame's on), for the second reticle and its shot (aim.cpp: the line was the gun's
        // holster matrix or the copy's T of another build before). The copy also with Tw, the gun in hand as the aim
        // draws it (the item's correction on that build's game matrix, item_gun's) onto the copy as drawn
        g_sec_pub.ok = false;
        {
            const HeldSample* sh = nullptr;
            bool copy = false;
            for (const HeldSample& x : f.sec)
                if (x.valid && x.placed && (!sh || x.build > sh->build)) sh = &x;
            if (!sh) {
                for (const HeldSample& x : f.copy)
                    if (x.valid && x.placed && (!sh || x.build > sh->build)) sh = &x;
                copy = sh != nullptr;
            }
            if (sh && sh->john >= 0 && sh->john < 2 && f.ik[sh->john]) {
                Corr dw, ds;
                held_delta(*sh, -1, &dw, &ds, off);
                const Corr D = corr_mul(dw, corr_rows(sh->m));
                body::SecondDrawn& sd = g_sec_pub.d;
                bool fin = true;
                for (int i = 0; i < 3; ++i) {  // rows: the axes, then the position
                    for (int k = 0; k < 3; ++k) sd.m[i * 4 + k] = D.A[k * 3 + i];
                    sd.m[i * 4 + 3] = 0.0f;
                }
                for (int k = 0; k < 3; ++k) sd.m[12 + k] = D.a[k];
                sd.m[15] = 1.0f;
                for (int i = 0; i < 16; ++i) fin = fin && std::isfinite(sd.m[i]);
                sd.W = sh->W;
                sd.john = sh->john;
                sd.copy = copy;
                for (int k = 0; k < 3; ++k) sd.shift[k] = copy ? sh->mshift[k] : 0.0f;
                std::memcpy(sd.R, f.ik_r[sh->john], sizeof(sd.R));
                std::memcpy(sd.t, f.ik_t[sh->john], sizeof(sd.t));
                sd.tw_ok = false;
                if (copy) {
                    const HeldSample* hg = nullptr;
                    for (const HeldSample& x : f.held)
                        if (x.valid && !x.sec && x.build == sh->build && x.W == sh->W) hg = &x;
                    Corr mi;
                    if (hg && corr_inv(corr_mul(g_item_world[hg->left ? 0 : 1], corr_rows(hg->game)), &mi)) {
                        const Corr Tw = corr_mul(D, mi);
                        bool tf = true;
                        for (int i = 0; i < 3; ++i) {
                            for (int k = 0; k < 3; ++k) sd.Tw[i * 4 + k] = Tw.A[k * 3 + i];
                            sd.Tw[i * 4 + 3] = 0.0f;
                        }
                        for (int k = 0; k < 3; ++k) sd.Tw[12 + k] = Tw.a[k];
                        sd.Tw[15] = 1.0f;
                        for (int i = 0; i < 16; ++i) tf = tf && std::isfinite(sd.Tw[i]);
                        sd.tw_ok = tf;
                        if (tf) g_sec_drawn_tw.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                sd.frame = g_body_frame;
                sd.build = sh->build;
                sd.ms = now;
                g_sec_pub.ok = fin;
                if (fin) g_sec_drawn_pubs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_hand_seq.fetch_add(1, std::memory_order_release);
        // run 6 item 6: the held props placed by the body follow its pelvis within the frame (the pelvis as drawn, not
        // the root bone, which moves apart from the skeleton while walking: cycle Q2's 2.5 cm in 3-5 % of the frames)
        float rw[3];
        for (int k = 0; k < 3; ++k) rw[k] = (holster_ok[body::kPelvisPoint] ? holster_pt[body::kPelvisPoint][k] : root_corr[k]) + off[k];
        held_prop::note_root(rw);
    }
    if (slot == 0 && g_skinned_bone >= 0 && g_skinned_bone < n) std::memcpy(g_player_bone_diag, s + g_skinned_bone * 12, sizeof(g_player_bone_diag));
}

// A rigid matrix's three axes (rows of 4) set to 1e-4 long, the translation kept: idempotent.
// No cloth flutter for a draw ([Body] HiddenNoFlutter): the character shaders push a vertex along its normalised
// skinned normal by up to 0.7 x gWindParams.y, which a collapsed (hidden) vertex keeps. Around the player's draws
// while parts are hidden, gWindParams.y is set to 0 through the game's own SetShaderGlobal (as DrawVisEntity sets the
// entity fade) and set back after the draw; the game rewrites it each frame (FUN_1406b1480). Render thread; SEH-
// guarded reads, no system calls.
using SetShaderGlobal_t = void (*)(int, const void*, int, int);
struct WindHold {
    bool held = false;
    int id = 0;
    float old[4] = {};
};
std::atomic<uint64_t> g_wind_holds{0};
std::atomic<float> g_wind_test{-1.0f};  // "skel body wind test <y>": that wind on the player's draws (a test aid)
std::atomic<bool> g_wind_still_cfg{true};
// [Body] NoFlutter (2026-11-02, GOAL-older-items item 23; after optimumbox's 0.8.11.11): the cloth flutter held off on
// every player draw, not only while parts are hidden (from inside the body the flutter reads as a buzz)
std::atomic<bool> g_no_flutter_cfg{true};
std::atomic<float> g_collapse_cfg{1e-7f};
std::atomic<bool> g_collapse_root_cfg{true};
bool wind_param(int* id, uintptr_t* at) {
    const uintptr_t pid = anchors::addr(anchors::Id::WindParamsId), tab = anchors::addr(anchors::Id::ShaderGlobalTable);
    uintptr_t cb = 0, map = 0;
    int off = -1;
    if (!pid || !tab || !raw(pid, id, 4) || *id <= 0 || *id > 4096) return false;
    const uintptr_t e = tab + static_cast<uintptr_t>(*id) * 16;
    if (!raw(e, &cb, 8) || !cb || !raw(e + 8, &off, 4) || off < 0 || off > 0x10000) return false;
    // where SetShaderGlobal writes: the mapped copy (+0x18), else the CPU copy it maps (+0x00)
    if (!raw(cb + 0x18, &map, 8)) return false;
    if (!map && (!raw(cb, &map, 8) || !map)) return false;
    *at = map + static_cast<uintptr_t>(off);
    return true;
}
void wind_hold(WindHold* w, float y) {
    const uintptr_t fn = anchors::addr(anchors::Id::SetShaderGlobal);
    uintptr_t at = 0;
    if (w->held || !fn || !wind_param(&w->id, &at) || !raw(at, w->old, 16)) return;
    if (w->old[1] == y) return;  // already that wind (none, this frame)
    const float v[4] = {w->old[0], y, w->old[2], w->old[3]};
    reinterpret_cast<SetShaderGlobal_t>(fn)(w->id, v, 1, 5);
    w->held = true;
    g_wind_holds.fetch_add(1, std::memory_order_relaxed);
}
void wind_release(const WindHold& w) {
    if (w.held) reinterpret_cast<SetShaderGlobal_t>(anchors::addr(anchors::Id::SetShaderGlobal))(w.id, w.old, 1, 5);
}
float collapse_scale() { return g_collapse_cfg.load(std::memory_order_relaxed); }
thread_local WindHold t_rigid_wind;
void rigid_wind(bool on) {
    if (on) {
        t_rigid_wind = WindHold{};
        wind_hold(&t_rigid_wind, 0.0f);  // (already none, or held by the hide's: nothing done)
    } else {
        wind_release(t_rigid_wind);
        t_rigid_wind.held = false;
    }
}

// A matrix's origin (12..14) in a box (min xyz, max xyz); false for NaN.
bool in_box(const float* b, const float* m) {
    return m[12] >= b[0] && m[12] <= b[3] && m[13] >= b[1] && m[13] <= b[4] && m[14] >= b[2] && m[14] <= b[5];
}

// The sample (an index into g_frame.held) whose matrix the record matrix `rm` is (the item in hand), else -1: the
// origin and the first row within 2 mm; of several (a still gun's), the newest.
// "skel near": the draws near the item in hand (render thread; reads only)
struct NearDraw {
    uintptr_t drawable;
    int kind;  // 1 a matrix set, 2 rigid
    int count;
    float dmin;
    float at[3];
    uint64_t n;
    int abone;    // the nearest attachment bone (g_rig.attach's), at its nearest
    float adist;
};
std::atomic<bool> g_near_on{false};
std::mutex g_near_mutex;
NearDraw g_near[24];
int g_nnear = 0;
// run 10 item 2: a draw's instance (the record's +0x98 object: its owner at +0x18, the skeleton the player's draws are
// told by; the object itself when it has none), so props the mod made are told from the same model's other copies
uintptr_t draw_instance(void* const* rec) {
    const uintptr_t o = reinterpret_cast<uintptr_t>(rec[0x13]);
    uintptr_t owner = 0;
    if (o && raw(o + 0x18, &owner, 8) && owner) return owner;
    return o;
}

void note_near(uintptr_t drawable, int kind, int count, const float* pos, const float* item) {
    const float dx = pos[0] - item[0], dy = pos[1] - item[1], dz = pos[2] - item[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d > 0.6f) return;
    int ab = -1;
    float ad = 1e9f;
    const std::vector<float>& ap = g_frame.attach_pos;
    for (size_t i = 0; i * 3 + 2 < ap.size() && i < g_rig.attach.size(); ++i) {
        const float ex = pos[0] - ap[i * 3], ey = pos[1] - ap[i * 3 + 1], ez = pos[2] - ap[i * 3 + 2];
        const float e = std::sqrt(ex * ex + ey * ey + ez * ez);
        if (e < ad) ad = e, ab = g_rig.attach[i];
    }
    std::lock_guard lock(g_near_mutex);
    for (int i = 0; i < g_nnear; ++i)
        if (g_near[i].drawable == drawable && g_near[i].kind == kind) {
            ++g_near[i].n;
            if (d < g_near[i].dmin) {
                g_near[i].dmin = d;
                std::memcpy(g_near[i].at, pos, sizeof(g_near[i].at));
                g_near[i].abone = ab;
                g_near[i].adist = ad;
            }
            return;
        }
    if (g_nnear < 24) g_near[g_nnear++] = {drawable, kind, count, d, {pos[0], pos[1], pos[2]}, 1, ab, ad};
}

int held_match(const float* rm) {
    int best = -1;
    for (int i = 0; i < kHeldRing; ++i) {
        const HeldSample& h = g_frame.held[i];
        if (!h.valid || (best >= 0 && h.build < g_frame.held[best].build)) continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) {
            const float a = rm[12 + k] - h.m[12 + k], b = rm[k] - h.m[k];
            d = std::fmax(d, std::fmax(std::fabs(a), std::fabs(b)));
        }
        if (d < 2e-3f) best = i;
    }
    if (best >= 0) (best == g_frame.held_new ? g_held_newest : g_held_older).fetch_add(1, std::memory_order_relaxed);
    return best;
}

// The same for the second gun's samples ([Hands] DualWield), else -1.
int sec_match(const float* rm) {
    int best = -1;
    for (int i = 0; i < kHeldRing; ++i) {
        const HeldSample& h = g_frame.sec[i];
        if (!h.valid || (best >= 0 && h.build < g_frame.sec[best].build)) continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) {
            const float a = rm[12 + k] - h.m[12 + k], b = rm[k] - h.m[k];
            d = std::fmax(d, std::fmax(std::fabs(a), std::fabs(b)));
        }
        if (d < 2e-3f) best = i;
    }
    return best;
}

// What a held item's draw takes (under g_draw_mutex): its hand's correction now, less the one it was placed with
// (world and the player set's space, as g_corr_world and g_corr_set relate).
// Worked in the set's space (small numbers; the placement's translation in double), so the world's large coordinates
// do not round the gun off the hand each frame (round 8: "guns shake slightly in hand").
// off: the set's offset (null: the player set's last, g_player_offset; transform_set's publish passes its own set's)
void held_delta(const HeldSample& h, int bone, Corr* w, Corr* s, const float* off_in) {
    static const Corr kId = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
    // the second gun: its own hand's (never the transplant); the carried gun's put-away: the front hand's it was placed at
    const int fj = h.sec ? h.john : h.follow_j;
    const int sw = (h.sec || h.follow_j >= 0) && fj >= 0 && fj < 2 ? g_rig.wrist[fj] : -1;
    const Corr& now = h.sec || h.follow_j >= 0 ? (sw >= 0 && sw < static_cast<int>(g_corr_set.size()) ? g_corr_set[sw] : kId)
                    : g_item_xfer ? g_item_set[h.left ? 0 : 1]
                    : bone >= 0 && bone < static_cast<int>(g_corr_set.size()) ? g_corr_set[bone] : kId;
    const float* off = off_in ? off_in : g_player_offset;
    if (put_by_target(h)) {
        // [Hands] CarryByForegrip, the put-away: its front target's move from the one it was placed with to this frame's,
        // D = Rn Rp^T about it (as StaleGunFix's), not the wrist's correction change: that also carries the game's own
        // hidden wrist's move over the frame (the review: 5-13 mm at the muzzle while the game's arm leaves the foregrip).
        // In the set's space: y = D (x - tp) + tn
        const float* Rn = g_frame.ik_r[h.follow_j];
        const float* Rp = h.pair_r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) s->A[i * 3 + j] = Rn[i * 3] * Rp[j * 3] + Rn[i * 3 + 1] * Rp[j * 3 + 1] + Rn[i * 3 + 2] * Rp[j * 3 + 2];
        double tp[3], tn[3];
        for (int k = 0; k < 3; ++k) {
            tp[k] = static_cast<double>(h.pair_t[k]) - off[k];
            tn[k] = static_cast<double>(g_frame.ik_t[h.follow_j][k]) - off[k];
        }
        for (int k = 0; k < 3; ++k)
            s->a[k] = static_cast<float>(tn[k] - (static_cast<double>(s->A[k * 3]) * tp[0] + static_cast<double>(s->A[k * 3 + 1]) * tp[1] +
                                                  static_cast<double>(s->A[k * 3 + 2]) * tp[2]));
    } else {
        double ua[3];  // the placement's correction in the set's space
        for (int k = 0; k < 3; ++k)
            ua[k] = h.ad[k] - off[k] + (static_cast<double>(h.A[k * 3]) * off[0] + static_cast<double>(h.A[k * 3 + 1]) * off[1] +
                                        static_cast<double>(h.A[k * 3 + 2]) * off[2]);
        float ut[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) ut[i * 3 + j] = h.A[j * 3 + i];
        mul3(now.A, ut, s->A);
        for (int k = 0; k < 3; ++k)
            s->a[k] = static_cast<float>(now.a[k] - (static_cast<double>(s->A[k * 3]) * ua[0] + static_cast<double>(s->A[k * 3 + 1]) * ua[1] +
                                                     static_cast<double>(s->A[k * 3 + 2]) * ua[2]));
    }
    std::memcpy(w->A, s->A, sizeof(w->A));
    for (int k = 0; k < 3; ++k)
        w->a[k] = static_cast<float>(static_cast<double>(s->a[k]) + off[k] -
                                     (static_cast<double>(s->A[k * 3]) * off[0] + static_cast<double>(s->A[k * 3 + 1]) * off[1] +
                                      static_cast<double>(s->A[k * 3 + 2]) * off[2]));
}

bool wraw(uintptr_t a, const void* in, size_t n) {
    __try {
        std::memcpy(reinterpret_cast<void*>(a), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A matrix's three axis rows unit length and square to each other, its translation under a metre (a grip locator)
bool rigid_rows(const float* m) {
    for (int i = 0; i < 3; ++i) {
        const float* a = m + i * 4;
        const float l = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        if (!(l > 0.98f && l < 1.02f)) return false;  // also NaN
        for (int j = i + 1; j < 3; ++j) {
            const float* b = m + j * 4;
            if (!(std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) < 0.02f)) return false;
        }
    }
    return m[12] * m[12] + m[13] * m[13] + m[14] * m[14] < 1.0f;
}

// the wrist correction of hand h (world), its translation also in double (the second gun's placement)
// out = G x B as the game makes a gun's matrix (FUN_140148d20): the rows of G (the gun in the bone's frame; its 4th
// column unused) through B (the bone in the world), the translation row plus B's
void grip_times(const float* G, const float* B, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            out[i * 4 + k] = G[i * 4] * B[k] + G[i * 4 + 1] * B[4 + k] + G[i * 4 + 2] * B[8 + k] + (i == 3 ? B[12 + k] : 0.0f);
}

bool hand_correction_d(int h, float A[9], float a[3], bool* ik, double* ad, double* ms = nullptr) {  // ms: its publish's time (body_points' too)
    if (h < 0 || h > 1) return false;
    HandCorr c;
    if (!seq_copy(g_hand_seq, &c, &g_hand_corr[h], sizeof(c))) return false;
    if (!c.ms || log::now_ms() - c.ms > 250) return false;  // no player set drawn lately: not in first person
    if (ms) *ms = c.ms;
    std::memcpy(A, c.A, sizeof(c.A));
    std::memcpy(a, c.a, sizeof(c.a));
    std::memcpy(ad, c.ad, sizeof(c.ad));
    if (ik) *ik = c.ik;
    return true;
}


// [Hands] DualWield: the second gun placed in John's free hand j as the game places a gun in hand (FUN_1402fe6d0:
// W +0x80 = the model's grip locator x the hand bone's world matrix; the right grip at locators +0xc0, the left at
// +0x100), then that hand's correction (as the gun in hand's). W +0x80 itself takes it, so the gun's muzzle and barrel
// (its shots) follow the hand. The game thread inside the item update; SEH-guarded, no system calls.
bool place_secondary(uintptr_t W, float* m, int j) {
    auto refuse = [](int why) {
        g_sec_refused.fetch_add(1, std::memory_order_relaxed);
        g_sec_why.store(why, std::memory_order_relaxed);
        return false;
    };
    if (j < 0 || j > 1 || !pose::anchor_active()) return refuse(1);
    const int ab = g_att_bone[j].load(std::memory_order_relaxed);
    const uintptr_t skel = g_skel_game.load(std::memory_order_relaxed);
    uintptr_t mtx = 0, L = 0;
    alignas(16) float B[16], G[16], src[16], out[16];
    if (ab < 0 || !skel || !raw(skel + 0x28, &mtx, sizeof(mtx)) || !mtx || !raw(mtx + static_cast<uintptr_t>(ab) * 0x40, B, sizeof(B)))
        return refuse(2);
    if (!raw(W + 0x338, &L, sizeof(L)) || !L || !raw(L + (j ? 0xc0 : 0x100), G, sizeof(G)) || !rigid_rows(G)) return refuse(3);
    grip_times(G, B, src);
    float A[9], a[3];
    double ad[3] = {};
    bool ik = false;
    if (!hand_correction_d(j, A, a, &ik, ad) || !ik) return refuse(4);
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) out[i * 4 + k] = A[k * 3] * src[i * 4] + A[k * 3 + 1] * src[i * 4 + 1] + A[k * 3 + 2] * src[i * 4 + 2];
        out[i * 4 + 3] = src[i * 4 + 3];
    }
    for (int k = 0; k < 3; ++k)  // in double, as the gun in hand's
        out[12 + k] = static_cast<float>(static_cast<double>(A[k * 3]) * src[12] + static_cast<double>(A[k * 3 + 1]) * src[13] +
                                         static_cast<double>(A[k * 3 + 2]) * src[14] + ad[k]);
    out[15] = src[15];
    if (!wraw(reinterpret_cast<uintptr_t>(m), out, sizeof(out))) return refuse(5);
    // its shot direction too: FUN_1402fe6d0 set W +0x920 = -W +0xa0 (normalised) from the holster's matrix before this
    // call, and the shot takes it (the simulator: 131 degrees off the drawn barrel without this)
    const float zl = std::sqrt(out[8] * out[8] + out[9] * out[9] + out[10] * out[10]);
    if (zl > 1e-6f) {
        const float dir[3] = {-out[8] / zl, -out[9] / zl, -out[10] / zl};
        if (!wraw(W + 0x920, dir, sizeof(dir))) return refuse(5);
    }
    g_sec_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    g_sec_placed.W = W;
    g_sec_placed.build = g_vis_builds.load(std::memory_order_relaxed);
    std::memcpy(g_sec_placed.m, out, sizeof(out));
    std::memcpy(g_sec_placed.A, A, sizeof(A));
    std::memcpy(g_sec_placed.a, a, sizeof(a));
    std::memcpy(g_sec_placed.ad, ad, sizeof(ad));
    g_sec_placed.john = j;
    g_sec_placed.ms = log::now_ms();
    g_sec_placed.carry = false;
    g_sec_placed.follow_j = -1;
    g_sec_placed.put = false;
    g_sec_placed.pair_ok = false;
    g_sec_seq.fetch_add(1, std::memory_order_release);
    g_sec_placements.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// [Hands] CarryByForegrip: John's hand j's attachment bone as drawn (the bone place_secondary takes, through that hand's
// correction): its axis rows R, its translation t in double; A, a, ad the correction. The game thread; SEH-guarded reads.
bool corrected_bone(int j, float R[9], double t[3], float A[9], float a[3], double ad[3], double* ms = nullptr) {
    if (j < 0 || j > 1) return false;
    const int ab = g_att_bone[j].load(std::memory_order_relaxed);
    const uintptr_t skel = g_skel_game.load(std::memory_order_relaxed);
    uintptr_t mtx = 0;
    alignas(16) float B[16];
    if (ab < 0 || !skel || !raw(skel + 0x28, &mtx, sizeof(mtx)) || !mtx || !raw(mtx + static_cast<uintptr_t>(ab) * 0x40, B, sizeof(B))) return false;
    bool ik = false;
    if (!hand_correction_d(j, A, a, &ik, ad, ms) || !ik) return false;
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) R[i * 3 + k] = A[k * 3] * B[i * 4] + A[k * 3 + 1] * B[i * 4 + 1] + A[k * 3 + 2] * B[i * 4 + 2];
    for (int k = 0; k < 3; ++k)
        t[k] = static_cast<double>(A[k * 3]) * B[12] + static_cast<double>(A[k * 3 + 1]) * B[13] + static_cast<double>(A[k * 3 + 2]) * B[14] + ad[k];
    bool ok = true;
    for (int i = 0; i < 9; ++i) ok = ok && std::isfinite(R[i]);
    for (int k = 0; k < 3; ++k) ok = ok && std::isfinite(t[k]);
    return ok;
}

// "skel carry trace": John's front hand's target as a placement's update reads it (body_points), into its Placed (made
// before the placement's seqlock is taken: the readers' window not lengthened). Nothing while no trace is asked (a
// rigid placement's own is always made, from its placement's read). pub_ms: the publish of the correction the placement
// used (hand_correction_d's): the points of another publish are not its pair (r228: one frame of a turn apart, 1.7 cm)
struct FrontPair {
    bool ok = false;
    float t[3] = {}, r[9] = {};
};
FrontPair pair_front(double pub_ms = -1.0) {
    FrontPair p;
    if (!(log::now_ms() < g_carry_trace_until.load(std::memory_order_relaxed))) return p;
    BodyPoints bp;
    if (!body_points(&bp) || (pub_ms >= 0.0 && bp.ms != pub_ms)) return p;
    const int fj = 1 - bp.gun;
    if (fj < 0 || fj > 1 || !bp.hand_ok[fj]) return p;
    std::memcpy(p.t, bp.hand[fj], sizeof(p.t));
    std::memcpy(p.r, bp.target_rot[fj], sizeof(p.r));
    p.ok = true;
    return p;
}
void put_pair(Placed& d, const FrontPair& p) {
    d.pair_ok = p.ok;
    std::memcpy(d.pair_t, p.t, sizeof(d.pair_t));
    std::memcpy(d.pair_r, p.r, sizeof(d.pair_r));
}

// [Hands] CarryByForegrip: W, the item the game holds in its hand, is the carried gun still to be held at the front hand
// (its put-away: T not made, not taken back, not "skel carry old"); John's carrying hand in *j. The game thread's
// placement (hk_obj_set_matrix); the frame's carry_put follows what it placed (the held sample's follow_j). Any thread
bool carry_puts(uintptr_t W, int* j) {
    if (!W || g_carry_old.load(std::memory_order_relaxed) || dual::carry_returning()) return false;
    int sj = -1;
    float T[16];
    if (dual::secondary_W(&sj) != W || sj < 0 || sj > 1 || dual::place_mode(T) != 4) return false;
    if (j) *j = sj;
    return true;
}

// [Hands] CarryByForegrip: the carried gun until its T is made (its put-away, then the front bone's settling): P (4x4
// rows, the gun as drawn at the hand-over) moved rigidly with John's front hand's target since, from (h0, R0) then to
// (h1, R1) now (axes as columns): each axis turned by Rd = R1 R0^T, the origin h1 + Rd (P's origin - h0), so the gun
// keeps its place and turn in that target (the review, round 36: the move alone left a turn of the hand out, and T kept
// it). P's fourth column is kept. The game thread; false when not finite.
bool carry_rigid(const float* P, const float* h0, const float* R0, const float* h1, const float* R1, float* out) {
    float Rd[9];
    for (int k = 0; k < 3; ++k)
        for (int m = 0; m < 3; ++m) Rd[k * 3 + m] = R1[k * 3] * R0[m * 3] + R1[k * 3 + 1] * R0[m * 3 + 1] + R1[k * 3 + 2] * R0[m * 3 + 2];
    float t[16];
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) t[i * 4 + k] = Rd[k * 3] * P[i * 4] + Rd[k * 3 + 1] * P[i * 4 + 1] + Rd[k * 3 + 2] * P[i * 4 + 2];
        t[i * 4 + 3] = P[i * 4 + 3];
    }
    const double d[3] = {static_cast<double>(P[12]) - h0[0], static_cast<double>(P[13]) - h0[1], static_cast<double>(P[14]) - h0[2]};
    for (int k = 0; k < 3; ++k) t[12 + k] = static_cast<float>(h1[k] + Rd[k * 3] * d[0] + Rd[k * 3 + 1] * d[1] + Rd[k * 3 + 2] * d[2]);
    t[15] = P[15];
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(t[r * 4 + k])) return false;
    std::memcpy(out, t, sizeof(t));
    return true;
}

// [Hands] CarryByForegrip: the carried long gun (a holstered item's W, as place_secondary's) placed at John's hand j by
// T, the gun in that hand's corrected bone's frame taken at the hand-over: the gun = T x the bone (row vectors), so it
// keeps its pose on the hand (the correction is a world map: corr(T B) = T corr(B)). The game's fourth column of m kept;
// its shot direction (W +0x920) as place_secondary sets it. The game thread inside the item update; SEH-guarded.
// rig: T is this update's candidate, the gun placed rigidly at that front target (its draws moved by it: held_delta);
// put: the game still puts the gun away (the item not yet holstered); prop: there, the matrix goes into prop (for the
// caller's o_obj_set_matrix), W +0x80 left the game's and no shot direction written (the review: as the in-hand
// put-away's, the carry never fires)
// [Hands] CarryEaseMs (2026-10-31, the off-hand build step 3, R10; rounds 55.7 / 55.8, the user: "positions still goes off
// if moving off hand fast"; r77 h4: moving, a one-step jump of 12-28 cm and 32-60 deg at the hand-over): the carried gun
// eased over this from where it was last drawn two-handed (the latest capture, g_two_T: the gun as drawn), not jumped
std::atomic<float> g_carry_ease_ms{150.0f};
std::atomic<uint64_t> g_carry_eased{0};
std::atomic<float> g_carry_ease_d{-1.0f};  // the first eased placement's distance from its target (m, "skel carry")
std::atomic<double> g_carry_ease_end{-1e12};  // the ease's end (log::now_ms), for actions' carried bolt frame
bool place_carry(uintptr_t W, float* m, int j, const float* T, const FrontPair* rig = nullptr, bool put = false, float* prop = nullptr) {
    auto refuse = [](int why) {
        g_carry_refused.fetch_add(1, std::memory_order_relaxed);
        g_sec_why.store(why, std::memory_order_relaxed);
        return false;
    };
    if (j < 0 || j > 1 || !pose::anchor_active()) return refuse(1);
    float R[9], A[9], a[3];
    double t[3], ad[3] = {}, cms = -1.0;
    if (!corrected_bone(j, R, t, A, a, ad, &cms)) return refuse(4);
    alignas(16) float out[16];
    if (!raw(reinterpret_cast<uintptr_t>(m), out, sizeof(out))) return refuse(5);  // (its fourth column kept)
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) out[i * 4 + k] = T[i * 4] * R[k] + T[i * 4 + 1] * R[3 + k] + T[i * 4 + 2] * R[6 + k];
    for (int k = 0; k < 3; ++k)  // in double: the bone's large translation plus T's small one turned
        out[12 + k] = static_cast<float>(static_cast<double>(T[12]) * R[k] + static_cast<double>(T[13]) * R[3 + k] +
                                         static_cast<double>(T[14]) * R[6 + k] + t[k]);
    for (int r = 0; r < 4; ++r)  // (the game's fourth column is its own data: not tested)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(out[r * 4 + k])) return refuse(6);
    {  // [Hands] CarryEaseMs: from the last two-handed place (the game thread's own statics)
        static uint64_t s_ser = ~0ull;
        static float s_from[16];
        static bool s_ok = false;
        static double s_t0 = 0.0;
        const double now = log::now_ms();
        const uint64_t ser = dual::carry_serial();
        static float s_h0[3], s_r0[9];
        const float em = g_carry_ease_ms.load(std::memory_order_relaxed);
        if (ser != s_ser) {
            s_ser = ser;
            s_ok = false;
            // (the review) the clock from the carry's start: a first placement after the ease's time eases nothing (the gun
            // already drawn at the carry by the put-away's own path, body's carry_rigid)
            s_t0 = dual::state().carry_ms;
            if (!(s_t0 > 0.0) || now - s_t0 > 1000.0) s_t0 = now;
            g_carry_ease_end.store(s_t0 + em, std::memory_order_relaxed);
            for (int tries = 0; tries < 8 && !s_ok && now - s_t0 < em; ++tries) {
                const uint32_t s0 = g_two_T_seq.load(std::memory_order_acquire);
                if (s0 & 1) continue;
                const TwoHandT c = g_two_T;
                if (g_two_T_seq.load(std::memory_order_acquire) != s0) continue;
                if (c.W != W || now - c.ms > 500.0 || !c.hand0_ok) break;
                std::memcpy(s_from, c.P, sizeof(s_from));
                std::memcpy(s_h0, c.hand0, sizeof(s_h0));
                std::memcpy(s_r0, c.rot0, sizeof(s_r0));
                s_ok = true;
                const float dx = out[12] - s_from[12], dy = out[13] - s_from[13], dz = out[14] - s_from[14];
                g_carry_ease_d.store(std::sqrt(dx * dx + dy * dy + dz * dz), std::memory_order_relaxed);
            }
        }
        const double e = now - s_t0;
        alignas(16) float from_now[16];
        BodyPoints ebp;
        // (the review) the start carried with the front target since the capture, so the eased gun moves with the hand
        const bool from_ok = s_ok && body_points(&ebp) && ebp.hand_ok[j] && carry_rigid(s_from, s_h0, s_r0, ebp.hand[j], ebp.target_rot[j], from_now);
        if (from_ok && em > 0.0f && e < em) {
            float f = static_cast<float>(e / em);
            f = f * f * (3.0f - 2.0f * f);  // (smoothstep)
            float m0[9], m1[9], q0[4], q1[4], q[4], mr[9];
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k) m0[i * 3 + k] = from_now[i * 4 + k], m1[i * 3 + k] = out[i * 4 + k];
            mat_to_q(m0, q0);
            mat_to_q(m1, q1);
            nlerp_q(q0, q1, f, q);
            q_to_mat(q, mr);
            alignas(16) float eo[16];
            std::memcpy(eo, out, sizeof(eo));
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k) eo[i * 4 + k] = mr[i * 3 + k];
            for (int k = 0; k < 3; ++k) eo[12 + k] = from_now[12 + k] + (out[12 + k] - from_now[12 + k]) * f;
            bool fin = true;
            for (int r = 0; r < 4 && fin; ++r)
                for (int k = 0; k < 3; ++k) fin = fin && std::isfinite(eo[r * 4 + k]);
            if (fin) {
                std::memcpy(out, eo, sizeof(out));
                g_carry_eased.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (prop) {
        std::memcpy(prop, out, sizeof(out));
    } else {
        if (!wraw(reinterpret_cast<uintptr_t>(m), out, sizeof(out))) return refuse(5);
        const float zl = std::sqrt(out[8] * out[8] + out[9] * out[9] + out[10] * out[10]);
        if (zl > 1e-6f) {
            const float dir[3] = {-out[8] / zl, -out[9] / zl, -out[10] / zl};
            if (!wraw(W + 0x920, dir, sizeof(dir))) return refuse(5);
        }
    }
    const FrontPair fp = rig ? *rig : pair_front(cms);  // ("skel carry trace": with T, the bone's own publish)
    g_sec_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing (its draws: the second gun's samples)
    g_sec_placed.W = W;
    g_sec_placed.build = g_vis_builds.load(std::memory_order_relaxed);
    std::memcpy(g_sec_placed.m, out, sizeof(out));
    std::memcpy(g_sec_placed.A, A, sizeof(A));
    std::memcpy(g_sec_placed.a, a, sizeof(a));
    std::memcpy(g_sec_placed.ad, ad, sizeof(ad));
    g_sec_placed.john = j;
    g_sec_placed.ms = log::now_ms();  // ([Reload] CarriedBolt: carry_frame)
    g_sec_placed.carry = true;
    g_sec_placed.follow_j = rig && rig->ok ? j : -1;
    g_sec_placed.put = put;
    put_pair(g_sec_placed, fp);
    g_sec_seq.fetch_add(1, std::memory_order_release);
    g_carry_placements.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool inv44(const float* m, float* out);  // below

// [Hands] CarryByForegrip: this update's two-handed long gun as drawn (g_place_buf), published for the hand-over. Only
// P: in the gun in hand's own update John's front hand's bone is not yet the frame's (the game's arm onto the foregrip
// comes after it: cycle r150, 0.18 m off), so T is made at the hand-over, against the bone as the frame left it, the
// one the carried gun's updates find (carry_T_from). The game thread.
void capture_two_hand(uintptr_t W, const float* P, int fj) {
    // the rows' xyz only (cycle r197): the fourth column is the game's own data, as place_carry keeps it (never tested, never used)
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(P[r * 4 + k])) {
                g_two_T_nonfinite.fetch_add(1, std::memory_order_relaxed);
                return;
            }
    if (!std::isfinite(P[3]) || !std::isfinite(P[7]) || !std::isfinite(P[11]) || !std::isfinite(P[15])) g_two_T_col4.fetch_add(1, std::memory_order_relaxed);
    BodyPoints bp;
    const bool bp_ok = body_points(&bp);
    if (fj < 0) fj = bp_ok && bp.gun == 0 ? 1 : 0;  // (-1: John's hand other than his gun hand)
    const bool hok = fj >= 0 && fj <= 1 && bp_ok && bp.hand_ok[fj];
    if (!hok) g_two_T_nohand.fetch_add(1, std::memory_order_relaxed);
    g_two_T_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    g_two_T.W = W;
    std::memcpy(g_two_T.P, P, sizeof(g_two_T.P));
    g_two_T.fj = fj;
    g_two_T.hand0_ok = hok;
    if (hok) {
        std::memcpy(g_two_T.hand0, bp.hand[fj], sizeof(g_two_T.hand0));
        std::memcpy(g_two_T.rot0, bp.target_rot[fj], sizeof(g_two_T.rot0));
    }
    g_two_T.ms = log::now_ms();
    {  // (round 55) both controllers still kTwoStillMs on end (as the hold's test in the draw: 0.15 m/s, 30 deg/s)
        constexpr double kTwoStillMs = 250.0, kSpeed = 0.15, kTurn = 0.52;
        static double s_ms = 0.0, s_from = -1.0, s_prev = 0.0;
        static float s_p[2][3] = {}, s_q[2][4] = {};
        const double now = g_two_T.ms, dt = (now - s_ms) * 0.001;
        if (now - s_prev > 100.0) {  // (the review) two-handed interrupted: no still capture of an earlier grip kept
            g_two_T_still.W = 0;
            s_from = -1.0;
        }
        s_prev = now;
        if (dt > 0.008 || dt < 0.0) {
            bool still = dt > 0.0 && dt < 0.2;
            for (int c = 0; c < 2; ++c) {
                const hands::Hand hc = hands::get(c);
                if (still) {
                    const double dx = hc.pos[0] - s_p[c][0], dy = hc.pos[1] - s_p[c][1], dz = hc.pos[2] - s_p[c][2];
                    const double dq = std::fabs(hc.rot[0] * s_q[c][0] + hc.rot[1] * s_q[c][1] + hc.rot[2] * s_q[c][2] + hc.rot[3] * s_q[c][3]);
                    const double turn = 2.0 * std::acos(dq > 1.0 ? 1.0 : dq);
                    still = std::sqrt(dx * dx + dy * dy + dz * dz) / dt < kSpeed && turn / dt < kTurn;
                }
                std::memcpy(s_p[c], hc.pos, sizeof(s_p[c]));
                std::memcpy(s_q[c], hc.rot, sizeof(s_q[c]));
            }
            s_ms = now;
            if (!still) s_from = -1.0;
            else if (s_from < 0.0) s_from = now;
            g_two_T_moving = !(still && now - s_from >= kTwoStillMs);
        }
        if (!g_two_T_moving && hok) g_two_T_still = g_two_T;
    }
    g_two_T_seq.fetch_add(1, std::memory_order_release);
    g_two_T_takes.fetch_add(1, std::memory_order_relaxed);
}

// T = P C^-1 (row vectors; its translation from the difference in double): P in John's hand fj's corrected bone's frame
// as the bone is now. False if the bone is not known, or T is not finite or out of the hand's reach
bool carry_T_from(const float* P, int fj, float T[16], double t_out[3]) {
    float R[9], A[9], a[3];
    double t[3], ad[3] = {};
    if (!corrected_bone(fj, R, t, A, a, ad)) return false;
    alignas(16) float Cm[16] = {R[0], R[1], R[2], 0, R[3], R[4], R[5], 0, R[6], R[7], R[8], 0, 0, 0, 0, 1}, Ci[16];
    if (!inv44(Cm, Ci)) return false;
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) T[i * 4 + k] = P[i * 4] * Ci[k] + P[i * 4 + 1] * Ci[4 + k] + P[i * 4 + 2] * Ci[8 + k];
        T[i * 4 + 3] = 0.0f;
    }
    const double d[3] = {P[12] - t[0], P[13] - t[1], P[14] - t[2]};
    for (int k = 0; k < 3; ++k) T[12 + k] = static_cast<float>(d[0] * Ci[k] + d[1] * Ci[4 + k] + d[2] * Ci[8 + k]);
    T[15] = 1.0f;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(T[i])) return false;
    if (t_out) std::memcpy(t_out, t, sizeof(t));
    return T[12] * T[12] + T[13] * T[13] + T[14] * T[14] < 1.5f * 1.5f;  // the gun within reach of the hand
}

// a row-vector affine inverse (the game's matrices): R^-1 and -t R^-1
bool inv44(const float* m, float* out) {
    const float a = m[0], b_ = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b_ * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-8f)) return false;
    const float k = 1.0f / det;
    const float r[9] = {(e * i - f * h) * k, (c * h - b_ * i) * k, (b_ * f - c * e) * k, (f * g - d * i) * k, (a * i - c * g) * k,
                        (c * d - a * f) * k, (d * h - e * g) * k, (b_ * g - a * h) * k, (a * e - b_ * d) * k};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) out[row * 4 + col] = r[row * 3 + col];
        out[row * 4 + 3] = 0.0f;
    }
    for (int col = 0; col < 3; ++col)
        out[12 + col] = -(m[12] * r[col] + m[13] * r[3 + col] + m[14] * r[6 + col]);
    out[15] = 1.0f;
    return true;
}
void mul44r(const float* a, const float* bm, float* out) {  // row vectors: out = a bm (affine)
    float t[16];
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            t[row * 4 + col] = a[row * 4] * bm[col] + a[row * 4 + 1] * bm[4 + col] + a[row * 4 + 2] * bm[8 + col] + a[row * 4 + 3] * bm[12 + col];
    std::memcpy(out, t, sizeof(t));
}

// [Hands] DualWieldCopy: the copy of the gun in hand (W, placed at P this update) in John's free hand j: C as
// place_secondary makes a second gun's matrix (the model's grip locator for that hand x the hand's attachment bone, then
// that hand's correction), never written to the game; T = P^-1 C (row vectors) goes to dual.cpp for the shot and the
// flash. The game thread inside the item update; SEH-guarded reads, no system calls.
bool place_copy(uintptr_t W, const float* P, int j) {
    auto refuse = [](int why) {
        g_copy_refused.fetch_add(1, std::memory_order_relaxed);
        g_copy_why.store(why, std::memory_order_relaxed);
        return false;
    };
    if (j < 0 || j > 1 || !pose::anchor_active()) return refuse(1);
    const int ab = g_att_bone[j].load(std::memory_order_relaxed);
    const uintptr_t skel = g_skel_game.load(std::memory_order_relaxed);
    uintptr_t mtx = 0, L = 0;
    alignas(16) float B[16], G[16], src[16], C[16], Pinv[16], T[16];
    if (ab < 0 || !skel || !raw(skel + 0x28, &mtx, sizeof(mtx)) || !mtx || !raw(mtx + static_cast<uintptr_t>(ab) * 0x40, B, sizeof(B)))
        return refuse(2);
    if (!raw(W + 0x338, &L, sizeof(L)) || !L || !raw(L + (j ? 0xc0 : 0x100), G, sizeof(G)) || !rigid_rows(G)) return refuse(3);
    grip_times(G, B, src);
    float A[9], a[3];
    double ad[3] = {};
    bool ik = false;
    if (!hand_correction_d(j, A, a, &ik, ad) || !ik) return refuse(4);
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) C[i * 4 + k] = A[k * 3] * src[i * 4] + A[k * 3 + 1] * src[i * 4 + 1] + A[k * 3 + 2] * src[i * 4 + 2];
        C[i * 4 + 3] = src[i * 4 + 3];
    }
    for (int k = 0; k < 3; ++k)
        C[12 + k] = static_cast<float>(static_cast<double>(A[k * 3]) * src[12] + static_cast<double>(A[k * 3 + 1]) * src[13] +
                                       static_cast<double>(A[k * 3 + 2]) * src[14] + ad[k]);
    C[15] = src[15];
    if (!inv44(P, Pinv)) return refuse(5);
    mul44r(Pinv, C, T);
    // [Hands] DualWieldOwnModel (run 5 item 2): the other sidearm's model held at C (held_prop slot 1, its draw moved onto
    // C); the shot moved on by the two models' muzzles' difference (the gun in hand's muzzle locator, W +0x338 -> [0] when
    // L +0x140 is set: an offset from bone [+0x20], the root: the model's axes; the other's from dual's table)
    float mshift[3] = {0.0f, 0.0f, 0.0f};  // (kept with the placement: the second reticle's muzzle, aim.cpp)
    if (dual::copy_model() >= 0) {  // another model (DualWieldOwnModel) or the same one (CopyAsProp): a held prop
        const char* frag = dual::sidearm_fragment(dual::copy_model());
        float pose[12];
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 3; ++k) pose[r * 3 + k] = C[r * 4 + k];  // the rows: the axes, then the position
        held_prop::want(1, frag, pose);
        float mo[3];
        uintptr_t loc = 0;
        uint8_t has = 0;
        int32_t bone = -1;
        float mg[3];
        if (frag && dual::sidearm_muzzle(dual::copy_model(), mo) && raw(L + 0x140, &has, 1) && has && raw(L, &loc, sizeof(loc)) && loc &&
            raw(loc, mg, sizeof(mg)) && raw(loc + 0x20, &bone, sizeof(bone)) && bone == 0) {
            const float d[3] = {mo[0] - mg[0], mo[1] - mg[1], mo[2] - mg[2]};
            for (int k = 0; k < 3; ++k) T[12 + k] += d[0] * C[k] + d[1] * C[4 + k] + d[2] * C[8 + k];  // d C (row vectors)
            std::memcpy(mshift, d, sizeof(mshift));
            g_copy_muzzle_moved.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_copy_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    std::memcpy(g_copy_placed.mshift, mshift, sizeof(mshift));
    g_copy_placed.W = W;
    g_copy_placed.build = g_vis_builds.load(std::memory_order_relaxed);
    std::memcpy(g_copy_placed.m, C, sizeof(C));
    std::memcpy(g_copy_placed.A, A, sizeof(A));
    std::memcpy(g_copy_placed.a, a, sizeof(a));
    std::memcpy(g_copy_placed.ad, ad, sizeof(ad));
    g_copy_placed.john = j;
    g_copy_seq.fetch_add(1, std::memory_order_release);
    g_copy_placements.fetch_add(1, std::memory_order_relaxed);
    dual::note_copy_placed(W, T);
    return true;
}

// Run 7 item 1c ([Hands] FixedGunGrip, off): the long gun held in the drawn hand as the game holds it while aiming. The
// placed gun's relation to the drawn wrist is the game's gun in its animated wrist (the hand's correction is rigid), and
// the game changes that between its aiming pose (the gun held by its grip) and its lowered and carry poses (held
// higher: the user's grip captures, research\round13\grip-*.jpg). Each long gun's aiming relation (the gun in the
// animated wrist's frame, rel = G W^-1 in row vectors) is learned while the game aims and steady, kept for the session
// and saved in the user ini ([Grips] GunHand.<weapon>), and the gun placed with it in every pose: G' = rel W. Before a
// gun's is learned: the built-in (measured in the simulator), else the last learned on any long gun (the template),
// else the game's own. The muzzle, the barrel ray, the shots, the copy and the second gun follow the placed gun.
std::atomic<bool> g_fixed_grip{false};
bool fixed_grip_on() { return g_fixed_grip.load(std::memory_order_relaxed); }
std::atomic<bool> g_fixed_sidearm{false};  // [Hands] FixedSidearmGrip: the sidearms held by their aiming hold too
struct GunRel {
    bool valid = false;
    float m[16] = {};  // the gun in the animated wrist's frame (rows: the axes, then the position)
};
GunRel g_gun_rel[kGripW];    // learned per eWeapon (game thread; copied out under g_fg_mutex)
GunRel g_gun_rel_tpl;        // the last learned on any long gun
GunRel g_gun_rel_tpl_side;   // the last learned on any sidearm (a sidearm never borrows a long gun's)
std::mutex g_fg_mutex;
struct FixedGripDiag {
    int weapon = -1, aiming = 0, src = -1, side = -1;  // src: 0 learned, 1 built-in, 2 template, 3 the game's, -1 off
    float now[16] = {};      // the game's gun in the animated wrist this update
    float placed[16] = {};   // the placed gun in the drawn wrist (the correction's map of the animated one)
    float aim_max_mm = 0, aim_max_deg = 0, low_max_mm = 0, low_max_deg = 0;  // the game's, against the learned, by pose
    float placed_max_mm = 0, placed_max_deg = 0;  // the placed relation against the learned (fixed: ~0)
    uint64_t learns = 0, fixed = 0, frames = 0, saved = 0;
    uint64_t by_src[3] = {};  // the frames placed by each hold: learned, built-in, template (0.8.1's test)
    float wrist_pos[3] = {}, gun_pos[3] = {}, att_pos[3] = {};  // the raw positions read (the matrices' spaces)
    int why = 0;  // the last early return: 1 no wrist bone, 2 the wrist not read, 3 not rigid, 4 not a long gun
};
FixedGripDiag g_fg_diag;
std::atomic<uint64_t> g_fg_aim_frames{0};
std::atomic<uint64_t> g_fg_tick_differs{0};  // placements whose item's gun was not the script tick's (a switch's frames)
bool is_long_gun_w(int w) { return w >= 8 && w <= 20; }
bool rigid_axes(const float* m) {  // the three axis rows unit and orthogonal (the position anywhere)
    for (int i = 0; i < 3; ++i) {
        const float* a = m + i * 4;
        const float l = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        if (!(l > 0.98f && l < 1.02f)) return false;
        for (int j = i + 1; j < 3; ++j) {
            const float* b = m + j * 4;
            if (!(std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) < 0.02f)) return false;
        }
    }
    return std::isfinite(m[12]) && std::isfinite(m[13]) && std::isfinite(m[14]);
}
void rel_diff(const float* a, const float* b, float* mm, float* deg) {  // position (mm) and turn (degrees) between two frames
    const float dx = a[12] - b[12], dy = a[13] - b[13], dz = a[14] - b[14];
    *mm = 1000.0f * std::sqrt(dx * dx + dy * dy + dz * dz);
    float tr = 0;  // trace of Ra Rb^T (rows are the axes)
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) tr += a[r * 4 + k] * b[r * 4 + k];
    const float c = std::fmax(-1.0f, std::fmin(1.0f, (tr - 1.0f) * 0.5f));
    *deg = std::acos(c) * 57.29578f;
}
void ortho_rows(float* m) {  // Gram-Schmidt on the three axis rows (after a blend)
    float* x = m;
    float* y = m + 4;
    float* z = m + 8;
    auto norm = [](float* v) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-6f)
            for (int k = 0; k < 3; ++k) v[k] /= l;
    };
    norm(x);
    const float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (int k = 0; k < 3; ++k) y[k] -= d * x[k];
    norm(y);
    z[0] = x[1] * y[2] - x[2] * y[1];
    z[1] = x[2] * y[0] - x[0] * y[2];
    z[2] = x[0] * y[1] - x[1] * y[0];
}
// the built-in aiming relations (2026-10-10): every gun's, learned in the headset with FixedGunGrip and FixedSidearmGrip
// on (the game's gun in its animated wrist while it aims: the game's data, the same for every player of this build; a
// second learning matched the first within 0.1 mm). As saved in [Grips] GunHand.<gun>: the three axis rows, then the
// position. A player's own learned hold still wins; a gun past the table (or a game update's new animation) learns its
// own as before.
bool builtin_gun_rel(int w, float* out) {
    static const float kRel[21][12] = {
        {1.00000f, 0.00000f, 0.00000f, 0.00000f, 0.17365f, -0.98481f, 0.00000f, 0.98481f, 0.17365f, -0.02760f, -0.12899f, -0.03158f},  // 0 Volcanic
        {1.00000f, 0.00000f, 0.00000f, 0.00000f, 0.17365f, -0.98481f, 0.00000f, 0.98481f, 0.17365f, -0.02762f, -0.11081f, 0.00379f},  // 1 SemiAutoPistol
        {1.00000f, 0.00000f, 0.00000f, 0.00000f, 0.17365f, -0.98481f, 0.00000f, 0.98481f, 0.17365f, -0.02762f, -0.10518f, -0.00115f},  // 2 HighPower
        {1.00000f, 0.00000f, 0.00000f, 0.00000f, 0.17365f, -0.98481f, 0.00000f, 0.98481f, 0.17365f, -0.02601f, -0.14740f, -0.02356f},  // 3 Mauser
        {1.00000f, 0.00000f, 0.00000f, 0.00000f, 0.17365f, -0.98481f, 0.00000f, 0.98481f, 0.17365f, -0.02762f, -0.12903f, -0.03157f},  // 4 Cattleman
        {1.00000f, 0.00000f, 0.00000f, 0.00000f, 0.17365f, -0.98481f, 0.00000f, 0.98481f, 0.17365f, -0.02759f, -0.12895f, -0.03148f},  // 5 Schofield
        {1.00000f, 0.00000f, 0.00000f, 0.00000f, 0.17365f, -0.98481f, 0.00000f, 0.98481f, 0.17365f, -0.02759f, -0.11804f, -0.03300f},  // 6 DoubleAction
        {1.00000f, 0.00000f, 0.00000f, 0.00000f, 0.17365f, -0.98481f, 0.00000f, 0.98481f, 0.17365f, -0.03069f, -0.12902f, -0.03931f},  // 7 LeMat
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02961f, -0.12136f, -0.02091f},  // 8 Carbine
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02260f, -0.12344f, -0.03239f},  // 9 Winchester
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02328f, -0.11655f, -0.02219f},  // 10 Henry
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02406f, -0.12627f, -0.01368f},  // 11 Evans
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02718f, -0.11370f, -0.02705f},  // 12 Springfield
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02670f, -0.13028f, -0.01593f},  // 13 BoltAction
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02866f, -0.11711f, -0.03045f},  // 14 Buffalo
        {0.99480f, 0.07976f, -0.06328f, -0.07835f, 0.20285f, -0.97607f, -0.06501f, 0.97596f, 0.20805f, -0.02410f, -0.15893f, -0.01720f},  // 15 SawedOff
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.03113f, -0.14251f, -0.04736f},  // 16 DoubleBarrel
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02764f, -0.11108f, -0.02622f},  // 17 PumpAction
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02257f, -0.12815f, -0.01560f},  // 18 SemiAutoShotgun
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02783f, -0.12408f, -0.03132f},  // 19 RollingBlock
        {0.96905f, 0.08393f, -0.23215f, -0.24578f, 0.41582f, -0.87561f, 0.02305f, 0.90557f, 0.42358f, -0.02048f, -0.11430f, 0.00993f},  // 20 Carcano
    };
    if (w < 0 || w >= 21) return false;
    const float* v = kRel[w];
    const int map[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
    std::memset(out, 0, sizeof(float) * 16);
    for (int i = 0; i < 12; ++i) out[map[i]] = v[i];
    out[15] = 1.0f;
    ortho_rows(out);  // as the user ini's are read (five decimals)
    return true;
}
// the learned holds saved: once a session per gun, and only with FixedGunGrip on (run 7 item 6's review: off, the hook
// wrote the user ini on every re-equip)
std::atomic<bool> g_gun_rel_saved[holster::kWeapons] = {};
struct GunRelSave {
    int w;
    float m[16];
};
void save_gun_rel(int w, const float* m);
DWORD WINAPI save_gun_rel_work(void* p) {  // a thread-pool work item: the ini written off the game thread
    GunRelSave* s = static_cast<GunRelSave*>(p);
    save_gun_rel(s->w, s->m);
    delete s;
    return 0;
}
void save_gun_rel(int w, const float* m) {
    if (w < 0 || w >= holster::kWeapons) return;
    char b[400];
    std::snprintf(b, sizeof(b), "%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9],
                  m[10], m[12], m[13], m[14]);
    config::set("Grips", (std::string("GunHand.") + holster::weapon_token(w)).c_str(), b);
}
void load_gun_rels() {
    for (int w = 0; w <= 20; ++w) {
        const std::string s = config::get_string("Grips", (std::string("GunHand.") + holster::weapon_token(w)).c_str(), "");
        float v[12];
        if (s.empty() || sscanf_s(s.c_str(), "%f %f %f %f %f %f %f %f %f %f %f %f", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7, v + 8,
                                     v + 9, v + 10, v + 11) != 12)
            continue;
        GunRel& g = g_gun_rel[w];
        const int map[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
        std::memset(g.m, 0, sizeof(g.m));
        for (int i = 0; i < 12; ++i) g.m[map[i]] = v[i];
        g.m[15] = 1.0f;
        ortho_rows(g.m);
        g.valid = rigid_axes(g.m) && g.m[12] * g.m[12] + g.m[13] * g.m[13] + g.m[14] * g.m[14] < 1.0f;
        if (g.valid) log::info("[body] FixedGunGrip: the %s's aiming hold read from the user ini", holster::weapon_token(w));
    }
}
// The game thread (hk_obj_set_matrix): src (the game's gun, this update) replaced by the aiming hold at the animated
// wrist when [Hands] FixedGunGrip is on and the gun is a long gun; always measured (the readback).
// (2026-11-02, GOAL-older-items item 21, a test) "skel grip learn off": the aiming hold's easing held (its learned hold
// kept as it is): whether that easing, fed each frame by the game's rounded matrices, moves the gun
std::atomic<bool> g_fg_learn{true};
void fixed_grip(int side, float* src, int item_w) {
    const uintptr_t skel = g_skel_game.load(std::memory_order_relaxed);
    const int wi = side >= 0 && side < 2 ? g_rig.wrist[side] : -1;
    uintptr_t mtx = 0;
    alignas(16) float Wr[16], Wi[16], rel[16], Ab[16];
    auto fail = [](int why) {
        std::lock_guard lock(g_fg_mutex);
        g_fg_diag.why = why;
    };
    if (wi < 0 || !skel) return fail(1);
    if (!raw(skel + 0x28, &mtx, sizeof(mtx)) || !mtx || !raw(mtx + static_cast<uintptr_t>(wi) * 0x40, Wr, sizeof(Wr))) return fail(2);
    const int ab = g_att_bone[side].load(std::memory_order_relaxed);
    const bool ab_ok = ab >= 0 && raw(mtx + static_cast<uintptr_t>(ab) * 0x40, Ab, sizeof(Ab));
    // affine 4x3: the game keeps other data in the fourth column (src [3] [7] [11]), so the products take it as 0 0 0 1
    alignas(16) float S[16];
    std::memcpy(S, src, sizeof(S));
    S[3] = S[7] = S[11] = 0.0f;
    S[15] = 1.0f;
    Wr[3] = Wr[7] = Wr[11] = 0.0f;
    Wr[15] = 1.0f;
    if (!rigid_axes(Wr) || !inv44(Wr, Wi)) return fail(3);
    // the gun is the placed item's own (item_w: its object's eWeapon): the script tick's weapon in hand is a frame
    // behind on a switch the mod makes (0.8.1 review: the new item placed by the last gun's hold, and that frame learned
    // into the last gun's); the tick's only when the item's could not be read
    RdrvrActorState ws{};
    const int tick_w = api::actor_state(&ws) && ws.valid ? ws.weapon : -1;
    const int w = item_w >= 0 ? item_w : tick_w;
    if (item_w >= 0 && tick_w != item_w) g_fg_tick_differs.fetch_add(1, std::memory_order_relaxed);
    const bool sidearm = w >= 0 && w <= 7;
    if (!is_long_gun_w(w) && !sidearm) return fail(4);
    const bool fix_on = sidearm ? g_fixed_sidearm.load(std::memory_order_relaxed) : g_fixed_grip.load(std::memory_order_relaxed);
    GunRel& tpl = sidearm ? g_gun_rel_tpl_side : g_gun_rel_tpl;
    mul44r(S, Wi, rel);
    const bool aiming = holster::aim_pose() || aim::aiming();  // the gun controller's aim pose (G +0x5d6 & 0x40), or the zoom
    std::lock_guard lock(g_fg_mutex);
    FixedGripDiag& d = g_fg_diag;
    // 0.8.1: the steady aim counted per gun and per draw (a gun drawn while the last one aimed, or drawn again while
    // aiming, learned from its first frame, the draw's, and skipped its built-in hold): from 0 on a gun change or after
    // a quarter second without a placement (put away, or not placed)
    static double last_ms = 0;
    const double now_ms = log::now_ms();
    if (d.weapon != w || now_ms - last_ms > 250.0) g_fg_aim_frames.store(0, std::memory_order_relaxed);
    last_ms = now_ms;
    if (d.weapon != w) {
        d = FixedGripDiag{};
        d.weapon = w;
    }
    const uint64_t af = aiming ? g_fg_aim_frames.fetch_add(1, std::memory_order_relaxed) + 1 : (g_fg_aim_frames.store(0), 0);
    ++d.frames;
    d.aiming = aiming ? 1 : 0;
    d.side = side;
    d.why = 0;
    std::memcpy(d.now, rel, sizeof(rel));
    for (int k = 0; k < 3; ++k) {
        d.wrist_pos[k] = Wr[12 + k];
        d.gun_pos[k] = src[12 + k];
        d.att_pos[k] = ab_ok ? Ab[12 + k] : 0.0f;
    }
    GunRel& g = g_gun_rel[w];
    // steady aiming: learn (an easing); [Reload] SnapToDrawnGun: not through the fire clip (it moves the gun in the hand)
    if (aiming && af > 20 && (g_fg_learn.load(std::memory_order_relaxed) || !g.valid) && rigid_axes(rel) && rel[12] * rel[12] + rel[13] * rel[13] + rel[14] * rel[14] < 1.0f &&
        !(fix_on && g_snap_drawn_cfg.load(std::memory_order_relaxed) && in_fire_clip())) {
        if (!g.valid) {
            std::memcpy(g.m, rel, sizeof(rel));
            g.valid = true;
        } else {
            for (int i = 0; i < 15; ++i) g.m[i] += 0.1f * (rel[i] - g.m[i]);
            ortho_rows(g.m);
        }
        tpl = g;
        if (++d.learns == 60 && fix_on && !g_gun_rel_saved[w].exchange(true)) {
            // a second of aiming, the switch on: saved once a session per gun, written by a work item (no file I/O here)
            GunRelSave* s = new GunRelSave{w, {}};
            std::memcpy(s->m, g.m, sizeof(s->m));
            if (QueueUserWorkItem(save_gun_rel_work, s, WT_EXECUTEDEFAULT)) {
                ++d.saved;
            } else {
                delete s;
                g_gun_rel_saved[w] = false;
            }
        }
    }
    float ref[16];
    int src_kind = 3;
    if (g.valid) {
        std::memcpy(ref, g.m, sizeof(ref));
        src_kind = 0;
    } else if (builtin_gun_rel(w, ref)) {
        src_kind = 1;
    } else if (tpl.valid) {
        std::memcpy(ref, tpl.m, sizeof(ref));
        src_kind = 2;
    }
    if (src_kind < 3) {
        float mm = 0, deg = 0;
        rel_diff(rel, ref, &mm, &deg);
        float& m1 = aiming ? d.aim_max_mm : d.low_max_mm;
        float& d1 = aiming ? d.aim_max_deg : d.low_max_deg;
        m1 = std::fmax(m1, mm);
        d1 = std::fmax(d1, deg);
        // for the headset's logs: the game's hold slid off the aiming hold (at most a line every 5 s)
        static double last_log = 0;
        const double now = log::now_ms();
        if ((mm > 10.0f || deg > 5.0f) && now - last_log > 5000.0) {  // at most a line every 5 s
            last_log = now;
            log::info("[grip] the %s's hold in the wrist %.1f mm and %.1f deg off its aiming hold (src %d): aiming %d, fixed %d",
                      holster::weapon_token(w), mm, deg, src_kind, aiming ? 1 : 0, fix_on ? 1 : 0);
        }
    }
    d.src = fix_on ? src_kind : -1;
    if (d.src >= 0 && d.src < 3 && rigid_axes(ref)) {
        ref[3] = ref[7] = ref[11] = 0.0f;
        ref[15] = 1.0f;
        alignas(16) float out[16];
        mul44r(ref, Wr, out);  // the gun at the animated wrist as the aiming pose holds it
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 3; ++k) src[r * 4 + k] = out[r * 4 + k];  // the game's fourth column kept
        ++d.fixed;
        ++d.by_src[d.src];
    }
}
void fixed_grip_placed(int side, const float* A, const double* ad, const float* placed) {  // the placed gun in the drawn wrist's frame
    const uintptr_t skel = g_skel_game.load(std::memory_order_relaxed);
    const int wi = side >= 0 && side < 2 ? g_rig.wrist[side] : -1;
    uintptr_t mtx = 0;
    alignas(16) float Wr[16], D[16], Di[16], rel[16], P[16];
    if (wi < 0 || !skel || !raw(skel + 0x28, &mtx, sizeof(mtx)) || !mtx || !raw(mtx + static_cast<uintptr_t>(wi) * 0x40, Wr, sizeof(Wr)))
        return;
    std::memcpy(P, placed, sizeof(P));
    P[3] = P[7] = P[11] = 0.0f;  // affine (as fixed_grip)
    P[15] = 1.0f;
    for (int i = 0; i < 3; ++i) {  // the drawn wrist: the correction's map of the animated one (as the gun's)
        for (int k = 0; k < 3; ++k) D[i * 4 + k] = A[k * 3] * Wr[i * 4] + A[k * 3 + 1] * Wr[i * 4 + 1] + A[k * 3 + 2] * Wr[i * 4 + 2];
        D[i * 4 + 3] = 0.0f;
    }
    for (int k = 0; k < 3; ++k)
        D[12 + k] = static_cast<float>(static_cast<double>(A[k * 3]) * Wr[12] + static_cast<double>(A[k * 3 + 1]) * Wr[13] +
                                       static_cast<double>(A[k * 3 + 2]) * Wr[14] + ad[k]);
    D[15] = 1.0f;
    if (!inv44(D, Di)) return;
    mul44r(P, Di, rel);
    std::lock_guard lock(g_fg_mutex);
    FixedGripDiag& d = g_fg_diag;
    std::memcpy(d.placed, rel, sizeof(rel));
    if (d.weapon >= 0 && d.weapon < kGripW && g_gun_rel[d.weapon].valid) {
        float mm = 0, deg = 0;
        rel_diff(rel, g_gun_rel[d.weapon].m, &mm, &deg);
        d.placed_max_mm = std::fmax(d.placed_max_mm, mm);
        d.placed_max_deg = std::fmax(d.placed_max_deg, deg);
    }
}

// The game thread: the player's held prop placed at the drawn hand (see g_held_at_hand_cfg), and the second gun's.
// the lasso's draw: its rope's points draws below it are the lasso's
void hk_lasso_draw(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    g_lasso_draws.fetch_add(1, std::memory_order_relaxed);
    ++t_lasso_draw;
    o_lasso_draw(a, b, c, d, e, f);
    --t_lasso_draw;
}

// (2026-11-03, round 78, the user: "Lasso has a full put away animation"; cycle N45: its draws are its rope's, never a small
// prop's) while the lasso a mod put-away let go is on its way (leaving, weapon 21), its rope is drawn out of sight (500 m
// down) until it is holstered. (Cycle N47: the coil at the belt is not drawn as a rope there, twice only, after the
// put-away: none to draw it as; the counts kept)
extern std::atomic<int> g_leave_weapon;  // (below, with the put-away's hide)
uintptr_t leave_live();
bool root_ok();
alignas(16) float g_coil[0x800 / 4];  // (the render thread) the coil's points, the root's frame
int32_t g_coil_n = 0;
uint64_t g_coil_frame = 0;
std::atomic<uint64_t> g_leave_rope{0}, g_leave_rope_coil{0}, g_coil_kept{0}, g_coil_far{0};
bool hk_rope_leaving(uintptr_t lod, uint64_t cam, uintptr_t model, uintptr_t pts, uintptr_t ld, uint32_t p6, uint32_t p7) {
    if (!t_lasso_draw || !pts || !g_frame.valid || !root_ok() ||
        *reinterpret_cast<const DWORD*>(anchors::addr(anchors::Id::RenderThreadId)) != GetCurrentThreadId())
        return false;
    alignas(16) static thread_local float buf[0x810 / 4];
    int32_t n = 0;
    if (!raw(pts + 0x800, &n, sizeof(n)) || n <= 0 || n > 128 || !raw(pts, buf, 0x804)) return false;
    const float* w = g_frame.w_root;
    bool leaving = leave_live() && g_leave_weapon.load(std::memory_order_relaxed) == 21;
    if (leaving) {  // (the review: John's lasso only, its rope within 1.5 m of him)
        const float ex = buf[0] - g_frame.actor_world[0], ez = buf[2] - g_frame.actor_world[2];
        leaving = g_frame.actor_valid && ex * ex + ez * ez <= 1.5f * 1.5f;
    }
    if (!leaving) {
        if (g_frame.weapon == 21) return false;
        // (cycle N46: no coil kept; the root's place may not be the world's) another's lasso: its rope over 1.5 m from John
        const float dx = buf[0] - g_frame.actor_world[0], dz = buf[2] - g_frame.actor_world[2];
        if (!g_frame.actor_valid || dx * dx + dz * dz > 1.5f * 1.5f) {
            g_coil_far.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        g_coil_kept.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {  // world -> the root's frame: (p - t) R^T
            const float* p = buf + i * 4;
            const float d[3] = {p[0] - w[12], p[1] - w[13], p[2] - w[14]};
            float* q = g_coil + i * 4;
            for (int c = 0; c < 3; ++c) q[c] = d[0] * w[c * 4] + d[1] * w[c * 4 + 1] + d[2] * w[c * 4 + 2];
            q[3] = p[3];
        }
        g_coil_n = n;
        g_coil_frame = g_body_frame;
        return false;  // (drawn as the game draws it)
    }
    const bool coil = false;  // (N47: no coil drawn as a rope at the belt to draw it as)
    const int m = coil ? g_coil_n : n;
    for (int i = 0; i < m; ++i) {
        float* p = buf + i * 4;
        if (coil) {
            const float* q = g_coil + i * 4;
            for (int c = 0; c < 3; ++c) p[c] = q[0] * w[c] + q[1] * w[4 + c] + q[2] * w[8 + c] + w[12 + c];
            p[3] = q[3];
        } else {
            p[1] -= 500.0f;
        }
    }
    if (coil) {
        std::memcpy(buf + 0x800 / 4, &m, sizeof(m));
        g_leave_rope_coil.fetch_add(1, std::memory_order_relaxed);
    }
    g_leave_rope.fetch_add(1, std::memory_order_relaxed);
    o_rope_points(lod, cam, model, reinterpret_cast<uintptr_t>(buf), ld, p6, p7);
    return true;
}

// The rope's points draw (render thread): inside the lasso's draw, a corrected copy of its points
void hk_rope_points(uintptr_t lod, uint64_t cam, uintptr_t model, uintptr_t pts, uintptr_t ld, uint32_t p6, uint32_t p7) {
    if (hk_rope_leaving(lod, cam, model, pts, ld, p6, p7)) return;  // (round 78) a leaving lasso's rope, and the coil kept
    // only on the render thread (the corrections and the frame are its own)
    // and only with the lasso in hand (eWeapon 21): the coil at the belt is drawn here too, near the game's hands at the hip
    if (!t_lasso_draw || !pts || !g_lasso_cfg.load(std::memory_order_relaxed) || !g_corr_world_valid || !g_frame.valid || g_frame.weapon != 21 ||
        *reinterpret_cast<const DWORD*>(anchors::addr(anchors::Id::RenderThreadId)) != GetCurrentThreadId())
        return o_rope_points(lod, cam, model, pts, ld, p6, p7);
    alignas(16) static thread_local float buf[0x810 / 4];
    int32_t n = 0;
    if (!raw(pts + 0x800, &n, sizeof(n)) || n <= 0 || n > 128 || !raw(pts, buf, 0x804)) return o_rope_points(lod, cam, model, pts, ld, p6, p7);
    // the game's wrists (their attachment bones' positions this frame) and their world corrections
    float wp[2][3];
    const Corr* wc[2] = {nullptr, nullptr};
    for (int h = 0; h < 2; ++h) {
        const int wb = g_rig.att_wrist[h] >= 0 ? g_rig.att_wrist[h] : g_rig.wrist[h];
        wp[h][0] = wp[h][1] = wp[h][2] = 1e9f;
        for (size_t i = 0; i < g_rig.attach.size() && i * 3 + 2 < g_frame.attach_pos.size(); ++i)
            if (g_rig.attach[i] == wb) std::memcpy(wp[h], &g_frame.attach_pos[i * 3], sizeof(wp[h]));
        if (wb >= 0 && wb < static_cast<int>(g_corr_world.size())) wc[h] = &g_corr_world[wb];
    }
    // (2026-10-31, the off-hand build step 1, R16; r78: drawn by the left controller the rope's first point was 0.12 m from
    // the empty drawn right hand) a transplanted lasso: the game simulates the rope at the wrist holding it, so the points
    // there move by the held item's correction (to the drawn hand holding it), not that wrist's own
    if (g_item_xfer)
        if (const HeldSample* hs = newest_held(g_frame.held)) {
            const int ih = hs->left ? 0 : 1;
            if (wc[ih]) wc[ih] = &g_item_world[ih];
        }
    int moved = 0;
    float first[3], last[3];
    std::memcpy(first, buf, sizeof(first));
    std::memcpy(last, buf + (n - 1) * 4, sizeof(last));
    for (int i = 0; i < n; ++i) {
        float* p = buf + i * 4;
        // how much it moves: by the nearer hand's distance (full within 0.5 m: the coil hangs to 0.45 m from its wrist;
        // none past 1.0 m: a thrown loop keeps the game's place); which hand moves it: each by 1 / d^4 (the nearer
        // rules; halfway between them, the rope stretched between both)
        float dist[2] = {1e9f, 1e9f};
        for (int h = 0; h < 2; ++h) {
            if (!wc[h]) continue;
            const float dx = p[0] - wp[h][0], dy = p[1] - wp[h][1], dz = p[2] - wp[h][2];
            dist[h] = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        const float dn = std::fmin(dist[0], dist[1]);
        const float amount = dn <= 0.5f ? 1.0f : dn >= 1.0f ? 0.0f : (1.0f - dn) / 0.5f;
        if (amount <= 0.0f) continue;
        float share[2] = {0, 0};
        {
            const float i0 = wc[0] ? 1.0f / std::fmax(1e-4f, dist[0] * dist[0] * dist[0] * dist[0]) : 0.0f;
            const float i1 = wc[1] ? 1.0f / std::fmax(1e-4f, dist[1] * dist[1] * dist[1] * dist[1]) : 0.0f;
            share[0] = i0 / (i0 + i1);
            share[1] = i1 / (i0 + i1);
        }
        float q[3] = {p[0], p[1], p[2]};
        for (int h = 0; h < 2; ++h) {
            if (!wc[h] || share[h] <= 0.0f) continue;
            const Corr& c = *wc[h];
            for (int k = 0; k < 3; ++k) {
                const float t = c.A[k * 3] * p[0] + c.A[k * 3 + 1] * p[1] + c.A[k * 3 + 2] * p[2] + c.a[k];
                q[k] += amount * share[h] * (t - p[k]);
            }
        }
        std::memcpy(p, q, sizeof(q));
        ++moved;
    }
    g_lasso_points.fetch_add(1, std::memory_order_relaxed);
    if (moved) g_lasso_moved.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_lasso_mutex);
        std::memcpy(g_lasso_dbg[0], first, sizeof(first));
        std::memcpy(g_lasso_dbg[1], last, sizeof(last));
        std::memcpy(g_lasso_dbg[2], wp[0], sizeof(wp[0]));
        std::memcpy(g_lasso_dbg[3], wp[1], sizeof(wp[1]));
        std::memcpy(g_lasso_dbg[4], buf, 12);
        std::memcpy(g_lasso_dbg[5], buf + (n - 1) * 4, 12);
        g_lasso_n = n;
        g_lasso_moved_n = moved;
    }
    o_rope_points(lod, cam, model, reinterpret_cast<uintptr_t>(buf), ld, p6, p7);
}

// (2026-11-03, round 77, the user: the instant holster's step taken back "is not good enough ... Find a way to hide the put
// away animation without letting go of a sidearm moving the character at all") the mod's put-aways are the game's own again
// (ACTOR_PUT_ITEM_AWAY: John not stepped, InstantPutAway off), and what its clip shows is hidden: the item leaves the hand
// at once (the manager's item in hand cleared) but is drawn some 0.2-0.65 s more at the game's animated hand (state 3,
// research/run33/offhand_rules.md R3). put_away_leaving: the item in hand now (the weapon manager's +0x80) noted; each
// placement of its W while it is drawn and no longer in hand kept (the last kLeaveRing matrices, the game thread); the
// draws at those matrices hidden (their axes shrunk, as a hidden hat's: the draw only, no game state), until it is
// holstered or stowed, taken into the hand again, or 1.5 s. ShowGuns' copy at its holster shows from the let-go on
constexpr int kLeaveRing = 4;
std::atomic<uintptr_t> g_leave_item{0};
std::atomic<double> g_leave_until{0.0}, g_leave_begun{0.0};
// (the review) the leaving item while its window lasts (every reader checks the 1.5 s itself: leave_note, which ends it,
// runs only at the gun's placement)
uintptr_t leave_live() {
    const uintptr_t li = g_leave_item.load(std::memory_order_acquire);
    return li && log::now_ms() <= g_leave_until.load(std::memory_order_relaxed) ? li : 0;
}
std::mutex g_leave_mx;
float g_leave_m[kLeaveRing][16];
uint64_t g_leave_m_frame[kLeaveRing] = {};
int g_leave_m_n = 0;  // (under g_leave_mx) the next ring entry
std::atomic<uint64_t> g_leave_puts{0}, g_leave_updates{0}, g_leave_hidden{0}, g_leave_ends[5] = {};  // ends: holstered, in hand again, timed out, none read, another put-away
std::atomic<uint64_t> g_leave_near{0};
std::atomic<uint64_t> g_leave_at[3] = {};  // the counts at the put-away: its kept, hidden, missed
std::atomic<int> g_leave_weapon{-1};
std::atomic<uintptr_t> g_leave_last_W{0};  // (round 79) the last leaving item's W, its flame kept away until g_leave_last_until
std::atomic<double> g_leave_last_until{0.0};
void leave_end(int why) {
    const uintptr_t was = g_leave_item.load(std::memory_order_acquire);
    uintptr_t wW = 0;
    if (was && raw(was + 0xa0, &wW, sizeof(wW)) && wW) {
        g_leave_last_W.store(wW, std::memory_order_relaxed);
        g_leave_last_until.store(log::now_ms() + 1000.0, std::memory_order_relaxed);
    }
    if (!g_leave_item.exchange(0) || why < 0 || why >= 5) return;
    g_leave_ends[why].fetch_add(1, std::memory_order_relaxed);
    static const char* const kWhy[5] = {"holstered", "taken into the hand again", "1.5 s passed", "its state not read", "another put-away begun"};
    log::info("[holster] the put-away's item (weapon %d) on its way: %llu placements kept, %llu draws hidden, %llu of its draws missed; %s",
              g_leave_weapon.load(std::memory_order_relaxed), static_cast<unsigned long long>(g_leave_updates.load() - g_leave_at[0]),
              static_cast<unsigned long long>(g_leave_hidden.load() - g_leave_at[1]), static_cast<unsigned long long>(g_leave_near.load() - g_leave_at[2]),
              kWhy[why]);
}
// (game thread, hk_obj_set_matrix) W's placement: kept while it is the leaving item's, drawn, not in hand
void leave_note(uintptr_t W, const float* m) {
    const uintptr_t li = g_leave_item.load(std::memory_order_acquire);
    if (!li) return;
    const double now = log::now_ms();
    if (now > g_leave_until.load(std::memory_order_relaxed)) return leave_end(2);
    uintptr_t iw = 0, hand = 0;
    int32_t state = 0;
    const uintptr_t wmgr = g_held_wmgr.load(std::memory_order_relaxed);
    if (!raw(li + 0xa0, &iw, sizeof(iw)) || iw != W) return;  // (another weapon object's placement)
    if (!raw(li + 0x24, &state, sizeof(state)) || !wmgr || !raw(wmgr + 0x80, &hand, sizeof(hand))) return leave_end(3);
    if (hand == li) {  // still the item in hand: the put-away not made yet, or taken again
        if (now - g_leave_begun.load(std::memory_order_relaxed) > 400.0) leave_end(1);
        return;
    }
    if (state != 3) return leave_end(0);  // holstered (2) or stowed (1): drawn where the game hangs it from here
    float mm[16];
    if (!raw(reinterpret_cast<uintptr_t>(m), mm, sizeof(mm))) return;
    std::lock_guard lock(g_leave_mx);
    std::memcpy(g_leave_m[g_leave_m_n], mm, sizeof(mm));
    g_leave_m_frame[g_leave_m_n] = g_body_frame;
    g_leave_m_n = (g_leave_m_n + 1) % kLeaveRing;
    g_leave_updates.fetch_add(1, std::memory_order_relaxed);
}
// (the render thread, DrawVisEntity) a draw at one of the leaving item's kept placements (within 2 mm, as held_match).
// (the test's measure) a draw of the same instance as a hidden one (draw_instance), while the item leaves, not hidden:
// a draw of it the hide missed (g_leave_near, above)
std::atomic<uintptr_t> g_leave_inst{0};
// (2026-11-04, round 81, the user: the knife "works the first time you put it away, after that it plays the animation
// again"; the long guns "play a holster animation and did in the last build too". Their log: each item's first put-away
// hidden (no holstered place known yet), every later one drawn at its kept holstered place, which the animation moved)
// a leaving item's draws hidden, never drawn at a kept place (that way kept for the test only: "holster hold subst on")
std::atomic<bool> g_leave_subst_on{false};
// (round 81, the user: the torch's model "flickers once at custom holster position when you let go of it"; their log: a
// draw of another model near it, not matched, at the let-go) the item in hand's drawable (the render thread's, as its
// draws are matched as the held item), taken as the leaving item's at its put-away: during the put-away, a draw of that
// model near John not at a kept placement (the holstered one drawn once at the let-go) hidden too
std::atomic<uintptr_t> g_held_drawable{0}, g_leave_drawable{0};
std::atomic<double> g_held_drawable_ms{0.0};
std::atomic<uint64_t> g_leave_model_hidden{0};
std::atomic<int> g_leave_model_told{0};
// (the review) only the items (the lasso, the knife, the torch, the throwables): a gun's model is also the mod's own
// copies' (ShowGuns at the holster, the other revolver), which must show; only once the game has taken it out of the hand
// (a placement kept this put-away: a refused or late put-away leaves it seen in the hand); only on John (within 25 cm of
// one of his attachment bones: another's of the same model beside him is not)
bool leave_item_kind(int w) { return w == 21 || w == 22 || w == 33 || w == 23 || w == 24 || w == 25 || w == 29 || (w >= 35 && w <= 37); }
bool leave_model(void* const* rec) {
    const uintptr_t d = reinterpret_cast<uintptr_t>(rec[0x10]);
    if (!d || d != g_leave_drawable.load(std::memory_order_relaxed) || !leave_live() || !leave_item_kind(g_leave_weapon.load(std::memory_order_relaxed)) ||
        g_leave_updates.load(std::memory_order_relaxed) <= g_leave_at[0].load(std::memory_order_relaxed))
        return false;
    const float* rc = reinterpret_cast<const float*>(rec);
    const float dx = rc[12] - g_frame.actor_world[0], dy = rc[13] - g_frame.actor_world[1], dz = rc[14] - g_frame.actor_world[2];
    if (!(dx * dx + dy * dy + dz * dz < 1.5f * 1.5f)) return false;  // (an NPC's of the same model: not John's)
    bool on_john = false;
    for (size_t i = 0; !on_john && i * 3 + 2 < g_frame.attach_pos.size(); ++i) {
        const float* a = &g_frame.attach_pos[i * 3];
        const float ex = rc[12] - a[0], ey = rc[13] - a[1], ez = rc[14] - a[2];
        on_john = ex * ex + ey * ey + ez * ez < 0.25f * 0.25f;
    }
    if (!on_john) return false;
    g_leave_model_hidden.fetch_add(1, std::memory_order_relaxed);
    if (g_leave_model_told.fetch_add(1, std::memory_order_relaxed) == 0)
        log::info("[holster] the leaving item's own model drawn away from its way (%.2f m from John, %.0f ms after the let-go): hidden",
                  static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)), log::now_ms() - g_leave_begun.load(std::memory_order_relaxed));
    return true;
}
bool leave_draw(const float* rc) {
    if (!leave_live()) return false;
    std::lock_guard lock(g_leave_mx);
    for (int i = 0; i < kLeaveRing; ++i) {
        if (!g_leave_m_frame[i] || g_body_frame - g_leave_m_frame[i] > 4) continue;
        const float* h = g_leave_m[i];
        float d = 0.0f;
        for (int k = 0; k < 3; ++k) d = std::fmax(d, std::fmax(std::fabs(rc[12 + k] - h[12 + k]), std::fabs(rc[k] - h[k])));
        if (d < 2e-3f) return true;
    }
    return false;
}
// (2026-11-03, round 78, the user: the knife "sort of flickers before holstering", the torch too: hidden on its way, it is
// gone some 0.3 s, then at its sheath; a gun's shown copy at its holster covers that) the small props on John, not in hand,
// matched to one of his bones (a holstered knife, torch, lasso, gun), each instance's last record matrix kept in his
// root bone's frame (the game's, g_frame.w_root: rows the axes, then the position); a leaving item's draws are then drawn
// there instead (moved by the root since, then the prop path's correction as any holstered prop's), else hidden
struct HolsterSeen {
    uintptr_t inst = 0;  // (cycle N45: the holstered knife is another instance than the drawn one: its drawable, the model)
    uint64_t frame = 0;
    int bone = -1;       // (the review) the bone it was matched to: its place used only while still by that bone
    float rel[16] = {};
    // (cycle N51: in the root's frame it was drawn 109 mm from where the game then hung it: the holster clip sways the root)
    // its place from its bone's attachment point (world) and its axes, both at John's heading then (the root's yaw)
    float off[3] = {}, ax[9] = {}, yaw = 0.0f;
};
// the root's heading (its z axis about the world's up)
float root_yaw() { return std::atan2(g_frame.w_root[8], g_frame.w_root[10]); }
bool arm_bone(int b) {  // either arm's: its upper arm's subtree (the elbow, the forearm, the hand)
    if (b < 0) return true;
    for (int h = 0; h < 2; ++h)
        for (int x : g_rig.arm_set[h])
            if (x == b) return true;
    return (b < static_cast<int>(g_rig.in_elbow.size()) && g_rig.in_elbow[b]) || (b < static_cast<int>(g_rig.in_wrist.size()) && g_rig.in_wrist[b]);
}
bool attach_of(int bone, float* p) {
    for (size_t i = 0; i < g_rig.attach.size() && i * 3 + 2 < g_frame.attach_pos.size(); ++i)
        if (g_rig.attach[i] == bone) {
            std::memcpy(p, &g_frame.attach_pos[i * 3], 3 * sizeof(float));
            return true;
        }
    return false;
}
HolsterSeen g_hseen[16];  // (the render thread)
std::atomic<uint64_t> g_leave_subst{0};
// (round 79, the test's measure) the last substituted draw's place (world) and model, against that model's first holstered
// draw once its put-away has ended: how far the place it was drawn at on its way is from where it is then hung (mm)
uintptr_t g_subst_model = 0;
float g_subst_pos[3] = {};
std::atomic<float> g_subst_err_mm{-1.0f};
// (round 80, the test's) the substituted place's travel during one put-away (from its first, John's place taken out: mm)
uint64_t g_subst_puts = ~0ull;
float g_subst_first[3] = {};
std::atomic<float> g_subst_travel_mm{-1.0f};
std::atomic<uint64_t> g_subst_checks{0};
// m (row 4x4: the axes, then the position) times the rigid R's inverse (out = m R^-1), or times R (out = m R)
void mul_rigid(const float* m, const float* R, float* out, bool inverse) {
    float Ri[16];
    if (inverse) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) Ri[r * 4 + c] = R[c * 4 + r];
        for (int c = 0; c < 3; ++c) Ri[12 + c] = -(R[12] * Ri[c] + R[13] * Ri[4 + c] + R[14] * Ri[8 + c]);
        Ri[3] = Ri[7] = Ri[11] = 0.0f;
        Ri[15] = 1.0f;
        R = Ri;
    }
    float o[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) o[r * 4 + c] = m[r * 4] * R[c] + m[r * 4 + 1] * R[4 + c] + m[r * 4 + 2] * R[8 + c] + m[r * 4 + 3] * R[12 + c];
    std::memcpy(out, o, sizeof(o));
}
bool root_ok() {
    const float* w = g_frame.w_root;
    float s = 0.0f;
    for (int k = 0; k < 16; ++k) s += std::fabs(w[k]);
    bool fin = s > 0.5f;
    for (int k = 0; k < 16 && fin; ++k) fin = std::isfinite(w[k]);
    return fin;
}
void hseen_note(uintptr_t inst, const float* rc, int bone) {
    if (!inst || !root_ok() || leave_live()) return;  // (the review: not while an item leaves, its missed draws by the hip)
    // (round 80) hung by the nearest bone within 20 cm that is not an arm's (the empty hand's forearm hangs by the hip)
    bone = -1;
    float bd = 0.20f * 0.20f;
    for (size_t i = 0; i < g_rig.attach.size() && i * 3 + 2 < g_frame.attach_pos.size(); ++i) {
        const int ab = g_rig.attach[i];
        if (arm_bone(ab)) continue;
        const float* a = &g_frame.attach_pos[i * 3];
        const float dx = rc[12] - a[0], dy = rc[13] - a[1], dz = rc[14] - a[2], d = dx * dx + dy * dy + dz * dz;
        if (d < bd) {
            bd = d;
            bone = ab;
        }
    }
    if (bone < 0) return;
    HolsterSeen* e = nullptr;
    for (HolsterSeen& h : g_hseen)
        if (h.inst == inst) e = &h;
    if (!e) {
        e = &g_hseen[0];
        for (HolsterSeen& h : g_hseen)
            if (h.frame < e->frame) e = &h;
    }
    float m[16];
    std::memcpy(m, rc, sizeof(m));
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
    float at[3];
    if (!attach_of(bone, at)) return;
    mul_rigid(m, g_frame.w_root, e->rel, true);
    for (int k = 0; k < 3; ++k) e->off[k] = rc[12 + k] - at[k];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) e->ax[r * 3 + c] = rc[r * 4 + c];
    e->yaw = root_yaw();
    e->inst = inst;
    e->frame = g_body_frame;
    e->bone = bone;
    if (g_subst_model && inst == g_subst_model) {  // (the measure) its first holstered draw after its way
        const float dx = rc[12] - g_subst_pos[0], dy = rc[13] - g_subst_pos[1], dz = rc[14] - g_subst_pos[2];
        g_subst_err_mm.store(1000.0f * std::sqrt(dx * dx + dy * dy + dz * dz), std::memory_order_relaxed);
        g_subst_checks.fetch_add(1, std::memory_order_relaxed);
        g_subst_model = 0;
    }
}
// a leaving item's draw (leave_draw true) put at its holstered place: true when substituted, the record saved in the caller's
// own `orig` for its restore after the draw (*at; per call, as rec_save: the review, calls nest)
bool leave_subst(void* const* rec, float** at, float* orig) {
    const uintptr_t inst = reinterpret_cast<uintptr_t>(rec[0x10]);  // (its model: as hseen_note keys them)
    if (!inst || !root_ok()) return false;
    for (const HolsterSeen& h : g_hseen) {
        if (h.inst != inst || !h.frame) continue;
        float* rc = reinterpret_cast<float*>(const_cast<void**>(rec));
        // at its bone's attachment point now, its offset and axes turned by John's turn since (about the world's up)
        float bp[3];
        if (!attach_of(h.bone, bp)) return false;
        const float dy = root_yaw() - h.yaw, cy = std::cos(dy), sy = std::sin(dy);
        auto turn = [&](const float* v, float* o) {  // (a turn by dy about y: as the root's z axis turned from yaw to yaw + dy)
            o[0] = cy * v[0] + sy * v[2];
            o[1] = v[1];
            o[2] = -sy * v[0] + cy * v[2];
        };
        float m[16] = {};
        for (int r = 0; r < 3; ++r) turn(h.ax + r * 3, m + r * 4);
        float o[3];
        turn(h.off, o);
        for (int k = 0; k < 3; ++k) m[12 + k] = bp[k] + o[k];
        m[15] = 1.0f;
        bool fin = true;
        for (float x : m) fin = fin && std::isfinite(x);
        // (the review) only near the bone it hung from (an offset over 20 cm, hseen_note's reach: not a holstered place)
        if (!fin || h.off[0] * h.off[0] + h.off[1] * h.off[1] + h.off[2] * h.off[2] > 0.20f * 0.20f) return false;
        if (!*at) {
            std::memcpy(orig, rc, 32 * sizeof(float));
            *at = rc;
        }
        for (int k = 0; k < 15; ++k)
            if (k != 3 && k != 7 && k != 11) rc[k] = m[k];
        std::memcpy(rc + 16, rc, 16 * sizeof(float));  // (+0x40, last frame's: the same, no smear)
        g_leave_subst.fetch_add(1, std::memory_order_relaxed);
        g_subst_model = inst;  // (the measure: the game's own place for it, before the prop path's correction)
        std::memcpy(g_subst_pos, m + 12, sizeof(g_subst_pos));
        {  // (round 80, the measure) its travel during this put-away, John's own place taken out
            const float rel[3] = {m[12] - g_frame.actor_world[0], m[13] - g_frame.actor_world[1], m[14] - g_frame.actor_world[2]};
            const uint64_t pn = g_leave_puts.load(std::memory_order_relaxed);
            if (g_subst_puts != pn) {
                g_subst_puts = pn;
                std::memcpy(g_subst_first, rel, sizeof(rel));
                g_subst_travel_mm.store(0.0f, std::memory_order_relaxed);
            } else {
                const float dx = rel[0] - g_subst_first[0], dy = rel[1] - g_subst_first[1], dz = rel[2] - g_subst_first[2];
                const float t = 1000.0f * std::sqrt(dx * dx + dy * dy + dz * dz);
                if (t > g_subst_travel_mm.load(std::memory_order_relaxed)) g_subst_travel_mm.store(t, std::memory_order_relaxed);
            }
        }
        return true;
    }
    return false;
}
// (round 78, the lasso: 38 placements kept, none of its draws matched, cycle N44) any draw within 30 cm of the leaving
// item's newest placement and not matched: its bone count and its distances, logged (a few a put-away)
std::atomic<int> g_leave_probe_left{0};
void leave_probe(void* const* rec, int count) {
    if (g_leave_probe_left.load(std::memory_order_relaxed) <= 0 || !leave_live()) return;
    const float* rc = reinterpret_cast<const float*>(rec);
    float h[16];
    {
        std::lock_guard lock(g_leave_mx);
        int newest = -1;
        for (int i = 0; i < kLeaveRing; ++i)
            if (g_leave_m_frame[i] && g_body_frame - g_leave_m_frame[i] <= 4 && (newest < 0 || g_leave_m_frame[i] > g_leave_m_frame[newest])) newest = i;
        if (newest < 0) return;
        std::memcpy(h, g_leave_m[newest], sizeof(h));
    }
    const float dx = rc[12] - h[12], dy = rc[13] - h[13], dz = rc[14] - h[14], d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(d < 0.3f)) return;
    float ax = 0.0f;
    for (int k = 0; k < 3; ++k) ax = std::fmax(ax, std::fabs(rc[k] - h[k]));
    if (g_leave_probe_left.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    log::info("[holster] near the leaving item, not matched: a draw of %d bones, %.3f m from its placement (its x axis %.3f off), drawable %p",
              count, d, ax, rec[0x10]);
}

uint64_t hk_obj_set_matrix(uintptr_t obj, const float* m) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_gun_place_ret || !m || !g_held_at_hand_cfg.load(std::memory_order_relaxed) ||
        !g_held_fix_cfg.load(std::memory_order_relaxed))
        return o_obj_set_matrix(obj, m);
    const uintptr_t W = reinterpret_cast<uintptr_t>(m) - 0x80;
    leave_note(W, m);  // (round 77) the leaving item's placements, for its draws' hide
    int put_j = -1;  // [Hands] CarryByForegrip: the carried gun still in the game's hand (its put-away): placed at this John's hand
    {
        int sj = -1;
        uintptr_t sitem = 0;
        if (const uintptr_t sw = dual::secondary_W(&sj, &sitem); sw && sw == W) {
            // re-checked here (the weapon tick's view is the last update's): the slot item still has this W, holstered,
            // and is not the item in hand
            const uintptr_t wmgr = g_held_wmgr.load(std::memory_order_relaxed);
            uintptr_t iw = 0, hand = 0;
            int32_t state = 0;
            const bool st_ok = sitem && raw(sitem + 0xa0, &iw, sizeof(iw)) && iw == W && raw(sitem + 0x24, &state, sizeof(state));
            const bool hand_ok = wmgr && raw(wmgr + 0x80, &hand, sizeof(hand));  // the weapon manager's item in hand, read
            const bool held2 = st_ok && state == 2 && (!wmgr || (hand_ok && hand != sitem));
            alignas(16) float T[16];
            // [Hands] CarryByForegrip: at T on the front hand; ended (0) or T not read (3): left where the game puts it.
            // Until T is made (4): each holstered update places the gun where it was drawn, moved rigidly with John's
            // front hand's target since (carry_rigid: its move, the review, a walk would leave it behind; and its turn,
            // round 36, the gun kept the hand's place but not its turn, and T froze that), by this update's candidate T;
            // T is kept once the front bone has settled on the drawn hand, the candidate T itself (the gun in the bone's
            // frame: its translation within kCarrySettleTol and each axis within kCarrySettleTurn) over the last
            // kCarrySettleWin updates (cycle r151: two neighbouring updates within 1 cm let it settle at once while the
            // game's arm IK was still easing out: 5 cm left), or after kCarrySettleMax updates. In the bone's frame, not
            // the world: a walk moves both, a turn of the hand turns both
            static int s_settle = 0;
            static double s_settle_e[kCarrySettleWin][12] = {};
            static double s_settle_h[kCarrySettleWin][7] = {};  // (round 55) the carrying controller's place and turn at each
            static uint64_t s_serial = ~0ull;
            int mode = dual::place_mode(T);
            const uint64_t serial = dual::carry_serial();
            if (mode != 4 || serial != s_serial) {  // (a new carry: a new count, the review)
                s_settle = 0;
                s_serial = serial;
            }
            const bool old = g_carry_old.load(std::memory_order_relaxed);  // ("skel carry old": the move only, the bone from the hand in the world)
            if (mode == 4 && !held2 && !carry_puts(W, &put_j)) put_j = -1;
            // (r228: in the simulator the gun leaves the game's hand at once at the put-away and is holstered some 0.3 s
            // later, and in between it went unplaced: left where the game put it, drawn off the front hand. r231's count:
            // all 72 such updates read the item drawn (3) while the weapon manager's item in hand (+0x80) was no longer it,
            // so the in-hand placement below, which wants that item, refused it. Those are placed as the holstered gun
            // is, rigid at the target, T not yet settled on, the prop only: put_free. The item's states
            // (research/gc-weapons.md): 1 stowed, 2 holstered, 3 drawn; taken: 1; 2 while still the manager's item in
            // hand; 3 while not, within the carry's first second. Both reads made, never assumed; "skel carry" counts)
            const bool young = log::now_ms() - dual::state().carry_ms < 1000.0;
            const bool put_free = put_j >= 0 && st_ok &&
                                  (state == 1 || (state == 2 && hand_ok && hand == sitem) || (state == 3 && hand_ok && hand != sitem && young));
            if (put_j >= 0 && st_ok) {  // 0-2 by the state; 3 drawn and the manager's item, 4 drawn and not, 5 the manager not read
                const int si = state == 3 ? (!hand_ok ? 5 : hand == sitem ? 3 : 4) : state >= 0 && state < 3 ? state : 6;
                g_carry_put_states[si].fetch_add(1, std::memory_order_relaxed);
            }
            if (put_free) put_j = -1;  // (not as the item in hand, below)
            alignas(16) float pbuf[16];  // (put_free: the prop's matrix)
            FrontPair rig;  // the target a candidate T was made at (its draws moved by it: held_delta)
            if (mode == 4 && (held2 || put_free)) {
                alignas(16) float P[16], Tm[16];
                float h0[3], R0[9];
                double tb[3];
                BodyPoints bpn;
                mode = 3;
                if (sj >= 0 && sj <= 1 && dual::carry_start(P, h0, R0) && body_points(&bpn) && bpn.hand_ok[sj]) {
                    bool moved = true;
                    if (old)
                        for (int k = 0; k < 3; ++k) P[12 + k] += bpn.hand[sj][k] - h0[k];
                    else
                        moved = carry_rigid(P, h0, R0, bpn.hand[sj], bpn.target_rot[sj], P);
                    const bool cand = moved && carry_T_from(P, sj, Tm, tb);
                    if (cand && !held2) {  // (the put-away: placed, no settling yet)
                        std::memcpy(T, Tm, sizeof(Tm));
                        mode = 2;
                    } else if (cand) {
                        double e[12];  // T's axes and translation (the old test's: its axes, the bone from the hand in the world)
                        for (int i = 0; i < 3; ++i) {
                            for (int k = 0; k < 3; ++k) e[i * 3 + k] = Tm[i * 4 + k];
                            e[9 + i] = old ? tb[i] - bpn.hand[sj][i] : Tm[12 + i];
                        }
                        std::memcpy(s_settle_e[s_settle % kCarrySettleWin], e, sizeof(e));
                        double hv[7] = {};  // (round 55) the carrying controller (tracking space): its place, its turn
                        {
                            const int cc = dual::state().ctrl;
                            const hands::Hand hc = hands::get(cc >= 0 && cc <= 1 ? cc : 0);
                            for (int i = 0; i < 3; ++i) hv[i] = hc.pos[i];
                            for (int i = 0; i < 4; ++i) hv[3 + i] = hc.rot[i];
                        }
                        std::memcpy(s_settle_h[s_settle % kCarrySettleWin], hv, sizeof(hv));
                        bool settled = s_settle + 1 >= kCarrySettleWin;
                        for (int i = 0; settled && i < kCarrySettleWin; ++i) {
                            const double* q = s_settle_e[i];
                            const double dx = q[9] - e[9], dy = q[10] - e[10], dz = q[11] - e[11];
                            settled = dx * dx + dy * dy + dz * dz < kCarrySettleTol * kCarrySettleTol;
                            for (int ax = 0; settled && !old && ax < 3; ++ax) {
                                const double* u = q + ax * 3;
                                const double* v = e + ax * 3;
                                settled = (u[0] - v[0]) * (u[0] - v[0]) + (u[1] - v[1]) * (u[1] - v[1]) + (u[2] - v[2]) * (u[2] - v[2]) <
                                          kCarrySettleTurn * kCarrySettleTurn;
                            }
                        }
                        bool still = s_settle + 1 >= kCarrySettleWin;  // (round 55) the front target still over the window
                        for (int i = 0; still && i < kCarrySettleWin; ++i) {
                            const double* q = s_settle_h[i];
                            const double dx = q[0] - hv[0], dy = q[1] - hv[1], dz = q[2] - hv[2];
                            still = dx * dx + dy * dy + dz * dz < kCarrySettleStill * kCarrySettleStill;
                            // the turn between the two orientations: 2 acos |q1 . q2| (cos of half of it against)
                            const double dq = std::fabs(q[3] * hv[3] + q[4] * hv[4] + q[5] * hv[5] + q[6] * hv[6]);
                            still = still && dq >= std::cos(0.5 * kCarrySettleStillTurn);
                        }
                        if (settled && !still) g_carry_settle_moving.fetch_add(1, std::memory_order_relaxed);
                        if ((settled && still) || (still && s_settle + 1 >= kCarrySettleMax) || s_settle + 1 >= kCarrySettleMaxMoving) {
                            dual::carry_T_set(Tm);
                            g_carry_settle_n.store(s_settle + 1, std::memory_order_relaxed);
                            s_settle = 0;
                        } else {
                            ++s_settle;
                        }
                        std::memcpy(T, Tm, sizeof(Tm));
                        mode = 2;
                    }
                    if (mode == 2 && !old) {
                        rig.ok = true;
                        std::memcpy(rig.t, bpn.hand[sj], sizeof(rig.t));
                        std::memcpy(rig.r, bpn.target_rot[sj], sizeof(rig.r));
                    }
                }
            }
            const bool carry = mode >= 2;
            bool put_placed = false;
            if ((held2 || put_free) && (mode == 2 ? place_carry(W, const_cast<float*>(m), sj, T, rig.ok ? &rig : nullptr, put_free, put_free ? pbuf : nullptr)
                                                  : mode == 1 && held2 && place_secondary(W, const_cast<float*>(m), sj))) {
                dual::note_placed(W);  // m is W +0x80: written when placed (put_free: the prop only)
                if (put_free) {
                    g_carry_puts.fetch_add(1, std::memory_order_relaxed);
                    put_placed = true;
                }
            }
            if (put_placed) return o_obj_set_matrix(obj, pbuf);
            // (a carried gun still in hand, its put-away not done or taken back: placed as the item in hand, below)
            if (held2 || put_free || !carry) return o_obj_set_matrix(obj, m);
        }
    }
    // the player's item in hand in this very update (a draw or a put-away comes in its script phase, before the items'
    // update that places the props): the weapon manager's item (+0x80) in the hand (+0x24 == 3), whose W (+0xa0, what
    // FUN_1403b41e0 passes to FUN_1402fe6d0) is this one. SEH-guarded reads, no locks.
    const uintptr_t wmgr = g_held_wmgr.load(std::memory_order_relaxed);
    uintptr_t item = 0, iw = 0;
    int32_t state = 0;
    uint8_t lf = 0;
    if (!wmgr || !raw(wmgr + 0x80, &item, sizeof(item)) || !item || !raw(item + 0x24, &state, sizeof(state)) || state != 3 ||
        !raw(item + 0xa0, &iw, sizeof(iw)) || iw != W || !raw(item + 0x98, &lf, sizeof(lf)) || !pose::anchor_active())
        return o_obj_set_matrix(obj, m);
    {  // [Reload] LeverParts: the gun's own fire clip (its lever, slide and hammer, and the lever's sounds in it) stopped.
       // The shot links the clip to the gun controller's phase (W +0x330 = &G +0x24, FUN_140d16400); the next update
       // of the gun (FUN_1402fe6d0, before this placement) reads a phase outside 0..1 as the clip's end: it releases the
       // clip and clears the link itself. The link is pointed at such a phase (the game thread, SEH-guarded).
        uintptr_t link = 0;
        if ((worked_flags() & kStopFireClip) && raw(W + 0x330, &link, sizeof(link)) && link &&
            link != reinterpret_cast<uintptr_t>(&g_clip_done)) {
            g_clip_done = 2.0f;
            const uintptr_t done = reinterpret_cast<uintptr_t>(&g_clip_done);
            if (wraw(W + 0x330, &done, sizeof(done))) g_clip_stops.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (put_j >= 0) {
        // [Hands] CarryByForegrip: the carried gun still in the game's hand, its put-away under way (up to a second): its
        // prop drawn where the carry will hold it, at the front hand's target as the hand-over left it (carry_rigid), not
        // on the gun hand (round 36: it rode the empty hand away, then jumped); published with that hand's correction, so
        // its draws take that hand's move since (held_delta). W +0x80 stays the game's (its put-away). No two-handed
        // capture, no fixed grip: the gun is not in the gun hand. Not placed: as the item in hand, below
        alignas(16) float P[16], R0[9], src[16];
        float h0[3], A[9], a[3];
        double ad[3] = {};
        bool ik = false;
        BodyPoints bpn;
        if (dual::carry_start(P, h0, R0) && body_points(&bpn) && bpn.hand_ok[put_j] && hand_correction_d(put_j, A, a, &ik, ad) && ik &&
            raw(reinterpret_cast<uintptr_t>(m), src, sizeof(src)) && carry_rigid(P, h0, R0, bpn.hand[put_j], bpn.target_rot[put_j], g_place_buf)) {
            for (int r = 0; r < 4; ++r) g_place_buf[r * 4 + 3] = src[r * 4 + 3];  // (the game's fourth column, as below)
            FrontPair fp;  // the target it was placed at: its draws move by that target's move since (held_delta)
            fp.ok = true;
            std::memcpy(fp.t, bpn.hand[put_j], sizeof(fp.t));
            std::memcpy(fp.r, bpn.target_rot[put_j], sizeof(fp.r));
            g_placed_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
            g_placed.W = W;
            g_placed.build = g_vis_builds.load(std::memory_order_relaxed);
            std::memcpy(g_placed.m, g_place_buf, sizeof(g_placed.m));
            std::memcpy(g_placed.A, A, sizeof(A));
            std::memcpy(g_placed.a, a, sizeof(a));
            std::memcpy(g_placed.ad, ad, sizeof(ad));
            g_placed.ms = log::now_ms();
            g_placed.follow_j = put_j;
            put_pair(g_placed, fp);
            g_placed_seq.fetch_add(1, std::memory_order_release);
            g_placements.fetch_add(1, std::memory_order_relaxed);
            g_carry_puts.fetch_add(1, std::memory_order_relaxed);
            return o_obj_set_matrix(obj, g_place_buf);
        }
    }
    float A[9], a[3], src[16];
    bool ik = false;
    double ad[3] = {};
    if (!item_correction(lf ? 0 : 1, A, a, &ik, ad) || !ik || !raw(reinterpret_cast<uintptr_t>(m), src, sizeof(src)))
        return o_obj_set_matrix(obj, m);
    int16_t item_w = -1;  // the item's eWeapon (W +0x28 -> info +8, as holster.cpp's slot_gun reads it)
    {
        uintptr_t info = 0;
        if (raw(W + 0x28, &info, sizeof(info)) && info && !raw(info + 8, &item_w, sizeof(item_w))) item_w = -1;
    }
    // (2026-11-02, GOAL-offhand-changes C12c; round 66-68, the user: the lasso kills as soon as it catches, 18 -> 0 within
    // 270 ms) the lasso with its rope out (thrown, or on someone: state 3 or more) is left where the game puts it: moved to
    // the drawn hand each update, its rope's end jumped with it and the rope pulled the one caught (the rope is drawn at the
    // drawn hands by LassoAtHands all the same)
    // (2026-11-02, GOAL-older-items item 6 / GOAL-offhand-changes item 56; round 71, the user: with "the lasso where the game
    // holds it" it was the easiest to land) the lasso is never moved: the game's own item matrix (its throw from where the
    // game holds it); it is drawn in the drawn hand all the same, by the draws' hand correction (as an unplaced held item)
    if (item_w == 21) {
        g_lasso_left.fetch_add(1, std::memory_order_relaxed);
        return o_obj_set_matrix(obj, m);
    }
    fixed_grip(lf ? 0 : 1, src, item_w);  // run 7 item 1c: measured always; with [Hands] FixedGunGrip the long gun held by its aiming hold
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) g_place_buf[i * 4 + k] = A[k * 3] * src[i * 4] + A[k * 3 + 1] * src[i * 4 + 1] + A[k * 3 + 2] * src[i * 4 + 2];
        g_place_buf[i * 4 + 3] = src[i * 4 + 3];
    }
    for (int k = 0; k < 3; ++k)  // in double: the two large terms cancel to the hand's position, rounded once
        g_place_buf[12 + k] = static_cast<float>(static_cast<double>(A[k * 3]) * src[12] + static_cast<double>(A[k * 3 + 1]) * src[13] +
                                                 static_cast<double>(A[k * 3 + 2]) * src[14] + ad[k]);
    g_place_buf[15] = src[15];
    fixed_grip_placed(lf ? 0 : 1, A, ad, g_place_buf);
    {  // [Gestures] ThrowByGrip: the readied throwing knife by its tip, turned 180 deg about its local axis through the
       // pivot c: the other two axes negated, the origin moved by 2 c along them
        int ax = 0;
        float c[3];
        if (aim::knife_tip(&ax, c)) {
            for (int i = 0; i < 3; ++i) {
                if (i == ax) continue;
                for (int k = 0; k < 3; ++k) {
                    g_place_buf[12 + k] += 2.0f * c[i] * g_place_buf[i * 4 + k];
                    g_place_buf[i * 4 + k] = -g_place_buf[i * 4 + k];
                }
            }
        }
    }
    // [Hands] CarryByForegrip: a long gun held two-handed (fully), in the game's right hand: its place in the front hand's
    // corrected bone's frame, for a hand-over (2026-10-31, the off-hand build step 3: the Explosive Rifle and the Blunderbuss
    // too, r82 H2: "its two-handed place not known"). (2026-11-01, GOAL-offhand-changes C16) the front hand John's other
    // than his gun hand: his right for a gun drawn into his left (the transplant: this placement is the drawn one already,
    // item_correction's)
    if ((is_long_gun_w(item_w) || item_w == 31 || item_w == 34) && holster::two_hand_blend() >= 0.99f && dual::carry_enabled()) {
        // (the review: with John's left the gun hand, only when the transplant was made: else this placement is John's
        // right wrist's own, the gun at his right's grip)
        BodyPoints cbp;
        if (!lf && body_points(&cbp) && (cbp.gun == 1 || g_item_xfer))
            capture_two_hand(W, g_place_buf, cbp.gun == 0 ? 1 : 0);
        else
            g_two_T_left.fetch_add(1, std::memory_order_relaxed);  // ("skel carry": not captured, the item on the game's left hand)
    }
    const FrontPair fp = pair_front();  // ("skel carry trace")
    g_placed_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    g_placed.W = W;
    g_placed.build = g_vis_builds.load(std::memory_order_relaxed);
    std::memcpy(g_placed.m, g_place_buf, sizeof(g_placed.m));
    std::memcpy(g_placed.A, A, sizeof(A));
    std::memcpy(g_placed.a, a, sizeof(a));
    std::memcpy(g_placed.ad, ad, sizeof(ad));
    g_placed.ms = log::now_ms();
    g_placed.follow_j = -1;
    put_pair(g_placed, fp);
    g_placed_seq.fetch_add(1, std::memory_order_release);
    g_placements.fetch_add(1, std::memory_order_relaxed);
    if (dual::copy_W() == W) {  // [Hands] DualWieldCopy: in the hand that took it (round 13: after a left-hand draw the gun is
        // drawn in John's left by the transplant while the game's item stays in its right: the "other hand" was the left)
        const int cj = dual::copy_john();
        place_copy(W, g_place_buf, cj == 0 || cj == 1 ? cj : (lf ? 1 : 0));
    }
    return o_obj_set_matrix(obj, g_place_buf);
}

void shrink_axes(float* m) {
    for (int r = 0; r < 3; ++r) {
        float* v = m + r * 4;
        float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 0) {
            float k = 1e-4f / len;
            v[0] *= k;
            v[1] *= k;
            v[2] *= k;
        }
    }
}

// ---- the entity draw (DrawVisEntity, render thread): record [0x12] the grmMatrixSet, [0x13] an object whose +0x18
// is the skeleton (the player's set is found by it), +0x40 the record's world matrix.
using DrawVisEntity_t = void (*)(void* ctx, void** rec, uint64_t pass, uint64_t bucket, uint64_t a5, uint64_t a6, uint64_t a7);
DrawVisEntity_t o_DrawVisEntity = nullptr;

// Every change is made for the one draw and undone after it (round 5b: a prop's set is static, so a change written into
// it stayed: the hat stayed shrunk after first person ended, the rifle kept a wrong turn). The draw copies the
// matrices into its constants during the call (the shader globals' CPU shadow, 0x1490a0), so restoring after is safe.
struct PropDiag {
    int bone;
    uint8_t count;
    uint32_t flags;
    float set_t[3], rec_t[3], set_x[3], rec_x[3], rec_z[3];
    // 2026-10-12 (the user's "second model of a gun in the holster facing backwards"): the record's x and z axes after
    // the body's world correction for its bone (A x), and whether the still stance pins that bone (StillBelt's)
    float cor_x[3], cor_z[3];
    bool cor;
    bool pinned;
};
PropDiag g_prop_diag[8];
std::atomic<uint64_t> g_belt_twins{0};  // the game's holstered gun's draws hidden for a holster's shown gun (2026-10-12)
int g_nprop_diag = 0;


// [Hands] DualWieldCopy: the copy's second draw of the held item. A world map Tw on top of the held draw's correction:
// Tw = D_off o (C o P^-1) o D_gun^-1 (the copy's hand's motion since placement, the placement's map, the held draw's
// correction undone), and its twin Ts in the camera-relative set space (a_s = a_w - off + A off).
struct CopyJob {
    int kind = 0;  // 1 rigid (the record's two matrices), 2 a set under the record's matrix, 3 a skinned set
    float* sm = nullptr;
    size_t count = 0;
    float* rc = nullptr;
    float* rw = nullptr;
    uintptr_t mset = 0;
    Corr Tw, Ts;
    // [Hands] GunKick (2026-10-27): the gun in hand's kick its draw carries (the copy's draw is made from it): undone on the
    // copy, which takes the second spring's own (kick_draw's undo)
    float kl[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    float klp[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // and its previous half's (last frame's kick)
};
Corr corr_rows(const float* m) {  // a row-vector matrix as x' = A x + a
    Corr c;
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 3; ++i) c.A[k * 3 + i] = m[i * 4 + k];
        c.a[k] = m[12 + k];
    }
    return c;
}
Corr corr_mul(const Corr& x, const Corr& y) {  // x o y
    Corr o;
    mul3(x.A, y.A, o.A);
    for (int k = 0; k < 3; ++k) o.a[k] = x.A[k * 3] * y.a[0] + x.A[k * 3 + 1] * y.a[1] + x.A[k * 3 + 2] * y.a[2] + x.a[k];
    return o;
}
bool corr_inv(const Corr& x, Corr* o) {
    const float* m = x.A;
    const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!(std::fabs(det) > 1e-8f)) return false;
    const float k = 1.0f / det;
    const float r[9] = {(m[4] * m[8] - m[5] * m[7]) * k, (m[2] * m[7] - m[1] * m[8]) * k, (m[1] * m[5] - m[2] * m[4]) * k,
                        (m[5] * m[6] - m[3] * m[8]) * k, (m[0] * m[8] - m[2] * m[6]) * k, (m[2] * m[3] - m[0] * m[5]) * k,
                        (m[3] * m[7] - m[4] * m[6]) * k, (m[1] * m[6] - m[0] * m[7]) * k, (m[0] * m[4] - m[1] * m[3]) * k};
    std::memcpy(o->A, r, sizeof(r));
    for (int j = 0; j < 3; ++j) o->a[j] = -(r[j * 3] * x.a[0] + r[j * 3 + 1] * x.a[1] + r[j * 3 + 2] * x.a[2]);
    return true;
}
// the copy's maps for the held item's draw matched to sample hs with world correction c_gun; false: no copy this draw
bool copy_maps(const HeldSample& hs, const Corr& c_gun, Corr* Tw, Corr* Ts) {
    if (!dual::copy_W() || !hs.placed) return false;
    const HeldSample* cp = nullptr;
    for (const HeldSample& c : g_frame.copy)
        if (c.valid && c.build == hs.build && c.W == hs.W) cp = &c;
    if (!cp) {
        g_copy_nomatch.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Corr dw, ds, pinv, ginv;
    held_delta(*cp, -1, &dw, &ds);
    if (!corr_inv(corr_rows(hs.m), &pinv) || !corr_inv(c_gun, &ginv)) return false;
    *Tw = corr_mul(dw, corr_mul(corr_mul(corr_rows(cp->m), pinv), ginv));
    const float* off = g_player_offset;
    *Ts = *Tw;
    for (int k = 0; k < 3; ++k)
        Ts->a[k] = Tw->a[k] - off[k] + (Tw->A[k * 3] * off[0] + Tw->A[k * 3 + 1] * off[1] + Tw->A[k * 3 + 2] * off[2]);
    return true;
}
// [Hands] OwnModelFollow (run 6 item 1a, on): the other sidearm's own model (held_prop slot 1) drawn where the copy's
// hand is in this frame. held_prop holds the copy's C from the newest placement (the game thread, the update before),
// so the prop showed the hand of that update: it lagged behind and swam as John moved (round 12). Now its draw takes
// C from the frame's newest copy sample (the build being drawn) and the hand's motion since that placement (held_delta:
// the hand's correction now against the one C was placed with), exactly as the same-model copy's second draw does
// (copy_maps' D_off). The pose's layout: the axes X, Y, Z as world vectors, then the position. Under g_draw_mutex.
std::atomic<bool> g_own_follow{true};
std::atomic<uint64_t> g_own_follow_draws{0}, g_own_follow_nocp{0}, g_own_follow_newer{0};
int own_follow_pose(float* pose) {  // John's hand of the copy (0 left, 1 right), or -1: the pose left as it was
    g_other_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
    if (g_corr_frame != g_body_frame) g_other_stale_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
    const HeldSample* cp = nullptr;
    for (const HeldSample& c : g_frame.copy)
        if (c.valid && c.placed && (!cp || c.build > cp->build)) cp = &c;
    if (!cp) {
        g_own_follow_nocp.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    const float dx = cp->m[12] - pose[9], dy = cp->m[13] - pose[10], dz = cp->m[14] - pose[11];
    if (dx * dx + dy * dy + dz * dz > 1e-6f) g_own_follow_newer.fetch_add(1, std::memory_order_relaxed);  // held_prop's is newer
    if (!g_own_follow.load(std::memory_order_relaxed)) return cp->john;
    Corr dw, ds;
    held_delta(*cp, -1, &dw, &ds);
    for (int ax = 0; ax < 4; ++ax) {  // C's rows: the axes, then the position; the position also takes dw's shift
        const float* v = cp->m + ax * 4;
        for (int k = 0; k < 3; ++k) pose[ax * 3 + k] = dw.A[k * 3] * v[0] + dw.A[k * 3 + 1] * v[1] + dw.A[k * 3 + 2] * v[2] + (ax == 3 ? dw.a[k] : 0.0f);
    }
    g_own_follow_draws.fetch_add(1, std::memory_order_relaxed);
    return cp->john;
}
// Run 6 item 1, "skel follow arm|<report>": each drawn gun against its hand's drawn wrist, per frame: the gun in hand's
// drawn root, the other sidearm's own model's drawn pose and the same-model copy's drawn root, with both wrists as
// drawn (the player's set after the corrections), all in the world (3x4 rows: the 3x3, then the translation column),
// paired by the frame they were drawn in (the draw order does not matter: the first draw of each in a frame counts).
// The report: each one's pose in its wrist's frame against the recording's first frame. No system calls (frame numbers
// only); under g_draw_mutex.
// The draws of one DrawVisEntity pass are recorded (`skel follow arm [pass]`, 1 by default: each eye draws pass 1 once a
// frame, so the k-th draw of it in a frame is eye k's); each object's k-th draw of that pass is paired with the player
// set's k-th. A draw made before the frame's first player set (last frame's correction: g_corr_frame) is marked stale.
struct FollowObj {  // zero-initialised (the arrays stay in .bss)
    bool ok, stale;
    int side;  // John's hand: 0 left, 1 right
    int w;     // the weapon in hand then (the baseline restarts when it changes)
    float m[12];
    // 2026-10-14 (the user: the gun "shakes slightly when held still and more dramatically when moved", indoors): the
    // drawn object in the camera it was drawn with (the current viewport's camera matrix +0x140: its rows right, up,
    // back, then the position), metres along those rows
    bool cam_ok;
    float cam_p[3];
    float cam_r[9];  // its turn in that camera (the camera's rows times the object's 3x3: object to camera)
};
struct FollowSample {
    uint64_t frame;
    int nw, nobj[3];          // the pass's draws seen this frame: the player's sets, then the gun in hand, the own model, the copy
    bool w_ok[2][2];          // [eye][hand]
    float w[2][2][12];
    FollowObj obj[3][2];      // [object][eye]
    bool hv_ok[2];            // each controller in the head's frame (the latest views, the latched hands): metres
    float hv[2][3];
    float hq[2][4];           // and its turn there (x y z w)
};
// q* v for a unit quaternion q (x y z w)
void follow_qrot(const float* q, const float* v, float* o) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}
constexpr int kFollowMax = 900;
FollowSample g_follow[kFollowMax];
int g_follow_n = 0;
std::atomic<bool> g_follow_rec{false};
std::atomic<int> g_follow_pass{1};
std::atomic<bool> g_follow_auto{true};  // [Debug] FollowLog: re-armed every 10 s with an item in hand, the report logged
bool g_follow_manual = false;  // a test's recording ("skel follow arm") not yet read ("skel follow"): never re-armed over (under g_draw_mutex)
FollowSample* follow_at(uint64_t frame) {
    if (!g_follow_rec.load(std::memory_order_relaxed) || static_cast<int>(t_draw_pass & 7) != g_follow_pass.load(std::memory_order_relaxed))
        return nullptr;
    if (g_follow_n > 0 && g_follow[g_follow_n - 1].frame == frame) return &g_follow[g_follow_n - 1];
    if (g_follow_n >= kFollowMax) {
        g_follow_rec = false;
        return nullptr;
    }
    FollowSample& s = g_follow[g_follow_n++];
    s = FollowSample{};
    s.frame = frame;
    xr::EyeView v[2];
    if (xr::eye_views_peek(v)) {  // the controllers in the head's frame (the head: the views' mean position, eye 0's turn)
        const float hp[3] = {0.5f * (v[0].position[0] + v[1].position[0]), 0.5f * (v[0].position[1] + v[1].position[1]),
                             0.5f * (v[0].position[2] + v[1].position[2])};
        const float qc[4] = {-v[0].orientation[0], -v[0].orientation[1], -v[0].orientation[2], v[0].orientation[3]};
        for (int h = 0; h < 2; ++h) {
            const hands::Hand hd = hands::get_real(h);
            if (!hd.valid) continue;
            const float d[3] = {hd.pos[0] - hp[0], hd.pos[1] - hp[1], hd.pos[2] - hp[2]};
            follow_qrot(qc, d, s.hv[h]);
            // conj(head) * hand
            const float* b = hd.rot;
            const float* a = qc;
            s.hq[h][0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
            s.hq[h][1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
            s.hq[h][2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
            s.hq[h][3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
            s.hv_ok[h] = true;
        }
    }
    return &s;
}
void follow_obj(int which, int side, const float* m) {  // the object's next draw of the recorded pass (under g_draw_mutex)
    FollowSample* fs = follow_at(g_body_frame);
    if (!fs) return;
    const int k = fs->nobj[which]++;
    if (k > 1) return;
    FollowObj& o = fs->obj[which][k];
    std::memcpy(o.m, m, sizeof(o.m));
    o.side = side;
    o.w = g_frame.weapon;
    o.stale = g_corr_frame != g_body_frame;
    o.ok = true;
    o.cam_ok = false;
    uintptr_t vp = 0;
    struct M16 {
        float v[16];
    } cm{};
    const float* c = cm.v;
    if (rd(anchors::addr(anchors::Id::ViewportCurrent), &vp) && vp && rd(vp + 0x140, &cm)) {
        const float d[3] = {m[3] - c[12], m[7] - c[13], m[11] - c[14]};
        for (int r = 0; r < 3; ++r) o.cam_p[r] = d[0] * c[r * 4] + d[1] * c[r * 4 + 1] + d[2] * c[r * 4 + 2];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) o.cam_r[r * 3 + k] = c[r * 4] * m[0 * 4 + k] + c[r * 4 + 1] * m[1 * 4 + k] + c[r * 4 + 2] * m[2 * 4 + k];
        o.cam_ok = true;
    }
}
// each object in its wrist's frame (X = W^-1 O) against the first such frame of its gun: the move (mm) and turn (deg);
// the wrist's own speed (mm a frame), to tell a gun a frame late (its error grows with the speed) from an offset. Under
// g_draw_mutex; each line logged with `tag`
std::string follow_report(const char* tag) {
    std::string o;
    char b[900];
    for (int which = 0; which < 6; ++which) {
        const int obj = which / 2, e = which % 2;  // the object, the eye (its k-th draw of the pass)
        const char* name = obj == 0 ? "gun in hand" : obj == 1 ? "own model" : "copy";
        float X0[12];
        bool have0 = false;
        int n = 0, nfast = 0, nstale = 0, w0 = -2, wl = -1, s0 = -1;
        float worst_t = 0, worst_a = 0, sum2 = 0, fast_t = 0, slow_t = 0, wsp_max = 0;
        const float* wprev = nullptr;
        for (int i = 0; i < g_follow_n; ++i) {
            const FollowSample& s = g_follow[i];
            const FollowObj& fo = s.obj[obj][e];
            const int side = fo.side;
            const float* O = fo.m;
            if (!fo.ok || side < 0 || side > 1 || !s.w_ok[e][side]) {
                wprev = nullptr;
                continue;
            }
            if (fo.stale) ++nstale;
            float wi[12], X[12];
            if (!inv34(s.w[e][side], wi)) continue;
            mul34(wi, O, X);
            float wsp = -1;
            if (wprev && side == s0) {  // the same hand's wrist, frame to frame
                const float dx = s.w[e][side][3] - wprev[3], dy = s.w[e][side][7] - wprev[7], dz = s.w[e][side][11] - wprev[11];
                wsp = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f;
                wsp_max = std::fmax(wsp_max, wsp);
            }
            wprev = s.w[e][side];
            if (!have0 || fo.w != w0 || side != s0) {  // a new gun or another hand: its own baseline (and no speed across)
                std::memcpy(X0, X, sizeof(X0));
                have0 = true;
                w0 = fo.w;
                s0 = side;
            }
            wl = fo.w;
            float xi[12], D[12];
            if (!inv34(X0, xi)) continue;
            mul34(xi, X, D);
            const float tl = std::sqrt(D[3] * D[3] + D[7] * D[7] + D[11] * D[11]) * 1000.0f;
            const float tr = D[0] + D[5] + D[10];
            const float an = std::acos(std::fmax(-1.0f, std::fmin(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f;
            worst_t = std::fmax(worst_t, tl);
            worst_a = std::fmax(worst_a, an);
            sum2 += tl * tl;
            ++n;
            if (wsp > 20.0f) {  // the hand moving over 20 mm a frame (1.8 m/s at 90 Hz, 0.9 m/s at 45 Hz)
                fast_t = std::fmax(fast_t, tl);
                ++nfast;
            } else if (wsp >= 0.0f && wsp < 5.0f) {
                slow_t = std::fmax(slow_t, tl);
            }
        }
        if (!n) continue;
        // the shake as seen (2026-10-14): frame to frame, the object's move in the camera it was drawn with against the
        // controller's in the head's frame (lengths: their frames' axes are not matched); with the controller still
        // (under 1 mm a frame) the object's whole move is shake
        int nstill = 0, nmov = 0;
        float still_max = 0, still2 = 0, mov_max = 0, mov2 = 0;
        float rs_max = 0, rs2 = 0, rm_max = 0, rm2 = 0;  // the same for the turn (deg a frame)
        for (int i = 1; i < g_follow_n; ++i) {
            const FollowSample &a = g_follow[i - 1], &s = g_follow[i];
            const FollowObj &fa = a.obj[obj][e], &fo = s.obj[obj][e];
            if (s.frame != a.frame + 1 || !fa.ok || !fo.ok || !fa.cam_ok || !fo.cam_ok || fa.side != fo.side || fa.w != fo.w) continue;
            const int side = fo.side;
            if (side < 0 || side > 1 || !a.hv_ok[side] || !s.hv_ok[side]) continue;
            const float dc = std::sqrt((fo.cam_p[0] - fa.cam_p[0]) * (fo.cam_p[0] - fa.cam_p[0]) + (fo.cam_p[1] - fa.cam_p[1]) * (fo.cam_p[1] - fa.cam_p[1]) +
                                       (fo.cam_p[2] - fa.cam_p[2]) * (fo.cam_p[2] - fa.cam_p[2])) * 1000.0f;
            const float dh = std::sqrt((s.hv[side][0] - a.hv[side][0]) * (s.hv[side][0] - a.hv[side][0]) + (s.hv[side][1] - a.hv[side][1]) * (s.hv[side][1] - a.hv[side][1]) +
                                       (s.hv[side][2] - a.hv[side][2]) * (s.hv[side][2] - a.hv[side][2])) * 1000.0f;
            // the object's turn between the frames, as seen (A0^T A1), and the controller's in the head's frame
            float tr = 0.0f;
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) tr += fa.cam_r[r * 3 + k] * fo.cam_r[r * 3 + k];
            const float ac = std::acos(std::fmax(-1.0f, std::fmin(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f;
            const float* q0 = a.hq[side];
            const float* q1 = s.hq[side];
            const float dq = std::fabs(q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3]);
            const float ah = 2.0f * std::acos(std::fmin(1.0f, dq)) * 57.2958f;
            if (dh < 1.0f && ah < 0.2f) {
                ++nstill;
                still_max = std::fmax(still_max, dc);
                still2 += dc * dc;
                rs_max = std::fmax(rs_max, ac);
                rs2 += ac * ac;
            } else {
                const float x = std::fabs(dc - dh), y = std::fabs(ac - ah);
                ++nmov;
                mov_max = std::fmax(mov_max, x);
                mov2 += x * x;
                rm_max = std::fmax(rm_max, y);
                rm2 += y * y;
            }
        }
        std::snprintf(b, sizeof(b), "\n  %s (weapon %d), eye %d: %d frames (stale %d), worst %.1f mm %.2f deg, rms %.1f mm | the wrist up to %.0f mm a frame; worst at over 20 mm a frame (%d frames) %.1f mm, under 5 mm a frame %.1f mm"
                      " | seen from the eye: the controller still %d frames, the object moving rms %.1f max %.1f mm a frame; moving %d frames, off the controller's move rms %.1f max %.1f mm"
                      " | its turn: still rms %.2f max %.2f deg a frame, moving off the controller's rms %.2f max %.2f deg",
                      name, wl, e, n, nstale, worst_t, worst_a, std::sqrt(sum2 / n), wsp_max, nfast, fast_t, slow_t, nstill,
                      nstill ? std::sqrt(still2 / nstill) : 0.0f, still_max, nmov, nmov ? std::sqrt(mov2 / nmov) : 0.0f, mov_max,
                      nstill ? std::sqrt(rs2 / nstill) : 0.0f, rs_max, nmov ? std::sqrt(rm2 / nmov) : 0.0f, rm_max);
        o += b;
        log::info("[follow]%s %s", tag, b + 3);  // the reply may be cut: each line in the log too
    }
    return o;
}
void pose_to34(const float* pose, float* m) {  // the axes X, Y, Z (world vectors), the position -> 3x4 rows
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = pose[j * 3 + i];
        m[i * 4 + 3] = pose[9 + i];
    }
}
// run 7 item 1: the last copy draw's kind and its root before and after apply_copy (world), under g_draw_mutex
struct CopyWhere {
    int kind = 0;
    uint64_t pass = 0;
    float pre[3] = {}, post[3] = {};
    uint64_t n = 0;
};
CopyWhere g_copy_where;
void copy_root(const CopyJob& cj, float* out) {
    if (cj.kind == 3 && cj.sm && cj.mset) {
        const float* po = reinterpret_cast<const float*>(cj.mset + 0x10);
        for (int k = 0; k < 3; ++k) out[k] = cj.sm[k * 4 + 3] + po[k];
    } else if (cj.rc) {
        for (int k = 0; k < 3; ++k) out[k] = cj.rc[12 + k];
    }
}
void apply_copy(const CopyJob& cj) {
    const Corr& c = cj.Tw;
    if (cj.kind == 1) {  // the record's matrices: x' = A x + a on the axis rows and the translation
        for (int half = 0; half < 2; ++half) {
            float* rm = (half ? cj.rw : cj.rc);
            for (int ax = 0; ax < 3; ++ax) {
                float v[3] = {rm[ax * 4], rm[ax * 4 + 1], rm[ax * 4 + 2]};
                for (int k = 0; k < 3; ++k) rm[ax * 4 + k] = c.A[k * 3] * v[0] + c.A[k * 3 + 1] * v[1] + c.A[k * 3 + 2] * v[2];
            }
            float t[3] = {rm[12], rm[13], rm[14]};
            for (int k = 0; k < 3; ++k) rm[12 + k] = c.A[k * 3] * t[0] + c.A[k * 3 + 1] * t[1] + c.A[k * 3 + 2] * t[2] + c.a[k];
        }
    } else if (cj.kind == 2) {  // the set under the record's matrix R: M' = R^T A R M, as the held correction goes in
        for (int half = 0; half < 2; ++half) {
            const float* rm = half ? cj.rw : cj.rc;
            float R[9], RT[9], AR[9], B[9];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    R[i * 3 + j] = rm[j * 4 + i];
                    RT[j * 3 + i] = rm[j * 4 + i];
                }
            mul3(c.A, R, AR);
            mul3(RT, AR, B);
            const float tr_[3] = {rm[12], rm[13], rm[14]};
            for (size_t j = cj.count * half; j < cj.count * (half + 1); ++j) {
                float* m = cj.sm + j * 12;
                const float mb[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                float nm[9], t[3] = {m[3], m[7], m[11]}, wt[3], v[3];
                mul3(B, mb, nm);
                for (int k = 0; k < 3; ++k) wt[k] = R[k * 3] * t[0] + R[k * 3 + 1] * t[1] + R[k * 3 + 2] * t[2] + tr_[k];
                for (int k = 0; k < 3; ++k) v[k] = c.A[k * 3] * wt[0] + c.A[k * 3 + 1] * wt[1] + c.A[k * 3 + 2] * wt[2] + c.a[k] - tr_[k];
                for (int k = 0; k < 3; ++k) {
                    m[k * 4 + 0] = nm[k * 3 + 0];
                    m[k * 4 + 1] = nm[k * 3 + 1];
                    m[k * 4 + 2] = nm[k * 3 + 2];
                    m[k * 4 + 3] = RT[k * 3] * v[0] + RT[k * 3 + 1] * v[1] + RT[k * 3 + 2] * v[2];
                }
            }
        }
    } else if (cj.kind == 3) {  // a skinned set in the camera-relative space: t' = A (t + d) + a - d
        const Corr& cs = cj.Ts;
        const float* po = reinterpret_cast<const float*>(cj.mset + 0x10);
        const float d[3] = {po[0] - g_player_offset[0], po[1] - g_player_offset[1], po[2] - g_player_offset[2]};
        for (size_t j = 0; j < cj.count * 2; ++j) {
            float* m = cj.sm + j * 12;
            const float mb[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
            float nm[9], t[3] = {m[3] + d[0], m[7] + d[1], m[11] + d[2]};
            mul3(cs.A, mb, nm);
            for (int k = 0; k < 3; ++k) {
                m[k * 4 + 0] = nm[k * 3 + 0];
                m[k * 4 + 1] = nm[k * 3 + 1];
                m[k * 4 + 2] = nm[k * 3 + 2];
                m[k * 4 + 3] = cs.A[k * 3] * t[0] + cs.A[k * 3 + 1] * t[1] + cs.A[k * 3 + 2] * t[2] + cs.a[k] - d[k];
            }
        }
    }
}

// (GOAL-single-pass item 8e, test only; off: one relaxed load) "skel passcmp on|off" and "skel passcmp": the player's sets in pass 2
// of a double frame against the same sets in its pass 1, as the game left them (raw: the set's current matrices and its +0x10 offset,
// before this hook's edits) and as drawn (after them): the frames equal, differing, and the largest difference of any element. Tells
// a per-pass change of the game's own data (another thread's write between the passes) from this hook's per-pass edit
constexpr int kPcSets = 16, kPcFloats = 2400;  // (200 bones)
struct PcSet {  // (keyed by the drawable and the entity call's own pass and bucket: a shadow call never meets a main one)
    uintptr_t drawable = 0;
    uint64_t seq = 0, dvp = 0, bk = 0;
    int n[2] = {};
    float m[2][kPcFloats + 3];  // [raw, drawn]: the matrices, then (raw only) the offset
};
std::atomic<bool> g_pc_on{false};
PcSet g_pc[kPcSets];  // (the render thread) pass 1's of the current double frame
std::atomic<uint64_t> g_pc_eq[2], g_pc_ne[2], g_pc_unpaired;
std::atomic<float> g_pc_max[2];
void pc_note(uintptr_t dr, uint64_t dvp, uint64_t bk, const float* m, int n, const float* off, int kind) {  // kind 0 raw, 1 drawn
    const int ps = dual_pass::pass();
    if (ps != 1 && ps != 2) return;
    const uint64_t seq = dual_pass::double_seq();
    if (n > kPcFloats) n = kPcFloats;
    if (ps == 1) {
        PcSet* s = nullptr;
        for (PcSet& c : g_pc)
            if (c.drawable == dr && c.seq == seq && c.dvp == dvp && c.bk == bk) s = &c;
        for (PcSet& c : g_pc)
            if (!s && c.seq != seq) s = &c;
        if (!s) return;
        if (s->seq != seq || s->drawable != dr || s->dvp != dvp || s->bk != bk) {
            s->drawable = dr;
            s->seq = seq;
            s->dvp = dvp;
            s->bk = bk;
            s->n[0] = s->n[1] = 0;
        }
        std::memcpy(s->m[kind], m, static_cast<size_t>(n) * sizeof(float));
        if (off) std::memcpy(s->m[kind] + n, off, 3 * sizeof(float));
        s->n[kind] = n + (off ? 3 : 0);
        return;
    }
    for (PcSet& c : g_pc) {
        if (c.drawable != dr || c.seq != seq || c.dvp != dvp || c.bk != bk || c.n[kind] != n + (off ? 3 : 0)) continue;
        float mx = 0.0f;
        for (int i = 0; i < n; ++i) mx = std::fmax(mx, std::fabs(c.m[kind][i] - m[i]));
        if (off)
            for (int i = 0; i < 3; ++i) mx = std::fmax(mx, std::fabs(c.m[kind][n + i] - off[i]));
        const bool same = !std::memcmp(c.m[kind], m, static_cast<size_t>(n) * sizeof(float)) &&
                          (!off || !std::memcmp(c.m[kind] + n, off, 3 * sizeof(float)));  // (bitwise: a NaN differs)
        (same ? g_pc_eq[kind] : g_pc_ne[kind]).fetch_add(1, std::memory_order_relaxed);
        if (mx > g_pc_max[kind].load(std::memory_order_relaxed)) g_pc_max[kind].store(mx, std::memory_order_relaxed);
        return;
    }
    g_pc_unpaired.fetch_add(1, std::memory_order_relaxed);
}

void hk_DrawVisEntity(void* ctx, void** rec, uint64_t pass, uint64_t bucket, uint64_t a5, uint64_t a6, uint64_t a7) {
    // ([Stereo] RouteFCensus, GOAL-single-pass item 8h' step 1, test only; off: two relaxed loads) points E0/E1, before this
    // hook's own work and outside 8c's entity timer: the state DrawVisList left for the bucket's first entity.
    // (item 8h' step 2, test only, the simulator only) route F's probe: true = pass 2's e1 entity of bucket 0 or 1 is not drawn
    // ("prepass fcensus probe skip|skipwb"), the return before every piece of this hook's own work. The audit: the player's
    // drawable list, g_last_player_set, g_player_offset, the wrists (g_steady), "skel lag" (once a frame) and held_prop's
    // learning are the frame's, already written by pass 1's draw (and the shadow pass's); the set and record edits
    // (transform_set, the props' corrections) are restored after the draw, so a skipped call has nothing to restore; the
    // motion-vector caches (gun_prev_fix, set_prev_fix) and the stale fix read the frame's first draw (g_body_frame), never pass
    // 2's; the wind hold is set and released around the draw only; the copy draw (DualWieldCopy) is part of the draw skipped
    // (eye 2 knowingly empty in e1); the per-pass counters and "skel follow" (a test record, its pass-2 draw then absent) are
    // diagnostics; 8c's entity timer counts the drawn calls only; t_cull_show stays 0. t_draw_pass is set to the value the
    // call would have left, for the draw-geometry hooks of a later callback draw
    if (fcensus::on() && fcensus::entity(rec, bucket, a7, reinterpret_cast<uintptr_t>(_ReturnAddress()))) {
        t_draw_pass = pass;
        return;
    }
    thread_local int dve_depth = 0;  // (the outermost entity call only: a nested one is inside its time)
    struct DveTimed {  // ([Stereo] PrepassGpuStages' CPU census, GOAL-single-pass item 8c, test only: off, one relaxed load)
        int& depth;
        int64_t t0 = 0;
        bool counted = false;  // (the depth counted only while the census is on: off, one relaxed load)
        explicit DveTimed(int& d) : depth(d) {
            if (!gpu_stages::cpu_on()) return;
            counted = true;
            if (depth++ == 0 && gpu_stages::cpu_in_e1()) {
                LARGE_INTEGER t;
                QueryPerformanceCounter(&t);
                t0 = t.QuadPart;
            }
        }
        ~DveTimed() {
            if (counted) --depth;
            if (!t0) return;
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            gpu_stages::cpu_dve(t.QuadPart - t0);
        }
    } dve_timed(dve_depth);
    struct DveLive {  // (t_dve_live: "skel pieces"'s untransformed draws, inside an entity call or not)
        DveLive() {
            ++t_dve_live;
            t_dve_frame = g_body_frame;
        }
        ~DveLive() { --t_dve_live; }
    } dve_live;
    t_draw_pass = pass;
    const float* const pc_s_in = t_pc_s;  // (the rigid copies' set as this call found it: put back after its restore)
    const uint64_t pc_frame_in = t_pc_frame;
    CopyJob cj;  // [Hands] DualWieldCopy: the held item drawn again at the free hand after its own draw
    thread_local std::vector<float> save;
    float wind_y = -1.0f;  // >= 0: this draw's cloth flutter wind (gWindParams.y), set around it
    float* set_at = nullptr;
    size_t set_n = 0;
    float* rec_at = nullptr;
    float rec_save[32];  // the record's two matrices (+0x00 drawn, +0x40 last frame's), restored after the draw
    float* leave_at = nullptr;  // (round 78) a leaving item's record drawn at its holstered place: the game's two matrices put back after
    float leave_orig[32];
    int cull_show = 0;   // the player's set in a show mode: its all-hidden geometries are skipped (HiddenGeometry)
    // ([Holsters] InteractionHighlights=parts, run 29 item 2) the gun this call draws (0 in hand, 1 the second gun or the copy's
    // prop), its eWeapon and where its frame is read from after the corrections (drawn_gun_stage's m, rec, po or rows)
    int gl_role = -1, gl_weapon = -1;
    const float *gl_m = nullptr, *gl_rec = nullptr, *gl_po = nullptr, *gl_rows = nullptr;
    if (g_frame.valid && rec && rec[0x12]) {
        uintptr_t mset = reinterpret_cast<uintptr_t>(rec[0x12]);
        uint8_t count = 0;
        if (raw(mset, &count, 1)) {
            float* sm = reinterpret_cast<float*>(mset + 0xd4);
            const size_t n = static_cast<size_t>(count) * 24;  // the current and the previous set, 12 floats a matrix
            if (count > 8 && g_leave_probe_left.load(std::memory_order_relaxed) > 0) leave_probe(rec, count);  // (round 78: the lasso's)
            if (g_near_on.load(std::memory_order_relaxed) && g_frame.valid && g_frame.held_any && g_frame.held_new >= 0) {
                const float* hm = g_frame.held[g_frame.held_new].m;
                const float* rr = reinterpret_cast<const float*>(rec);
                {  // "skel near": the set's record and its first matrix
                    note_near(reinterpret_cast<uintptr_t>(rec[0x10]), 1, count, rr + 12, hm + 12);
                    if (count >= 1) {
                        const float p0[3] = {sm[3], sm[7], sm[11]};
                        note_near(reinterpret_cast<uintptr_t>(rec[0x10]) ^ 1, 1, count, p0, hm + 12);  // (^1: by its first bone)
                    }
                }
            }
            uintptr_t owner = 0;
            bool leave_by_model = false;  // (the review) hidden by leave_model: never moved by leave_subst, not the leaving instance
            if (count > g_rig.max_index && count <= g_rig.count && rec[0x13] &&
                raw(reinterpret_cast<uintptr_t>(rec[0x13]) + 0x18, &owner, 8) && owner == g_frame.skel) {
                g_player_drawable.store(reinterpret_cast<uintptr_t>(rec[0x10]), std::memory_order_relaxed);
                g_last_player_set.store(mset, std::memory_order_relaxed);  // ("skel setdump")
                g_last_player_set_frame.store(g_body_frame, std::memory_order_relaxed);
                g_last_player_set_count.store(count, std::memory_order_relaxed);
                {  // each distinct one, up to eight (lock-free: the first empty slot taken)
                    const uintptr_t dr = reinterpret_cast<uintptr_t>(rec[0x10]);
                    bool known = false;
                    for (int si = 0; si < 8 && !known; ++si) {
                        auto& slot_d = g_player_drawables[si];
                        uintptr_t cur_d = slot_d.load(std::memory_order_relaxed);
                        if (cur_d == dr) {
                            g_player_drawn_at[si].store(g_body_frame, std::memory_order_relaxed);
                            known = true;
                        } else if (!cur_d && slot_d.compare_exchange_strong(cur_d, dr)) {
                            g_player_drawn_at[si].store(g_body_frame, std::memory_order_relaxed);
                            known = true;
                        }
                    }
                    for (int si = 0; si < 8 && !known; ++si) {  // full: an entry not drawn for 600 frames (an old outfit's) replaced
                        uintptr_t cur_d = g_player_drawables[si].load(std::memory_order_relaxed);
                        if (g_body_frame - g_player_drawn_at[si].load(std::memory_order_relaxed) > 600 &&
                            g_player_drawables[si].compare_exchange_strong(cur_d, dr)) {
                            g_player_drawn_at[si].store(g_body_frame, std::memory_order_relaxed);
                            known = true;
                        }
                    }
                }
                if (g_frame.show && o_draw_geom && g_geom_mode.load(std::memory_order_relaxed) != 0) cull_show = g_frame.show;
                std::lock_guard lock(g_draw_mutex);
                if (g_pc_on.load(std::memory_order_relaxed))  // ("skel passcmp", item 8e: the set as the game left it)
                    pc_note(reinterpret_cast<uintptr_t>(rec[0x10]), pass, bucket, sm, static_cast<int>(count) * 12, reinterpret_cast<const float*>(mset + 0x10), 0);
                save.assign(sm, sm + n);
                set_at = sm;
                set_n = n;
                g_pset_edit.store(sm, std::memory_order_release);  // (the player's set edited from here to its restore)
                // the sets' header offsets: +0x10 the current, +0x20 the previous (the matrices are the world less it)
                const float* off_cur = reinterpret_cast<const float*>(mset + 0x10);
                const float* off_prev = reinterpret_cast<const float*>(mset + 0x20);
                if (const int pv = g_rig.pelvis; pv > 0 && pv < count && static_cast<size_t>(pv) * 3 + 2 < g_rig.jbind.size()) {
                    const float* m = sm + pv * 12;  // (the shadows round: the eyes' pelvis as the game gave it)
                    const float* j = &g_rig.jbind[static_cast<size_t>(pv) * 3];
                    for (int r = 0; r < 3; ++r) g_eye_pelvis.g[r] = m[r * 4] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3] + off_cur[r];
                }
                transform_set(sm, count, 0, g_frame, off_cur);
                if (const int pv = g_rig.pelvis; pv > 0 && pv < count && static_cast<size_t>(pv) * 3 + 2 < g_rig.jbind.size()) {
                    const float* m = sm + pv * 12;  // (the shadows round: the eyes' pelvis, for the outside draw's against it)
                    const float* j = &g_rig.jbind[static_cast<size_t>(pv) * 3];
                    for (int r = 0; r < 3; ++r) {
                        g_eye_pelvis.p[r] = m[r * 4] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3] + off_cur[r];
                        g_eye_pelvis.ax[r] = m[r * 4];
                    }
                    g_eye_pelvis.frame = g_body_frame;
                }
                if (g_own_pieces.load(std::memory_order_relaxed))  // run 12 item 2: each moved piece's joint as drawn (world)
                    for (int i = 0; i < body::kPieces; ++i) {
                        const int b = g_rig.piece[i];
                        if (!g_piece_diag[i].ok || b <= 0 || b >= count || static_cast<size_t>(b) * 3 + 2 >= g_rig.jbind.size()) continue;
                        const float* m = sm + b * 12;
                        const float* j = &g_rig.jbind[static_cast<size_t>(b) * 3];
                        for (int r = 0; r < 3; ++r) g_piece_diag[i].got[r] = m[r * 4] * j[0] + m[r * 4 + 1] * j[1] + m[r * 4 + 2] * j[2] + m[r * 4 + 3] + off_cur[r];
                        g_piece_diag[i].frame = g_body_frame;
                        g_piece_diag[i].pose_ok = piece_pose(sm, count, i, g_piece_diag[i].loc, g_piece_diag[i].rel);
                    }
                transform_set(sm + count * 12, count, 1, g_frame, off_prev);  // the previous frame's (motion vectors)
                for (int hf = 0; hf < 2; ++hf) {  // the hands as drawn, for the sun's shadow (piece_outside_draw)
                    const float* src = sm + static_cast<size_t>(hf) * count * 12;
                    const float* o = hf ? off_prev : off_cur;
                    for (int side = 0; side < 2; ++side)
                        for (int b : g_rig.wrist_set[side]) {
                            if (b <= 0 || b >= count || b >= 256) continue;
                            float* d = &g_eye_hands.m[hf][static_cast<size_t>(b) * 12];
                            std::memcpy(d, src + static_cast<size_t>(b) * 12, 12 * sizeof(float));
                            for (int r = 0; r < 3; ++r) d[r * 4 + 3] += o[r];
                        }
                }
                g_eye_hands.frame = g_body_frame;
                g_eye_hands.count = count;
                if (const int nbs = g_bshift_n.load(std::memory_order_acquire)) {  // run 12 item 1, "skel boneshift" (test)
                    for (int i = 0; i < nbs; ++i) {
                        const int b = g_bshift_bone[i];
                        if (b < 0 || b >= count) continue;
                        for (int h = 0; h < 2; ++h)
                            for (int k = 0; k < 3; ++k) sm[(static_cast<size_t>(h) * count + b) * 12 + k * 4 + 3] += g_bshift_d[i][k];
                    }
                    g_bshift_draws.fetch_add(1, std::memory_order_relaxed);
                }
                set_prev_fix(reinterpret_cast<uintptr_t>(rec[0x10]), sm, count, off_cur, off_prev);  // run 12 item 9, [Body] ArmMotionVectors (by drawable: the set is made anew each frame)
                for (int h = 0; h < 2; ++h) {  // [Reload] LeverParts: each wrist as drawn (the held gun kept in it)
                    Steady& sd = g_steady[h];
                    const int wb = g_rig.wrist[h];
                    sd.wrist_ok = wb >= 0 && wb < count;
                    if (sd.wrist_ok) std::memcpy(sd.Sw, sm + wb * 12, sizeof(sd.Sw));
                }
                if (g_shake_on.load(std::memory_order_relaxed) && g_steady[1].wrist_ok && g_shake_wr_frame != g_body_frame) {  // ("skel shake")
                    g_shake_wr_frame = g_body_frame;
                    for (int k = 0; k < 3; ++k) g_shake_wr[k] = static_cast<double>(g_steady[1].Sw[k * 4 + 3]) + off_cur[k];
                }
                if (FollowSample* fs = follow_at(g_body_frame)) {  // run 6 item 1, "skel follow": the wrists as drawn (world)
                    const int e = fs->nw++;
                    for (int h = 0; e < 2 && h < 2; ++h)
                        if (g_steady[h].wrist_ok) {
                            std::memcpy(fs->w[e][h], g_steady[h].Sw, sizeof(fs->w[e][h]));
                            for (int k = 0; k < 3; ++k) fs->w[e][h][k * 4 + 3] += off_cur[k];
                            fs->w_ok[e][h] = true;
                        }
                }
                {  // "skel lag": the game's gun in its attachment bone's frame, this set's and the previous one's (animated)
                    const HeldSample* hs = newest_held(g_frame.held);
                    const int side = hs ? (hs->left ? 0 : 1) : -1;
                    const int ab = side < 0 ? -1 : g_rig.att_wrist[side] >= 0 ? g_rig.att_wrist[side] : g_rig.wrist[side];
                    if (ab >= 0 && ab < count && g_body_frame != g_lag_frame) {
                        g_lag_frame = g_body_frame;
                        for (int half = 0; half < 2; ++half) {
                            const float* m = save.data() + static_cast<size_t>(half) * count * 12 + static_cast<size_t>(ab) * 12;
                            const float* o = half ? off_prev : off_cur;
                            const float* j = &g_rig.jbind[static_cast<size_t>(ab) * 3];
                            float M[9], jw[3], G[9], go[3], p[3], R[9];
                            ortho_cols(m, M);
                            for (int k = 0; k < 3; ++k) jw[k] = m[k * 4] * j[0] + m[k * 4 + 1] * j[1] + m[k * 4 + 2] * j[2] + m[k * 4 + 3] + o[k];
                            for (int c = 0; c < 3; ++c) {  // the gun's axes as columns, normalised
                                const float* row = hs->game + c * 4;
                                float l = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                                l = l > 1e-6f ? l : 1.0f;
                                for (int k = 0; k < 3; ++k) G[k * 3 + c] = row[k] / l;
                            }
                            for (int k = 0; k < 3; ++k) go[k] = hs->game[12 + k] - jw[k];
                            for (int i = 0; i < 3; ++i) p[i] = M[0 * 3 + i] * go[0] + M[1 * 3 + i] * go[1] + M[2 * 3 + i] * go[2];
                            for (int i = 0; i < 3; ++i)
                                for (int c = 0; c < 3; ++c) R[i * 3 + c] = M[0 * 3 + i] * G[0 * 3 + c] + M[1 * 3 + i] * G[1 * 3 + c] + M[2 * 3 + i] * G[2 * 3 + c];
                            LagRange& L = half ? g_lag_prev : g_lag_now;
                            if (!L.have) {
                                std::memcpy(L.p0, p, sizeof(p));
                                std::memcpy(L.R0, R, sizeof(R));
                                L.have = true;
                            } else {
                                const float dmm = 1000.0f * std::sqrt((p[0] - L.p0[0]) * (p[0] - L.p0[0]) + (p[1] - L.p0[1]) * (p[1] - L.p0[1]) +
                                                                      (p[2] - L.p0[2]) * (p[2] - L.p0[2]));
                                float tr = 0;  // the angle between R and R0: acos((tr(R0^T R) - 1) / 2)
                                for (int i = 0; i < 3; ++i)
                                    for (int k = 0; k < 3; ++k) tr += L.R0[k * 3 + i] * R[k * 3 + i];
                                const float c = std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f);
                                const float ddeg = std::acos(c) * 57.29578f;
                                L.max_mm = std::fmax(L.max_mm, dmm);
                                L.max_deg = std::fmax(L.max_deg, ddeg);
                            }
                            ++L.n;
                        }
                    }
                }
                if ((g_frame.show && g_frame.still_cloth) || g_frame.no_flutter)
                    wind_y = 0.0f;
                else if (const float wt = g_wind_test.load(std::memory_order_relaxed); wt >= 0.0f)
                    wind_y = wt;
                std::memcpy(g_player_offset, reinterpret_cast<const void*>(mset + 0x10), sizeof(g_player_offset));
                g_sets.fetch_add(1, std::memory_order_relaxed);
            } else if (count >= 1 && count <= 8 && (leave_draw(reinterpret_cast<const float*>(rec)) || (leave_by_model = leave_model(rec))) &&
                       !(g_leave_subst_on.load(std::memory_order_relaxed) && !leave_by_model && leave_subst(rec, &leave_at, leave_orig))) {
                // (round 77) the item a mod put-away let go, drawn on its way to the holster: hidden (restored after the draw);
                // (round 78) drawn at its holstered place instead where that is known (leave_subst: then the prop path below)
                float* rc = reinterpret_cast<float*>(rec);
                float* rw = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(rec) + 0x40);
                save.assign(sm, sm + n);
                set_at = sm;
                set_n = n;
                std::memcpy(rec_save, rc, sizeof(rec_save));
                rec_at = rc;
                for (size_t j = 0; j < static_cast<size_t>(count) * 2; ++j) shrink_axes(sm + j * 12);
                shrink_axes(rc);
                shrink_axes(rw);
                g_leave_hidden.fetch_add(1, std::memory_order_relaxed);
                if (const uintptr_t inst = !leave_by_model ? draw_instance(rec) : 0) g_leave_inst.store(inst, std::memory_order_relaxed);
            } else if (count >= 1 && count <= 8) {
                if (!leave_at && leave_live()) {  // (round 77: the measure of a draw it missed; here leave_draw was false)
                    const uintptr_t inst = draw_instance(rec);
                    if (inst && inst == g_leave_inst.load(std::memory_order_relaxed)) g_leave_near.fetch_add(1, std::memory_order_relaxed);
                    leave_probe(rec, count);
                }
                // a prop on the player (the hat, the guns, the knife, the rifle on the back): a small draw whose
                // record matrix (+0x00, the drawn one; the translation at +0x30) is within 15 cm of one of the
                // player's attachment bones. On the head: shrunk to a point; elsewhere: its bone's correction.
                // +0x40 is last frame's (PrevWorldViewProjection: the motion vectors only; round 5b's "changing the
                // record moves nothing" was it).
                float* rc = reinterpret_cast<float*>(rec);
                float* rw = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(rec) + 0x40);
                int best = -1;
                float best_d = 0.15f * 0.15f;
                const std::vector<float>& ap = g_frame.attach_pos;
                const int hi = g_frame.held_any && g_frame.held_fix && in_box(g_frame.held_box, rc) ? held_match(rc) : -1;
                if (hi >= 0 && rec[0x10]) {  // (round 81) the item in hand's model, for its put-away (leave_model)
                    g_held_drawable.store(reinterpret_cast<uintptr_t>(rec[0x10]), std::memory_order_relaxed);
                    g_held_drawable_ms.store(log::now_ms(), std::memory_order_relaxed);
                }
                // [Hands] DualWield: the second gun (placed at John's free hand), never the nearest-bone rule
                int si = hi < 0 && g_frame.sec_any && g_frame.held_fix && in_box(g_frame.sec_box, rc) ? sec_match(rc) : -1;
                if (si >= 0) {
                    g_sec_drawable.store(reinterpret_cast<uintptr_t>(rec[0x10]), std::memory_order_relaxed);
                } else if (hi < 0 && g_frame.sec_new >= 0 && g_frame.held_fix && rec[0x10] &&
                           g_frame.sec[g_frame.sec_new].build == g_frame.sec_build &&
                           reinterpret_cast<uintptr_t>(rec[0x10]) == g_sec_drawable.load(std::memory_order_relaxed)) {
                    const float* sm2 = g_frame.sec[g_frame.sec_new].m;
                    const float dx = rc[12] - sm2[12], dy = rc[13] - sm2[13], dz = rc[14] - sm2[14];
                    if (dx * dx + dy * dy + dz * dz < 0.1f * 0.1f) {  // an older placement's draw: the newest sample's correction
                        si = g_frame.sec_new;
                        g_sec_loose.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // run 5: a prop the mod made and holds (held_prop: the shotgun shell, the other revolver), matched by
                // its drawable: its draw moved exactly onto the wanted pose, never hidden, not remembered
                float hp_pose[12];
                int hp_slot = -1, hp_john = -1;
                if (held_prop::diag_on()) held_prop::diag_draw(reinterpret_cast<uintptr_t>(rec[0x10]), rc + 12, t_draw_pass, rc);  // "props diag"
                // 2026-10-27: a spare of the shells' pool (held_prop::want_pool, [Reload] RoundInHand = 2): matched, hidden
                bool hp_hide = false;
                const bool hp = hi < 0 && si < 0 &&
                                held_prop::match(reinterpret_cast<uintptr_t>(rec[0x10]), draw_instance(rec), rc + 12, hp_pose, &hp_slot, &hp_hide);
                if (hp && hp_slot == 1) {  // [Hands] OwnModelFollow: the other sidearm at the copy's hand of this frame
                    std::lock_guard lock(g_draw_mutex);
                    hp_john = own_follow_pose(hp_pose);
                }
                // 2026-10-12 (the user: a second gun at the right hip facing backwards when turning toward it, gone with
                // ShowGuns off): the game's own holstered gun on John's belt, the same model as a holster's shown gun, was
                // matched onto the shown gun's pose only within the 0.3 m gate; turning, it was 0.34-0.38 m from it
                // (r16_backwards6) and drawn on its own at the game's holster. Hidden while a holster shows that model
                bool belt_twin = false;
                const HeldSample* hsm = hi >= 0 ? &g_frame.held[hi] : si >= 0 ? &g_frame.sec[si] : nullptr;
                const int held = hi >= 0 ? (g_frame.held[hi].left ? 0 : 1) : si >= 0 ? g_frame.sec[si].john : -1;
                if (!hsm && g_frame.sec_any) {  // "skel dual": near the second gun, matched to none of its samples
                    for (const HeldSample& hs2 : g_frame.sec) {
                        if (!hs2.valid) continue;
                        const float dx = rc[12] - hs2.m[12], dy = rc[13] - hs2.m[13], dz = rc[14] - hs2.m[14];
                        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d < 0.3f) {
                            g_sec_near.fetch_add(1, std::memory_order_relaxed);
                            if (d < g_sec_near_d) {
                                g_sec_near_d = d;
                                g_sec_near_x = std::fmax(std::fabs(rc[0] - hs2.m[0]), std::fmax(std::fabs(rc[1] - hs2.m[1]), std::fabs(rc[2] - hs2.m[2])));
                            }
                            break;
                        }
                    }
                }
                if (hi < 0 && g_frame.held_any && g_frame.held_new >= 0) {  // "skel lag": near the gun, matched to no sample
                    const float* hm = g_frame.held[g_frame.held_new].m;
                    const float dx = rc[12] - hm[12], dy = rc[13] - hm[13], dz = rc[14] - hm[14];
                    if (dx * dx + dy * dy + dz * dz < 0.05f * 0.05f) note_nearmiss(1, count, rec[0x10], rc, hm);
                }
                if (si >= 0) {  // the second gun: its hand's correction, never hidden, not remembered
                    best = g_rig.att_wrist[held] >= 0 ? g_rig.att_wrist[held] : g_rig.wrist[held];
                    g_sec_draws.fetch_add(1, std::memory_order_relaxed);
                    g_other_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
                    if (g_corr_frame != g_body_frame) g_other_stale_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
                } else if (held >= 0) {  // the item in hand: its hand's correction, never hidden, not remembered
                    best = g_rig.att_wrist[held] >= 0 ? g_rig.att_wrist[held] : g_rig.wrist[held];
                    g_held_draws.fetch_add(1, std::memory_order_relaxed);
                    g_held_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
                    if (g_corr_frame != g_body_frame) {
                        g_held_stale.fetch_add(1, std::memory_order_relaxed);
                        g_stale_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
                    }
                    g_held_frame_draws.fetch_add(1, std::memory_order_relaxed);
                } else if (hp) {  // a held prop (any wrist bone: only for the checks below; its correction is its own)
                    best = g_rig.att_wrist[0] >= 0 ? g_rig.att_wrist[0] : g_rig.wrist[0];
                    g_hprop_draws.fetch_add(1, std::memory_order_relaxed);
                } else if (in_box(g_frame.attach_box, rc)) {  // within 0.5 m of an attachment bone
                for (size_t i = 0; i * 3 + 2 < ap.size(); ++i) {
                    // (2026-10-13: not John's hands' attachments: the item in a hand has its own match above, and a prop
                    // near his fists is someone else's: an NPC's hat mid-punch took the hand's correction, "npc hats pop")
                    const int ab = g_rig.attach[i];
                    if (ab >= 0 && ab < static_cast<int>(g_rig.in_wrist.size()) && g_rig.in_wrist[ab]) continue;
                    float dx = rc[12] - ap[i * 3], dy = rc[13] - ap[i * 3 + 1], dz = rc[14] - ap[i * 3 + 2];
                    float d = dx * dx + dy * dy + dz * dz;
                    if (d < best_d) {
                        best_d = d;
                        best = ab;
                    }
                }
                const uintptr_t drawable = reinterpret_cast<uintptr_t>(rec[0x10]);
                const uintptr_t inst = draw_instance(rec);
                float axn[3] = {rc[0], rc[1], rc[2]};  // this draw's facing (its first axis, made unit)
                {
                    const float l = std::sqrt(axn[0] * axn[0] + axn[1] * axn[1] + axn[2] * axn[2]);
                    if (l > 1e-6f)
                        for (float& x : axn) x /= l;
                }
                // the same prop: its model, its instance if known, and a head prop facing within 45 degrees of its last
                // (only a facing seen within the last 10 frames: 2026-10-13, the user: "after using the gatling gun I could
                // breifly see my hat": the body off on the gun, John turned with it, and his hat's old facing took it for
                // another's until the memory ran out, 450 frames)
                auto same = [&](const PropBone& pb) {
                    if (pb.drawable != drawable || (pb.inst && inst && pb.inst != inst)) return false;
                    if (!pb.head || (pb.ax[0] == 0.0f && pb.ax[1] == 0.0f && pb.ax[2] == 0.0f) || g_body_frame - pb.frame > 10) return true;
                    return pb.ax[0] * axn[0] + pb.ax[1] * axn[1] + pb.ax[2] * axn[2] > 0.7f;
                };
                // a model known as a head prop, drawn facing away from it: another's (an NPC's hat of John's model):
                // left alone, never matched to another of John's bones either
                bool foreign_head = false;
                belt_twin = best >= 0 && held_prop::shown_at_holster(drawable);  // before the draw lock (held_prop's own)
                {
                    std::lock_guard lock(g_draw_mutex);
                    for (const PropBone& pb : g_prop_bones)
                        if (pb.drawable == drawable && pb.head && !same(pb)) foreign_head = true;
                    for (const PropBone& pb : g_prop_bones)
                        if (same(pb) && pb.head) foreign_head = false;  // (one of its matches fits: John's)
                    // (on John's head, within 5 cm of a head bone: his, whatever its facing)
                    if (best >= 0 && best < static_cast<int>(g_rig.in_head.size()) && g_rig.in_head[best] && best_d <= 0.05f * 0.05f)
                        foreign_head = false;
                    if (foreign_head) {
                        best = -1;
                        g_head_foreign.fetch_add(1, std::memory_order_relaxed);
                    }
                    const bool head_5cm = g_frame.held_fix;  // [Body] HeldPropFix: also this rule
                    if (head_5cm && best >= 0 && g_rig.in_head[best]) {
                        // a head prop is one first matched within 5 cm of a head bone (the hat: 0-1 cm); anything else
                        // near the head (a thrown stick or a knife in the game's overhand throw, a projectile) is not
                        // hidden with it
                        bool known_head = false;
                        for (const PropBone& pb : g_prop_bones)
                            if (same(pb) && pb.head) known_head = true;
                        if (!known_head && best_d > 0.05f * 0.05f) {
                            best = -1;
                            g_head_refused.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (best >= 0) {
                        bool found = false;
                        for (PropBone& pb : g_prop_bones)
                            if (same(pb)) {
                                // a prop once on the head stays a head prop: the hat bobs between head_Attachment and
                                // the nearer neck_Attachment while sprinting (round 5c: it flickered)
                                if ((head_5cm ? pb.head : g_rig.in_head[pb.bone] != 0) && !g_rig.in_head[best]) best = pb.bone;
                                else if (g_rig.in_head[best] && best_d <= 0.05f * 0.05f) pb.head = true;
                                pb.bone = best;
                                pb.frame = g_body_frame;
                                std::memcpy(pb.ax, axn, sizeof(pb.ax));
                                found = true;
                            }
                        if (!found && drawable)
                            g_prop_bones.push_back({drawable, best, g_body_frame, g_rig.in_head[best] && (!head_5cm || best_d <= 0.05f * 0.05f), inst,
                                                    {axn[0], axn[1], axn[2]}});
                    } else if (drawable && !foreign_head) {
                        for (PropBone& pb : g_prop_bones) {
                            if (!same(pb)) continue;
                            // (a head prop kept within 25 cm of its bone: a frame's lag at a gallop; 0.5 m took an NPC's hat)
                            const float keep = pb.head ? 0.25f : 0.5f;
                            for (size_t i = 0; i * 3 + 2 < ap.size(); ++i) {
                                if (g_rig.attach[i] != pb.bone) continue;
                                float dx = rc[12] - ap[i * 3], dy = rc[13] - ap[i * 3 + 1], dz = rc[14] - ap[i * 3 + 2];
                                if (dx * dx + dy * dy + dz * dz < keep * keep) {
                                    best = pb.bone;
                                    pb.frame = g_body_frame;
                                    if (pb.head) std::memcpy(pb.ax, axn, sizeof(pb.ax));
                                }
                            }
                        }
                    }
                    if (best < 0 && !foreign_head)  // (the diagnostic: his hat's model, its facing his, not kept)
                        for (const PropBone& pb : g_prop_bones)
                            if (pb.head && same(pb)) {
                                g_head_lost.fetch_add(1, std::memory_order_relaxed);
                                break;
                            }
                }
                }  // not the held item
                // (cycle N49: the holstered knife's instance is not John's skeleton: by its bone alone, within 15 cm of it, and used
                // only while still by that bone)
                // (2026-11-04, round 79 on the other PC: the knife "now plays full put away animation"; its log: every leaving draw
                // put at a kept place, 0 hidden: the knife in hand, never matched as the held gun, was kept as holstered on an
                // arm bone, and its way then followed the game's animated arm) only with nothing in hand (the fists, -1)
                // (round 80, the user: the knife "still plays whole put away animation"; its log: its way drawn at its kept place:
                // hung by the nearest bone, the empty hand's forearm by the hip, which the holster clip swings) never an arm's bone
                // (the review: a draw that hangs from a wrist is an item in hand, whatever bone is near: never kept)
                if (best >= 0 && hi < 0 && si < 0 && !hp && !leave_at && g_frame.weapon < 0 && g_leave_subst_on.load(std::memory_order_relaxed) &&
                    !(best < static_cast<int>(g_rig.in_wrist.size()) && g_rig.in_wrist[best]))
                    hseen_note(reinterpret_cast<uintptr_t>(rec[0x10]), rc, best);  // (round 78) a holstered prop's place on John (by its model), for a put-away's draws
                if (best >= 0 && hi < 0 && si < 0 && !hp) {  // (the diagnostic: whose the prop's instance is)
                    const bool own_skel = draw_instance(rec) == g_frame.skel;
                    (own_skel ? g_prop_own_skel : g_prop_other_owner).fetch_add(1, std::memory_order_relaxed);
                    if (g_rig.in_head[best]) (own_skel ? g_prop_head_own : g_prop_head_other).fetch_add(1, std::memory_order_relaxed);
                }
                if (best >= 0) {
                    std::lock_guard lock(g_draw_mutex);
                    if (g_nprop_diag < 8) {
                        PropDiag& pd = g_prop_diag[g_nprop_diag++];
                        pd.bone = best;
                        pd.count = count;
                        pd.flags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(rec) + 0xac);
                        for (int k = 0; k < 3; ++k) {
                            pd.set_t[k] = sm[k * 4 + 3];
                            pd.set_x[k] = sm[k * 4];
                            pd.rec_t[k] = rc[12 + k];
                            pd.rec_x[k] = rc[k];
                            pd.rec_z[k] = rc[8 + k];
                        }
                        pd.cor = g_corr_world_valid && best < static_cast<int>(g_corr_world.size());
                        pd.pinned = best < static_cast<int>(g_rig.belt_pin.size()) && g_rig.belt_pin[best];
                        if (pd.cor) {
                            const float* A = g_corr_world[best].A;
                            for (int k = 0; k < 3; ++k) {
                                pd.cor_x[k] = A[k * 3] * rc[0] + A[k * 3 + 1] * rc[1] + A[k * 3 + 2] * rc[2];
                                pd.cor_z[k] = A[k * 3] * rc[8] + A[k * 3 + 1] * rc[9] + A[k * 3 + 2] * rc[10];
                            }
                        }
                    }
                    save.assign(sm, sm + n);
                    set_at = sm;
                    set_n = n;
                    std::memcpy(rec_save, rc, sizeof(rec_save));
                    rec_at = rc;
                    const bool part_hidden = g_frame.show && !(g_frame.show == 1 ? g_rig.in_elbow[best] : g_rig.in_wrist[best]);
                    if (belt_twin) g_belt_twins.fetch_add(1, std::memory_order_relaxed);
                    if ((g_frame.hide && g_rig.in_head[best]) || part_hidden || belt_twin || (hp && hp_hide)) {
                        if (hp && hp_hide) g_hprop_hidden.fetch_add(1, std::memory_order_relaxed);
                        for (size_t j = 0; j < static_cast<size_t>(count) * 2; ++j) shrink_axes(sm + j * 12);
                        shrink_axes(rc);
                        shrink_axes(rw);
                        if (part_hidden && g_frame.still_cloth) wind_y = 0.0f;
                        g_hat_hides.fetch_add(1, std::memory_order_relaxed);
                    } else if (g_corr_world_valid && best < static_cast<int>(g_corr_world.size())) {
                        // the prop's set holds local matrices (measured: identity) under the record's matrix (world =
                        // R (M v + t) + t_rec: the current half under +0x00, the previous half under +0x40): the world
                        // correction (A, a) goes into the set in that record frame: M' = R^T A R M,
                        // t' = R^T (A (R t + t_rec) + a - t_rec)
                        Corr held_w, held_s;  // a held item placed at the drawn hand: only what the hand moved since
                        const bool use_held = hsm && hsm->placed;
                        if (use_held) held_delta(*hsm, best, &held_w, &held_s);
                        const bool xf = hi >= 0 && g_item_xfer;  // the held item in John's other hand
                        bool steady_on = false;  // the parts of a steadied model drawn (the gun is steadied after the correction)
                        if (xf && !use_held) g_xfer_draws.fetch_add(1, std::memory_order_relaxed);
                        int bb_weapon = -1;  // [Reload] CarriedBolt's check: a bolt gun's bolt drawn in this draw (its eWeapon)
                        if (hi >= 0) {  // run 6 item 9: the gun in hand's parts drawn from the player's gestures, in its own axes first
                            const bool rm_ = (*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(rec) + 0xac) & 0x10000) != 0;
                            g_held_set_count.store(count, std::memory_order_relaxed);
                            g_held_set_rm.store(rm_ ? 1 : 0, std::memory_order_relaxed);
                            if (g_parts_auto && g_parts_rec.load(std::memory_order_relaxed) && g_parts_frame != ~0ull && g_body_frame > g_parts_frame + 30) {
                                g_parts_auto = false;  // stalled (the gun put away, another drawn): dropped, not logged with mixed frames
                                g_parts_rec = false;
                                g_parts_n = 0;
                            }
                            if (g_parts_auto && !g_parts_rec.load(std::memory_order_relaxed)) {  // [Debug] TwoHandShotLog: its end
                                g_parts_auto = false;
                                g_shotlogs.fetch_add(1, std::memory_order_relaxed);
                                const int pz = g_grip_pose_pub.load(std::memory_order_relaxed);  // the hold used (FrontHandPose's or the stance's)
                                log::info("[two] a shot two-handed (weapon %d; blend %.2f -> %.2f, front pose %d -> %d, fixed grip %d, snap to the drawn gun "
                                          "%d): %s",
                                          g_parts_auto_weapon, g_parts_auto_blend, holster::two_hand_blend(), g_parts_auto_pose, pz,
                                          g_fixed_grip.load() ? 1 : 0, g_snap_drawn_cfg.load() ? 1 : 0, parts_post().c_str());
                            }
                            if (g_shotlog_cfg.load(std::memory_order_relaxed) && !g_parts_auto && !g_parts_manual && !g_parts_rec.load(std::memory_order_relaxed) &&
                                actions::since_shot_ms() < 60.0 && holster::two_hand_blend() > 0.5f) {  // a shot two-handed: recorded
                                g_parts_n = 0;
                                g_parts_frame = ~0ull;
                                g_parts_auto = true;
                                g_parts_auto_blend = holster::two_hand_blend();
                                g_parts_auto_pose = g_grip_pose_pub.load(std::memory_order_relaxed);
                                RdrvrActorState ast{};
                                g_parts_auto_weapon = api::actor_state(&ast) ? ast.weapon : -1;
                                g_parts_rec = true;
                            }
                            if (g_parts_rec.load(std::memory_order_relaxed))
                                record_parts(sm, count, g_body_frame, rc, reinterpret_cast<const float*>(mset + 0x10));
                            float drv[kDrvCount];
                            int gw = -1;
                            unsigned mask = actions::gun_drivers(&gw, drv);
                            // ([Reload] LeverParts: the lever and the hammer were not drawn on a transplanted gun, the steadying held
                            // to the game's hand. 2026-11-01, GOAL-offhand-changes C20; round 64, the user: the left-hand revolver's
                            // hammer does not move when cocked: drawn there too, the steadying in the hand drawn in since C14)
                            const unsigned done = mask ? gun_parts(sm, count, gw, drv, mask) : 0u;
                            if (done & 1u << kDrvOpen) g_open_draws.fetch_add(1, std::memory_order_relaxed);
                            if (done & (1u << kDrvLever | 1u << kDrvHammer)) g_lever_draws.fetch_add(1, std::memory_order_relaxed);
                            // any steadied model's parts (the lever guns, the pump, the bolts, the single shots): not only the
                            // lever's drivers (cycles L2 and M1: the Bolt Action swung 102 degrees through John's fire clip)
                            if (done && (gun_flags(gw) & kSteady)) steady_on = true;
                            // (2026-11-01, F5, cycle F4U3: the Blunderbuss's only part, its rod, is drawn only while rammed: after its
                            // shot no part was drawn, so it was never steadied) the worked gun's model steadied by its flags too
                            // (the review: only the worked gun's own model, by its bone count, not another drawable near it)
                            if (!steady_on) {
                                const GunModel* wm = gun_model(actions::parts_weapon(), count);
                                if (wm && (wm->flags & kSteady)) steady_on = true;
                            }
                            if ((done & 1u << kDrvBoltLift) && (gw == 13 || gw == 20 || gw == 31)) bb_weapon = gw;  // (the bolt guns: not the Semi-Auto pistol's toggle)
                        } else if (si >= 0) {  // [Reload] CarriedBolt: the carried bolt gun's bolt, as the gun hand works it
                            float cdrv[kDrvCount];
                            int cw = -1;
                            const unsigned cmask = actions::carry_drivers(&cw, cdrv);
                            if (cmask && gun_parts(sm, count, cw, cdrv, cmask)) {
                                g_carry_part_draws.fetch_add(1, std::memory_order_relaxed);
                                if (cmask & 1u << kDrvBoltLift) bb_weapon = cw;  // (a carried bolt gun's; not CarriedPump's fore-end)
                            }
                            // [Hands] DualWieldOpen (2026-10-28): the second gun opened by its own stick, drawn open (OpenCylinder)
                            float odrv[kDrvCount];
                            int ow = -1;
                            const unsigned omask = actions::second_drivers(false, &ow, odrv);
                            if (omask && (gun_parts(sm, count, ow, odrv, omask) & 1u << kDrvOpen)) g_sec_open_draws.fetch_add(1, std::memory_order_relaxed);
                        } else if (hp && hp_slot == 1) {  // and the copy's prop (DualWieldCopy: CopyAsProp, or the other sidearm's model)
                            float odrv[kDrvCount];
                            int ow = -1;
                            const unsigned omask = actions::second_drivers(true, &ow, odrv);
                            if (omask && (gun_parts(sm, count, ow, odrv, omask) & 1u << kDrvOpen)) g_sec_open_draws.fetch_add(1, std::memory_order_relaxed);
                        }
                        Corr hp_c{};
                        if (hp) {  // from the record's axes and position (the set's root when the draw has no record matrix)
                            const bool rm_ = (*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(rec) + 0xac) & 0x10000) != 0;
                            float Rc[9], pc[3];
                            if (rm_) {
                                for (int i = 0; i < 3; ++i)
                                    for (int j = 0; j < 3; ++j) Rc[i * 3 + j] = rc[j * 4 + i];
                                for (int k = 0; k < 3; ++k) pc[k] = rc[12 + k];
                            } else {
                                const float* po = reinterpret_cast<const float*>(mset + 0x10);
                                for (int i = 0; i < 3; ++i)
                                    for (int j = 0; j < 3; ++j) Rc[i * 3 + j] = sm[i * 4 + j];
                                for (int k = 0; k < 3; ++k) pc[k] = sm[k * 4 + 3] + po[k];
                            }
                            hp_c = pose_corr(Rc, pc, hp_pose);
                        }
                        const Corr& c = hp ? hp_c : use_held ? held_w : xf ? g_item_world[held] : g_corr_world[best];
                        // Without flag 0x10000 the draw gets no record matrix: the set is a skinned one, in the same
                        // camera-relative space as the player's own set (a held rifle: the player set's correction for
                        // its bone, as is, put it in the hands)
                        const bool rec_matrix = (*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(rec) + 0xac) & 0x10000) != 0;
                        if (count > 0 && (hi >= 0 || si >= 0 || (hp && hp_slot == 1)) && holster::interaction_highlights() == 3) {  // (parts)
                            gl_role = hi >= 0 ? 0 : 1;
                            gl_weapon = hi >= 0 ? holster::gun_in_hand() : si >= 0 ? dual::state().weapon : dual::copy_model();
                            gl_m = sm;
                            gl_rec = rec_matrix ? rc : nullptr;
                            gl_po = rec_matrix ? nullptr : reinterpret_cast<const float*>(mset + 0x10);
                        }
                        if (!rec_matrix && hp) {  // a held prop's skinned set: the world less its offset, moved in the world
                            const float* po = reinterpret_cast<const float*>(mset + 0x10);
                            for (size_t j = 0; j < static_cast<size_t>(count) * 2; ++j) {
                                float* m = sm + j * 12;
                                const float mb[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                                float nm[9], t[3] = {m[3] + po[0], m[7] + po[1], m[11] + po[2]};
                                mul3(c.A, mb, nm);
                                for (int k = 0; k < 3; ++k) {
                                    m[k * 4 + 0] = nm[k * 3 + 0];
                                    m[k * 4 + 1] = nm[k * 3 + 1];
                                    m[k * 4 + 2] = nm[k * 3 + 2];
                                    m[k * 4 + 3] = c.A[k * 3] * t[0] + c.A[k * 3 + 1] * t[1] + c.A[k * 3 + 2] * t[2] + c.a[k] - po[k];
                                }
                            }
                            g_hprop_skinned.fetch_add(1, std::memory_order_relaxed);
                        } else if (!rec_matrix) {
                            if (g_skinned_bone < 0) {
                                g_skinned_bone = best;
                                g_skinned_n = count > 8 ? 8 : count;
                                std::memcpy(g_skinned_diag, sm, sizeof(float) * 12 * g_skinned_n);
                                std::memcpy(g_skinned_hdr, reinterpret_cast<const void*>(mset), sizeof(g_skinned_hdr));
                                std::memcpy(g_skinned_rec, rec, sizeof(g_skinned_rec));
                            }
                            if (best < static_cast<int>(g_corr_set.size())) {
                                // both sets are the world less their header offsets (+0x10, measured: the prop's equals
                                // its record's world position): t' = A (t + d) + a - d, d = the prop's offset less the
                                // player's
                                const Corr& cs = use_held ? held_s : xf ? g_item_set[held] : g_corr_set[best];
                                const float* po = reinterpret_cast<const float*>(mset + 0x10);
                                const float d[3] = {po[0] - g_player_offset[0], po[1] - g_player_offset[1], po[2] - g_player_offset[2]};
                                for (size_t j = 0; j < static_cast<size_t>(count) * 2; ++j) {
                                    float* m = sm + j * 12;
                                    const float mb[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                                    float nm[9], t[3] = {m[3] + d[0], m[7] + d[1], m[11] + d[2]};
                                    mul3(cs.A, mb, nm);
                                    for (int k = 0; k < 3; ++k) {
                                        m[k * 4 + 0] = nm[k * 3 + 0];
                                        m[k * 4 + 1] = nm[k * 3 + 1];
                                        m[k * 4 + 2] = nm[k * 3 + 2];
                                        m[k * 4 + 3] = cs.A[k * 3] * t[0] + cs.A[k * 3 + 1] * t[1] + cs.A[k * 3 + 2] * t[2] + cs.a[k] - d[k];
                                    }
                                }
                            }
                        } else {
                        for (int half = 0; half < 2; ++half) {
                        const float* rm = half ? rw : rc;
                        float R[9], RT[9], AR[9], B[9];
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j) {
                                R[i * 3 + j] = rm[j * 4 + i];  // columns = the record's axis rows
                                RT[j * 3 + i] = rm[j * 4 + i];
                            }
                        mul3(c.A, R, AR);
                        mul3(RT, AR, B);
                        const float tr_[3] = {rm[12], rm[13], rm[14]};
                        for (size_t j = static_cast<size_t>(count) * half; j < static_cast<size_t>(count) * (half + 1); ++j) {
                            float* m = sm + j * 12;
                            const float mb[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                            float nm[9], t[3] = {m[3], m[7], m[11]}, wt[3], v[3];
                            mul3(B, mb, nm);
                            for (int k = 0; k < 3; ++k) wt[k] = R[k * 3] * t[0] + R[k * 3 + 1] * t[1] + R[k * 3 + 2] * t[2] + tr_[k];
                            for (int k = 0; k < 3; ++k)
                                v[k] = c.A[k * 3] * wt[0] + c.A[k * 3 + 1] * wt[1] + c.A[k * 3 + 2] * wt[2] + c.a[k] - tr_[k];
                            for (int k = 0; k < 3; ++k) {
                                m[k * 4 + 0] = nm[k * 3 + 0];
                                m[k * 4 + 1] = nm[k * 3 + 1];
                                m[k * 4 + 2] = nm[k * 3 + 2];
                                m[k * 4 + 3] = RT[k * 3] * v[0] + RT[k * 3 + 1] * v[1] + RT[k * 3 + 2] * v[2];
                            }
                        }
                        }
                        }
                        // item 1b's check ("skel drawngun"): the gun in hand's root after the correction, before the steadying
                        DrawnGun dg_t;
                        const bool dg_on = hi >= 0 && count > 0 && (t_draw_pass & 7) != 0 && g_dg_on.load(std::memory_order_relaxed);
                        if (dg_on)
                            drawn_gun_stage(dg_t, 0, sm, rec_matrix ? rc : nullptr, rec_matrix ? nullptr : reinterpret_cast<const float*>(mset + 0x10), nullptr);
                        if ((si >= 0 || (hp && hp_slot == 1)) && count > 0 && (t_draw_pass & 7) != 0 && (g_dg2_on.load(std::memory_order_relaxed) || g_dg2_scope.load(std::memory_order_relaxed) || g_dg2_bolt.load(std::memory_order_relaxed))) {
                            DrawnGun d2;  // (round 38 check 2's check) the second gun or the copy's prop as drawn
                            drawn_gun_stage(d2, 0, sm, rec_matrix ? rc : nullptr, rec_matrix ? nullptr : reinterpret_cast<const float*>(mset + 0x10), nullptr);
                            std::memcpy(d2.R[1], d2.R[0], sizeof(d2.R[0]));
                            std::memcpy(d2.o[1], d2.o[0], sizeof(d2.o[0]));
                            drawn_second_publish(d2, g_body_frame, si < 0);
                        }
                        // (2026-11-01, GOAL-offhand-fixes F5; round 63 checks 12 and 20, "the Blunderbuss plays an animation after you
                        // fire it"; the user's log and r88 U3: the drawn Blunderbuss 846 mm, 129 deg off the hand through John's 4 s
                        // ramming clip) a set under its record's matrix (the Blunderbuss's) steadied too: its bones taken into the
                        // world through the record (rw for the previous half), steadied there, put back
                        if (steady_on) {  // the drawn gun kept in the drawn hand through John's fire clip
                            int sh = -1;
                            for (int h = 0; h < 2; ++h)
                                if (g_rig.att_wrist[h] == best) sh = h;
                            // (2026-11-01, GOAL-offhand-changes C14; round 64, the user: the Volcanic alone in the left hand stops
                            // where it was after a shot, not following the hand) the transplanted gun (drawn in John's other hand
                            // than the game's) steadied in the hand it is drawn in: F5's steadying by the model's flags reached it,
                            // held to the game's hand's target, the still front controller
                            if (xf && sh >= 0) sh = 1 - sh;
                            Steady* sd = sh >= 0 && g_steady[sh].wrist_ok ? &g_steady[sh] : nullptr;
                            const float* po = reinterpret_cast<const float*>(mset + 0x10);
                            const float d[3] = {po[0] - g_player_offset[0], po[1] - g_player_offset[1], po[2] - g_player_offset[2]};
                            float root[12], T[12], ti[12];
                            if (rec_matrix) {  // (F5) the root in the world through its record, then the player set's space
                                rec_to_world(rc, sm, root);
                                for (int k = 0; k < 3; ++k) root[k * 4 + 3] -= g_player_offset[k];
                            } else {
                                std::memcpy(root, sm, sizeof(root));  // bone 0, the root, as drawn (the player set's space)
                                for (int k = 0; k < 3; ++k) root[k * 4 + 3] += d[k];
                            }
                            const bool have_t = sh >= 0 && g_frame.ik[sh];
                            if (have_t)
                                for (int i = 0; i < 3; ++i) {  // the target: its axes as columns, its position less the set's offset
                                    for (int j = 0; j < 3; ++j) T[i * 4 + j] = g_frame.ik_r[sh][i * 3 + j];
                                    T[i * 4 + 3] = g_frame.ik_t[sh][i] - g_player_offset[i];
                                }
                            const bool clip = in_fire_clip();
                            const double now_ms = log::now_ms();
                            if (sd && clip) sd->clip_end_ms = now_ms;
                            const float fade = sd && !clip ? static_cast<float>((now_ms - sd->clip_end_ms) / 150.0) : 0.0f;
                            if (sd && have_t && !clip && fade >= 1.0f && inv34(T, ti)) {
                                mul34(ti, root, sd->G);
                                sd->rest_ok = true;
                                sd->on = false;
                            } else if (sd && have_t && sd->rest_ok) {
                                float X[12];
                                if (steady_x(T, sd->G, root, fade, X)) {
                                    for (size_t j = 0; j < static_cast<size_t>(count) * 2; ++j) {
                                        float* m = sm + j * 12;
                                        float md[12];
                                        if (rec_matrix) {  // (F5) through its record (rw: the previous half's), in the player set's space
                                            const float* rm = j < static_cast<size_t>(count) ? rc : rw;
                                            float w[12], ws[12];
                                            rec_to_world(rm, m, w);
                                            for (int k = 0; k < 3; ++k) w[k * 4 + 3] -= g_player_offset[k];
                                            mul34(X, w, ws);
                                            for (int k = 0; k < 3; ++k) ws[k * 4 + 3] += g_player_offset[k];
                                            rec_from_world(rm, ws, m);
                                            continue;
                                        }
                                        std::memcpy(md, m, sizeof(md));
                                        for (int k = 0; k < 3; ++k) md[k * 4 + 3] += d[k];
                                        mul34(X, md, m);
                                        for (int k = 0; k < 3; ++k) m[k * 4 + 3] -= d[k];
                                    }
                                    sd->on = true;
                                    g_steady_draws.fetch_add(1, std::memory_order_relaxed);
                                    g_steady_hand[sh].fetch_add(1, std::memory_order_relaxed);
                                    if (xf) g_steady_xf.fetch_add(1, std::memory_order_relaxed);
                                }
                            } else if (clip) {
                                g_steady_misses.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        if (bb_weapon >= 0 && count > 2 && (t_draw_pass & 7) != 0) {  // [Reload] CarriedBolt's check: the drawn bolt in
                            // the world (after the correction and the steadying, before the kick), not the shadow pre-pass's draw
                            const float* bm = sm + 2 * 12;  // bone 2 (the bolt), the current half: axes as columns, then the move
                            BoltBone& bb = g_bolt_bone[hi >= 0 ? 0 : 1];
                            if (rec_matrix) {  // local in the record's frame: world = the record's rows times it, plus its place
                                for (int k = 0; k < 3; ++k) {
                                    for (int j = 0; j < 3; ++j) bb.R[k * 3 + j] = bm[0 * 4 + j] * rc[0 * 4 + k] + bm[1 * 4 + j] * rc[1 * 4 + k] + bm[2 * 4 + j] * rc[2 * 4 + k];
                                    bb.o[k] = bm[3] * rc[k] + bm[7] * rc[4 + k] + bm[11] * rc[8 + k] + rc[12 + k];
                                }
                            } else {  // the world less the set's offset
                                const float* po = reinterpret_cast<const float*>(mset + 0x10);
                                for (int k = 0; k < 3; ++k) {
                                    for (int j = 0; j < 3; ++j) bb.R[k * 3 + j] = bm[k * 4 + j];
                                    bb.o[k] = bm[k * 4 + 3] + po[k];
                                }
                            }
                            bb.weapon = bb_weapon;
                            bb.ms = log::now_ms();
                        }
                        if (hi >= 0 && count > 0 && g_gun_steady.load(std::memory_order_relaxed)) {  // [Hands] GunSteady (above)
                            const float* po = reinterpret_cast<const float*>(mset + 0x10);
                            double root[3];
                            for (int k = 0; k < 3; ++k)
                                root[k] = rec_matrix ? static_cast<double>(sm[3]) * rc[k] + static_cast<double>(sm[7]) * rc[4 + k] + static_cast<double>(sm[11]) * rc[8 + k] + rc[12 + k]
                                                     : static_cast<double>(sm[k * 4 + 3]) + po[k];
                            // (the review) a stale early draw (last frame's correction, StaleGunFix's) neither judged nor shifted;
                            // the hand the gun follows: the carried gun's put-away its front hand's, the transplant's the other
                            const bool gs_fresh = g_corr_frame == g_body_frame;
                            if (gs_fresh && g_gs_frame != g_body_frame) {  // once a frame
                                g_gs_frame = g_body_frame;
                                const int gj = hsm && hsm->follow_j >= 0 && hsm->follow_j <= 1 ? hsm->follow_j : xf ? 1 - held : held;
                                const float* tg = g_frame.ik_t[gj];
                                const float* cm = g_frame.cam + 12;
                                double mt = 0.0, mc = 0.0, md = 0.0, mag = 0.0;
                                for (int k = 0; k < 3; ++k) {
                                    mt = std::fmax(mt, std::fabs(static_cast<double>(tg[k]) - g_gs_tgt[k]));
                                    mc = std::fmax(mc, std::fabs(static_cast<double>(cm[k]) - g_gs_cam[k]));
                                    md = std::fmax(md, std::fabs(root[k] - g_gs_out[k]));
                                    mag = std::fmax(mag, std::fabs(root[k]));
                                }
                                const float mf = static_cast<float>(mag);
                                const double step = static_cast<double>(std::nextafter(mf, 1e30f) - mf);
                                const bool hold = g_gs_ok && g_gs_prev + 1 == g_body_frame && g_gs_count == count && g_frame.ik[gj] && mt < 2e-5 && mc < 2e-5 &&
                                                  md < 3.5 * step;  // (cycle N15: at 2.5, 4 of 419 frames at Fort Mercer let go; its flips reach 3.1 steps)
                                for (int k = 0; k < 3; ++k) {
                                    g_gs_delta[k] = hold ? g_gs_out[k] - root[k] : 0.0;
                                    g_gs_out[k] = root[k] + g_gs_delta[k];
                                    g_gs_tgt[k] = tg[k];
                                    g_gs_cam[k] = cm[k];
                                }
                                (hold ? g_gs_held : g_gs_free).fetch_add(1, std::memory_order_relaxed);
                                g_gs_ok = g_frame.ik[gj];
                                g_gs_prev = g_body_frame;
                                g_gs_count = count;
                            }
                            if (gs_fresh && g_gs_frame == g_body_frame && count == g_gs_count &&
                                (g_gs_delta[0] != 0.0 || g_gs_delta[1] != 0.0 || g_gs_delta[2] != 0.0)) {
                                double dl[3];  // the shift in the set's own frame (a record's: its rows are the axes)
                                for (int i = 0; i < 3; ++i)
                                    dl[i] = rec_matrix ? rc[i * 4] * g_gs_delta[0] + rc[i * 4 + 1] * g_gs_delta[1] + rc[i * 4 + 2] * g_gs_delta[2] : g_gs_delta[i];
                                for (int j = 0; j < count; ++j)  // the current half: every bone of the gun moved alike
                                    for (int i = 0; i < 3; ++i) sm[j * 12 + i * 4 + 3] = static_cast<float>(sm[j * 12 + i * 4 + 3] + dl[i]);
                            }
                        }
                        if (dg_on) {  // and after it (as seen), before the kick
                            drawn_gun_stage(dg_t, 1, sm, rec_matrix ? rc : nullptr, rec_matrix ? nullptr : reinterpret_cast<const float*>(mset + 0x10), nullptr);
                            drawn_gun_publish(dg_t, g_body_frame, hi != g_frame.held_new);
                        }
                        // [Hands] GunKick (2026-10-27; after optimumbox's recoil, 0.8.11.10): the kick on the drawn gun only, last
                        // (after the correction and the steadying), so StaleGunFix and GunMotionVectors take it as drawn
                        // (hi_kl, hi_klp: the gun in hand's kick on each half, for the copy's undo and the diagnostics' unkick)
                        float hi_kl[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, hi_klp[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
                        float sec_kl[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // (F10, the review: the second gun's or the copy's kick, for its early draws)
                        // (cycle r179: a set under its record's matrix as well: the hand's correction above went into its local matrices,
                        // the record left alone, and so does the kick: root M0 -> M0 X, every other bone K Mj, K = M0 X M0^-1, all
                        // local, so the drawn world (the record times them) takes the kick in the gun's axes exactly as a skinned draw)
                        const int kick_r = hi >= 0 ? 0 : si >= 0 || (hp && hp_slot == 1) ? 1 : -1;
                        const int kick_side = hi >= 0 ? (xf ? 1 - held : held) : si >= 0 ? held : hp_john >= 0 ? hp_john : g_frame.kick_j[1];
                        // (the review: count > 0, or an empty set would be taken for a rigid record and 32 floats written unrestored)
                        if (g_frame.kick_any && kick_r >= 0 && count > 0)  // the gun in hand; the second gun; the copy as a prop
                            kick_draw(kick_r, kick_side, sm, count, kick_r == 0 ? hi_kl : sec_kl, kick_r == 0 ? hi_klp : nullptr);
                        if (g_frame.kick_any && kick_r >= 0 && rec_matrix) g_kick_recm[kick_r].fetch_add(1, std::memory_order_relaxed);
                        if (hi >= 0 && !rec_matrix) {  // [Hands] GunKick, StaleGunFix (an early draw: then this frame's kick), then GunMotionVectors
                            // (StaleGunFix moves it with the hand it is placed at: the carried gun's put-away, the front hand; not
                            // when held_delta drew it at this frame's front target already: put_by_target)
                            const int ss = hsm->follow_j >= 0 ? hsm->follow_j : xf ? 1 - held : held;
                            if (!put_by_target(*hsm) && stale_place(ss, held, reinterpret_cast<uintptr_t>(rec[0x10]), sm, sm + static_cast<size_t>(count) * 12,
                                            count, reinterpret_cast<const float*>(mset + 0x10), reinterpret_cast<const float*>(mset + 0x20)))
                                kick_stale(hi_kl, sm, count);
                            gun_prev_fix(held, reinterpret_cast<uintptr_t>(rec[0x10]), sm, sm + static_cast<size_t>(count) * 12, count,
                                         reinterpret_cast<const float*>(mset + 0x10), reinterpret_cast<const float*>(mset + 0x20));
                        } else if (si >= 0 && rec_matrix && held >= 0 && held <= 1 && count > 0 && count <= 8 && g_stale_fix.load(std::memory_order_relaxed)) {
                            // (F10, cycle F10d: the second gun's early draws are all sets under their record's matrix, its bones
                            // local to the record) to the world (the record's axis rows times them, plus its place: as the bolt's
                            // read above), StaleGunFix and its cache there, then back into the record's frame (its axes' inverse,
                            // their transpose, as the correction above takes it)
                            float w[2 * 8 * 12], w0[2 * 8 * 12];
                            auto to_world = [&](const float* rm, const float* m, float* o) {
                                for (int k = 0; k < 3; ++k) {
                                    for (int j = 0; j < 4; ++j)
                                        o[k * 4 + j] = rm[0 * 4 + k] * m[0 * 4 + j] + rm[1 * 4 + k] * m[1 * 4 + j] + rm[2 * 4 + k] * m[2 * 4 + j];
                                    o[k * 4 + 3] += rm[12 + k];
                                }
                            };
                            auto from_world = [&](const float* rm, const float* o, float* m) {
                                for (int r_ = 0; r_ < 3; ++r_) {
                                    for (int j = 0; j < 3; ++j) m[r_ * 4 + j] = rm[r_ * 4] * o[j] + rm[r_ * 4 + 1] * o[4 + j] + rm[r_ * 4 + 2] * o[8 + j];
                                    m[r_ * 4 + 3] = rm[r_ * 4] * (o[3] - rm[12]) + rm[r_ * 4 + 1] * (o[7] - rm[13]) + rm[r_ * 4 + 2] * (o[11] - rm[14]);
                                }
                            };
                            for (int j = 0; j < count; ++j) {
                                to_world(rc, sm + j * 12, w + j * 12);
                                to_world(rw, sm + (count + j) * 12, w + (count + j) * 12);
                            }
                            std::memcpy(w0, w, sizeof(float) * 24 * count);
                            const float zero[3] = {0.0f, 0.0f, 0.0f};
                            const uintptr_t skey = reinterpret_cast<uintptr_t>(rec[0x10]) | 1u;
                            const bool pbt = put_by_target(*hsm);
                            if (!pbt && stale_place(hsm->follow_j >= 0 ? hsm->follow_j : held, held, skey, w, w + count * 12, count, zero, zero)) {
                                kick_stale(sec_kl, w, count, 1);
                                g_stale_shifted_other.fetch_add(1, std::memory_order_relaxed);
                            } else if (g_corr_frame != g_body_frame) {
                                g_sec_unmoved[pbt ? 1 : 1 + t_sp_why].fetch_add(1, std::memory_order_relaxed);
                            }
                            gun_prev_fix(held, skey, w, w + count * 12, count, zero, zero);
                            for (int j = 0; j < 2 * count; ++j)  // (only what changed: no round trip's rounding on the rest)
                                if (std::memcmp(w + j * 12, w0 + j * 12, sizeof(float) * 12)) from_world(j < count ? rc : rw, w + j * 12, sm + j * 12);
                        } else if (si >= 0 && rec_matrix && g_corr_frame != g_body_frame) {
                            g_sec_unmoved[0].fetch_add(1, std::memory_order_relaxed);  // (F10's diagnostic: its hand or count out of range)
                        }
                        if (si >= 0 && !rec_matrix && held >= 0 && g_stale_fix.load(std::memory_order_relaxed)) {  // (F10's: all off with the fix)
                            // (2026-11-01, GOAL-offhand-fixes F10; round 63 check 17, the user: a Sawed-off dual wielded lags
                            // indoors; the log: the second gun's draws stale every frame in the depth pre-pass with the Sawed-off
                            // of slot 6 the second gun, shown by ACTOR_FORCE_WEAPON_RENDER) StaleGunFix and its cache for the
                            // second gun's draws as for the gun in hand's (its own drawable: its own entries; its kick on an early
                            // draw last frame's)
                            const int ss = hsm->follow_j >= 0 ? hsm->follow_j : held;
                            // (the review: its own cache entries, never the gun in hand's: the copy shares its drawable, and the
                            // left hand's draw keeps the game's hand index; drawables are aligned, so key | 1 is no drawable's)
                            const uintptr_t skey = reinterpret_cast<uintptr_t>(rec[0x10]) | 1u;
                            const bool pbt = put_by_target(*hsm);
                            if (!pbt && stale_place(ss, held, skey, sm, sm + static_cast<size_t>(count) * 12, count,
                                                    reinterpret_cast<const float*>(mset + 0x10), reinterpret_cast<const float*>(mset + 0x20))) {
                                kick_stale(sec_kl, sm, count, 1);
                                g_stale_shifted_other.fetch_add(1, std::memory_order_relaxed);
                            } else if (g_corr_frame != g_body_frame) {
                                g_sec_unmoved[pbt ? 1 : 1 + t_sp_why].fetch_add(1, std::memory_order_relaxed);
                            }
                            gun_prev_fix(held, skey, sm, sm + static_cast<size_t>(count) * 12, count, reinterpret_cast<const float*>(mset + 0x10),
                                         reinterpret_cast<const float*>(mset + 0x20));
                        } else if (hi < 0 && si < 0 && hp && hp_slot == 1 && !rec_matrix && hp_john >= 0 && hp_john <= 1 && g_stale_fix.load(std::memory_order_relaxed)) {
                            // (F10: "the second Sawed-off as the copy lags indoors") the copy drawn as a prop (its own model,
                            // held_prop slot 1): the same, at its hand
                            const uintptr_t pkey = reinterpret_cast<uintptr_t>(rec[0x10]) | 1u;  // (the review: as the second gun's)
                            if (stale_place(hp_john, hp_john, pkey, sm, sm + static_cast<size_t>(count) * 12, count, reinterpret_cast<const float*>(mset + 0x10),
                                            reinterpret_cast<const float*>(mset + 0x20))) {
                                kick_stale(sec_kl, sm, count, 1);
                                g_stale_shifted_other.fetch_add(1, std::memory_order_relaxed);
                            }
                            gun_prev_fix(hp_john, pkey, sm, sm + static_cast<size_t>(count) * 12, count, reinterpret_cast<const float*>(mset + 0x10),
                                         reinterpret_cast<const float*>(mset + 0x20));
                        }
                        if (g_frame.kick_any && kick_r >= 0)  // ("skel recoil trace": as drawn, after both)
                            kick_note(kick_r, kick_side, sm, count, rec_matrix ? nullptr : reinterpret_cast<const float*>(mset + 0x10),
                                      rec_matrix ? nullptr : reinterpret_cast<const float*>(mset + 0x20), rec_matrix ? rc : nullptr);
                        if (hi >= 0 && !rec_matrix) {  // run 6 item 1, "skel follow": the gun in hand's root as drawn (world)
                            const float* po = reinterpret_cast<const float*>(mset + 0x10);
                            float m[12];
                            kick_unkick(sm, hi_kl, m);  // ([Hands] GunKick: the gun as held, not its kick)
                            for (int k = 0; k < 3; ++k) m[k * 4 + 3] += po[k];
                            follow_obj(0, xf ? 1 - held : held, m);
                        }
                        if (count > 0 && ((hi >= 0 && g_frame.long_gun) || (si >= 0 && g_frame.carry_j >= 0)))  // ("skel carry trace": as drawn)
                            carry_drawn_note(hi >= 0 ? (hsm->follow_j >= 0 ? 2 : 1) : hsm->put ? 2 : 3, sm, rec_matrix ? rc : nullptr,
                                             rec_matrix ? nullptr : reinterpret_cast<const float*>(mset + 0x10), nullptr);
                        if (hp && hp_slot == 1) {  // and the other sidearm's own model's pose (its draw is moved exactly onto it)
                            float m[12];
                            pose_to34(hp_pose, m);
                            follow_obj(1, hp_john, m);
                        }
                        // as drawn in an eye's pass (2026-10-10: the frame's first draw is the shadow pre-pass's, pass 0)
                        if (hi >= 0 && !rec_matrix && g_parts_n > 0 && g_parts_frame == g_body_frame && !g_parts[g_parts_n - 1].post_ok &&
                            ((t_draw_pass & 7) == 1 || (t_draw_pass & 7) == 3)) {
                            PartsSample& ps = g_parts[g_parts_n - 1];
                            const float* po = reinterpret_cast<const float*>(mset + 0x10);
                            kick_unkick(sm, hi_kl, ps.post);  // ([Hands] GunKick: the gun as held: the kick is not the front hand moving on it)
                            for (int k = 0; k < 3; ++k) ps.post[k * 4 + 3] += po[k] - g_player_offset[k];
                            std::memcpy(ps.left, g_steady[0].Sw, sizeof(ps.left));
                            ps.snap_ok = g_points.gun_frame_ok;
                            for (int i = 0; i < 3; ++i) {
                                for (int j = 0; j < 3; ++j) ps.snap[i * 4 + j] = g_points.gun_frame_R[i * 3 + j];
                                ps.snap[i * 4 + 3] = g_points.gun_frame_o[i] - g_player_offset[i];
                            }
                            ps.post_ok = true;
                        }
                        if (hi >= 0 && hsm && copy_maps(*hsm, c, &cj.Tw, &cj.Ts) && (rec_matrix || best < static_cast<int>(g_corr_set.size()))) {
                            cj.kind = rec_matrix ? 2 : 3;
                            cj.sm = sm;
                            cj.count = count;
                            cj.rc = rc;
                            cj.rw = rw;
                            cj.mset = mset;
                            std::memcpy(cj.kl, hi_kl, sizeof(cj.kl));  // ([Hands] GunKick: the gun in hand's kick, undone on the copy)
                            std::memcpy(cj.klp, hi_klp, sizeof(cj.klp));
                        }
                        if (hp) {
                            HpDiag& d = g_hp_diag;
                            d.path = rec_matrix ? 1 : 2;
                            d.count = count;
                            d.flags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(rec) + 0xac);
                            const float* po = reinterpret_cast<const float*>(mset + 0x10);
                            for (int k = 0; k < 3; ++k) {
                                d.target[k] = hp_pose[9 + k];
                                d.before[k] = rec_matrix ? rec_save[12 + k] : save[k * 4 + 3] + po[k];
                                d.after[k] = rec_matrix ? rc[12 + k] : sm[k * 4 + 3] + po[k];
                                d.xb[k] = rec_matrix ? rec_save[k] : save[k * 4];
                                d.xa[k] = rec_matrix ? rc[k] : sm[k * 4];
                            }
                        }
                        g_props.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    // a rigid draw with no matrix set (the draw takes the record's matrix, +0x00; +0x40 is last frame's): only the item
    // in hand is changed, its hand's correction on both. Round 6 also moved and hid any rigid draw near an attachment
    // bone, but wrote only +0x40, so that never showed (and no prop of the player's draws this way, round 6's intro).
    if (g_frame.valid && rec && !rec[0x12] && rec[0x10] && g_frame.held_any && g_frame.held_fix && g_frame.held_new >= 0 &&
        !in_box(g_frame.held_box, reinterpret_cast<const float*>(rec))) {  // "skel lag": a rigid draw near the gun, outside every sample's box
        const float* rc = reinterpret_cast<const float*>(rec);
        const float* hm = g_frame.held[g_frame.held_new].m;
        const float dx = rc[12] - hm[12], dy = rc[13] - hm[13], dz = rc[14] - hm[14];
        if (dx * dx + dy * dy + dz * dz < 0.05f * 0.05f) note_nearmiss(2, 0, rec[0x10], rc, hm);
    }
    const bool rigid = g_frame.valid && rec && !rec[0x12] && rec[0x10] && g_frame.held_fix;  // the record checked first
    if (g_near_on.load(std::memory_order_relaxed) && g_frame.valid && rec && !rec[0x12] && rec[0x10] && g_frame.held_any && g_frame.held_new >= 0)
        note_near(reinterpret_cast<uintptr_t>(rec[0x10]), 2, 0, reinterpret_cast<const float*>(rec) + 12, g_frame.held[g_frame.held_new].m + 12);
    const bool rigid_held = rigid && g_frame.held_any && in_box(g_frame.held_box, reinterpret_cast<const float*>(rec));
    const bool rigid_sec = rigid && g_frame.sec_any && (in_box(g_frame.sec_box, reinterpret_cast<const float*>(rec)) ||
                                                        reinterpret_cast<uintptr_t>(rec[0x10]) == g_sec_drawable.load(std::memory_order_relaxed));
    float hp_pose2[12];  // run 5: a held prop drawn rigid (held_prop): moved exactly onto its wanted pose
    int hp_slot2 = -1;
    bool hp_hide2 = false;  // 2026-10-27: a spare of the shells' pool: its draw hidden (both halves' axes shrunk)
    if (rigid && held_prop::diag_on())
        held_prop::diag_draw(reinterpret_cast<uintptr_t>(rec[0x10]), reinterpret_cast<const float*>(rec) + 12, t_draw_pass, reinterpret_cast<const float*>(rec));
    // (the review of 2026-10-27) the gun in hand and the second gun matched first, as the skinned path does (its hi < 0
    // && si < 0): a held prop's match, learning by position, could otherwise take the gun's draw for a shell's
    const int rigid_hi = rigid_held ? held_match(reinterpret_cast<const float*>(rec)) : -1;
    const int rigid_si = rigid_hi < 0 && rigid_sec ? sec_match(reinterpret_cast<const float*>(rec)) : -1;
    const bool rigid_hp = rigid && rigid_hi < 0 && rigid_si < 0 &&
                          held_prop::match(reinterpret_cast<uintptr_t>(rec[0x10]), draw_instance(rec), reinterpret_cast<const float*>(rec) + 12, hp_pose2,
                                           &hp_slot2, &hp_hide2);
    if (rigid_hp && hp_hide2) {
        float* rc = reinterpret_cast<float*>(rec);
        std::memcpy(rec_save, rc, sizeof(rec_save));  // (restored after the draw)
        rec_at = rc;
        shrink_axes(rc);
        shrink_axes(rc + 16);
        g_hprop_hidden.fetch_add(1, std::memory_order_relaxed);
    } else if (rigid_hp) {
        float* rc = reinterpret_cast<float*>(rec);
        std::lock_guard lock(g_draw_mutex);
        int hp_john2 = -1;
        if (hp_slot2 == 1) {  // [Hands] OwnModelFollow, and "skel follow"
            const int john = own_follow_pose(hp_pose2);
            hp_john2 = john;
            float m[12];
            pose_to34(hp_pose2, m);
            follow_obj(1, john, m);
        }
        std::memcpy(rec_save, rc, sizeof(rec_save));
        rec_at = rc;
        float Rc[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) Rc[i * 3 + j] = rc[j * 4 + i];
        const Corr c = pose_corr(Rc, rc + 12, hp_pose2);
        for (int half = 0; half < 2; ++half) {
            float* rm = rc + half * 16;
            for (int ax = 0; ax < 3; ++ax) {
                float v[3] = {rm[ax * 4], rm[ax * 4 + 1], rm[ax * 4 + 2]};
                for (int k = 0; k < 3; ++k) rm[ax * 4 + k] = c.A[k * 3] * v[0] + c.A[k * 3 + 1] * v[1] + c.A[k * 3 + 2] * v[2];
            }
            float t[3] = {rm[12], rm[13], rm[14]};
            for (int k = 0; k < 3; ++k) rm[12 + k] = c.A[k * 3] * t[0] + c.A[k * 3 + 1] * t[1] + c.A[k * 3 + 2] * t[2] + c.a[k];
        }
        if (hp_slot2 == 1 && holster::interaction_highlights() == 3) {  // ([Holsters] InteractionHighlights=parts: the copy's prop, rigid)
            gl_role = 1;
            gl_weapon = dual::copy_model();
            gl_rows = rc;
        }
        if (hp_slot2 == 1 && (t_draw_pass & 7) != 0 && (g_dg2_on.load(std::memory_order_relaxed) || g_dg2_scope.load(std::memory_order_relaxed) || g_dg2_bolt.load(std::memory_order_relaxed))) {  // (round 38 check 2's check) the copy's prop, rigid
            DrawnGun d2;
            drawn_gun_stage(d2, 0, nullptr, nullptr, nullptr, rc);
            drawn_gun_stage(d2, 1, nullptr, nullptr, nullptr, rc);
            drawn_second_publish(d2, g_body_frame, true);
        }
        // [Hands] GunKick (2026-10-27): the copy drawn as a prop (slot 1) kicks on its own shots
        if (hp_slot2 == 1 && g_frame.kick_any) {
            kick_draw(1, hp_john2 >= 0 ? hp_john2 : g_frame.kick_j[1], rc, 0, nullptr, nullptr);
            kick_note(1, hp_john2 >= 0 ? hp_john2 : g_frame.kick_j[1], rc, 0, nullptr, nullptr);
        }
        g_hprop_rigid.fetch_add(1, std::memory_order_relaxed);
        g_hp_diag.path = 3;
        for (int k = 0; k < 3; ++k) {
            g_hp_diag.before[k] = rec_save[12 + k];
            g_hp_diag.target[k] = hp_pose2[9 + k];
            g_hp_diag.after[k] = rc[12 + k];
        }
    }
    if ((rigid_held || rigid_sec) && !rigid_hp) {
        float* rc = reinterpret_cast<float*>(rec);
        const int hi = rigid_hi;  // (matched above, once: held_match counts its matches)
        int si = rigid_si;        // [Hands] DualWield: the second gun
        if (si >= 0) {
            g_sec_drawable.store(reinterpret_cast<uintptr_t>(rec[0x10]), std::memory_order_relaxed);
        } else if (hi < 0 && g_frame.sec_new >= 0 && g_frame.sec[g_frame.sec_new].build == g_frame.sec_build &&
                   reinterpret_cast<uintptr_t>(rec[0x10]) == g_sec_drawable.load(std::memory_order_relaxed)) {
            const float* sm2 = g_frame.sec[g_frame.sec_new].m;
            const float dx = rc[12] - sm2[12], dy = rc[13] - sm2[13], dz = rc[14] - sm2[14];
            if (dx * dx + dy * dy + dz * dz < 0.1f * 0.1f) {
                si = g_frame.sec_new;
                g_sec_loose.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const HeldSample* hsm = hi >= 0 ? &g_frame.held[hi] : si >= 0 ? &g_frame.sec[si] : nullptr;
        const int held = hi >= 0 ? (g_frame.held[hi].left ? 0 : 1) : si >= 0 ? g_frame.sec[si].john : -1;
        const int best = held < 0 ? -1 : g_rig.att_wrist[held] >= 0 ? g_rig.att_wrist[held] : g_rig.wrist[held];
        if (!hsm && rigid_held && g_frame.held_new >= 0) note_nearmiss(3, 0, rec[0x10], rc, g_frame.held[g_frame.held_new].m);  // in a box, matched to none
        if (si >= 0 && best >= 0) g_sec_draws.fetch_add(1, std::memory_order_relaxed);
        if (best >= 0) {
            g_held_draws.fetch_add(1, std::memory_order_relaxed);
            g_held_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
            if (g_corr_frame != g_body_frame) {
                g_held_stale.fetch_add(1, std::memory_order_relaxed);
                g_stale_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
            }
            g_held_frame_draws.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(g_draw_mutex);
            std::memcpy(rec_save, rc, sizeof(rec_save));
            rec_at = rc;
            if (g_corr_world_valid && best < static_cast<int>(g_corr_world.size())) {
                Corr held_w, held_s;
                const bool use_held = hsm->placed;
                if (use_held) held_delta(*hsm, best, &held_w, &held_s);
                const Corr& c = use_held ? held_w : hi >= 0 && g_item_xfer ? g_item_world[held] : g_corr_world[best];
                if (holster::interaction_highlights() == 3) {  // ([Holsters] InteractionHighlights=parts: a rigid gun in hand or second gun)
                    gl_role = hi >= 0 ? 0 : 1;
                    gl_weapon = hi >= 0 ? holster::gun_in_hand() : dual::state().weapon;
                    gl_rows = rc;
                }
                for (int half = 0; half < 2; ++half) {
                    float* rm = rc + half * 16;
                    for (int ax = 0; ax < 3; ++ax) {
                        float v[3] = {rm[ax * 4], rm[ax * 4 + 1], rm[ax * 4 + 2]};
                        for (int k = 0; k < 3; ++k) rm[ax * 4 + k] = c.A[k * 3] * v[0] + c.A[k * 3 + 1] * v[1] + c.A[k * 3 + 2] * v[2];
                    }
                    float t[3] = {rm[12], rm[13], rm[14]};
                    for (int k = 0; k < 3; ++k) rm[12 + k] = c.A[k * 3] * t[0] + c.A[k * 3 + 1] * t[1] + c.A[k * 3 + 2] * t[2] + c.a[k];
                }
                float hi_kl[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, hi_klp[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
                if (hi >= 0 && (t_draw_pass & 7) != 0 && g_dg_on.load(std::memory_order_relaxed)) {  // item 1b's check ("skel drawngun"): a rigid gun, corrected, before the kick
                    DrawnGun dg_t;
                    drawn_gun_stage(dg_t, 0, nullptr, nullptr, nullptr, rc);
                    drawn_gun_stage(dg_t, 1, nullptr, nullptr, nullptr, rc);
                    drawn_gun_publish(dg_t, g_body_frame, hi != g_frame.held_new);
                }
                if (si >= 0 && (t_draw_pass & 7) != 0 && (g_dg2_on.load(std::memory_order_relaxed) || g_dg2_scope.load(std::memory_order_relaxed) || g_dg2_bolt.load(std::memory_order_relaxed))) {  // (round 38 check 2's check) a rigid second gun
                    DrawnGun d2;
                    drawn_gun_stage(d2, 0, nullptr, nullptr, nullptr, rc);
                    drawn_gun_stage(d2, 1, nullptr, nullptr, nullptr, rc);
                    drawn_second_publish(d2, g_body_frame, false);
                }
                if (hi >= 0) {  // [Hands] GunKick (the drawn gun only), StaleGunFix (an early draw: then this frame's kick), then GunMotionVectors
                    if (g_frame.kick_any) kick_draw(0, g_item_xfer ? 1 - held : held, rc, 0, hi_kl, hi_klp);
                    if (!put_by_target(*hsm) &&
                        stale_place(hsm->follow_j >= 0 ? hsm->follow_j : g_item_xfer ? 1 - held : held, held, reinterpret_cast<uintptr_t>(rec[0x10]), rc,
                                    rc + 16, 0, nullptr, nullptr))
                        kick_stale(hi_kl, rc, 0);
                    gun_prev_fix(held, reinterpret_cast<uintptr_t>(rec[0x10]), rc, rc + 16, 0, nullptr, nullptr);
                    if (g_frame.kick_any) kick_note(0, g_item_xfer ? 1 - held : held, rc, 0, nullptr, nullptr);
                } else if (si >= 0) {  // [Hands] GunKick: the second gun's own
                    // (F10, F378: the second gun's early draws were rigid records, this branch: StaleGunFix and its cache as
                    // the gun in hand's here too, its own entries, key | 1)
                    float skl[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
                    if (g_frame.kick_any) kick_draw(1, held, rc, 0, skl, nullptr);
                    if (held >= 0 && held <= 1 && g_stale_fix.load(std::memory_order_relaxed)) {
                        const uintptr_t skey = reinterpret_cast<uintptr_t>(rec[0x10]) | 1u;
                        if (!put_by_target(*hsm) && stale_place(hsm->follow_j >= 0 ? hsm->follow_j : held, held, skey, rc, rc + 16, 0, nullptr, nullptr)) {
                            kick_stale(skl, rc, 0, 1);
                            g_stale_shifted_other.fetch_add(1, std::memory_order_relaxed);
                        } else if (g_corr_frame != g_body_frame) {
                            g_sec_unmoved[8].fetch_add(1, std::memory_order_relaxed);
                        }
                        gun_prev_fix(held, skey, rc, rc + 16, 0, nullptr, nullptr);
                    }
                    if (g_frame.kick_any) kick_note(1, held, rc, 0, nullptr, nullptr);
                }
                if ((hi >= 0 && g_frame.long_gun) || (si >= 0 && g_frame.carry_j >= 0))  // ("skel carry trace": as drawn)
                    carry_drawn_note(hi >= 0 ? (hsm->follow_j >= 0 ? 2 : 1) : hsm->put ? 2 : 3, nullptr, nullptr, nullptr, rc);
                if (hi >= 0 && copy_maps(*hsm, c, &cj.Tw, &cj.Ts)) {
                    cj.kind = 1;
                    cj.rc = rc;
                    cj.rw = rc + 16;
                    std::memcpy(cj.kl, hi_kl, sizeof(cj.kl));
                    std::memcpy(cj.klp, hi_klp, sizeof(cj.klp));
                }
            }
            g_rigid_props.fetch_add(1, std::memory_order_relaxed);
        }
    }
    WindHold wind;
    if (wind_y >= 0.0f) wind_hold(&wind, wind_y);
    t_cull_show = cull_show;
    // (GOAL-single-pass item 8e, test only: armed by [Stereo] MainFamily=region) an entity this hook edits per pass (its set or record,
    // a show mode, the wind hold, the copy) is bracketed for the region, which leaves its draws to each pass; off: one relaxed load
    const bool own = main_inst::own_armed() && (set_at || rec_at || cull_show || wind_y >= 0.0f || cj.kind);
    if (own) main_inst::own_mark(true);
    if (set_at && g_pc_on.load(std::memory_order_relaxed))  // ("skel passcmp": the set as drawn)
        pc_note(reinterpret_cast<uintptr_t>(rec[0x10]), pass, bucket, set_at, static_cast<int>(set_n / 2), nullptr, 1);
    // ([Holsters] InteractionHighlights=parts: the places lit on this gun, for its geometries' draws; made only for a gun's call,
    // the outer call's put back after it)
    std::optional<GlowCtx> glow_c;
    const GlowCtx* const glow_outer = t_glow;
    t_glow = nullptr;
    if (gl_role >= 0) {
        glow_c.emplace();
        // (a rigid record's draws: no matrix set to check them by, gl_rows)
        if (glow_ctx(gl_role, gl_weapon, gl_rows ? 0 : reinterpret_cast<uintptr_t>(rec[0x12]), gl_m, gl_rec, gl_po, gl_rows, &*glow_c)) t_glow = &*glow_c;
    }
    o_DrawVisEntity(ctx, rec, pass, bucket, a5, a6, a7);
    t_glow = glow_outer;
    if (cj.kind && dual::copy_model() >= 0 && held_prop::shown(1)) {  // run 5: the copy's model as a prop is drawn there instead
        cj.kind = 0;
        g_copy_own_skips.fetch_add(1, std::memory_order_relaxed);
    }
    if (cj.kind) {  // the copy: the same draw moved onto the free hand (restored with the rest below)
        {
            std::lock_guard lock(g_draw_mutex);
            float pre[3] = {};
            copy_root(cj, pre);
            apply_copy(cj);
            // [Hands] GunKick (2026-10-27): the copy is the gun in hand's draw moved, so it carries that gun's kick: undone (cj.kl)
            // and the copy's own spring's put on (its own shots).
            // Each half its own: the current half undoes the gun in hand's current kick (cj.kl), the previous half its previous
            // (cj.klp, last frame's), each then the copy's own of that frame (the review: one undo on both halves left the
            // copy's motion vectors with the gun in hand's kick in reverse)
            float copy_kl[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            // (cycle r179: kind 2, a set under its record's matrix, too: apply_copy put Tw into its local matrices, the kick goes
            // the same way; the gun in hand's own such draw carries its kick in them, cj.kl, undone here as for the others)
            if (g_frame.kick_any && cj.kind >= 1 && cj.kind <= 3) {
                float* km = cj.kind == 1 ? cj.rc : cj.sm;
                const int kc = cj.kind == 1 ? 0 : static_cast<int>(cj.count);
                kick_draw(1, g_frame.kick_j[1], km, kc, copy_kl, nullptr, cj.kl, cj.klp);
                if (cj.kind == 2) g_kick_recm[1].fetch_add(1, std::memory_order_relaxed);
                kick_note(1, g_frame.kick_j[1], km, kc, cj.kind == 3 ? reinterpret_cast<const float*>(cj.mset + 0x10) : nullptr,
                          cj.kind == 3 ? reinterpret_cast<const float*>(cj.mset + 0x20) : nullptr, cj.kind == 2 ? cj.rc : nullptr);
            }
            g_copy_where.kind = cj.kind;
            g_copy_where.pass = t_draw_pass;
            std::memcpy(g_copy_where.pre, pre, sizeof(pre));
            copy_root(cj, g_copy_where.post);
            ++g_copy_where.n;
            if (cj.kind == 3 && g_follow_rec.load(std::memory_order_relaxed)) {  // run 6 item 1, "skel follow": the copy
                const float* po = reinterpret_cast<const float*>(cj.mset + 0x10);
                float m[12];
                kick_unkick(cj.sm, copy_kl, m);  // ([Hands] GunKick: the copy as held, not its kick)
                for (int k = 0; k < 3; ++k) m[k * 4 + 3] += po[k];
                const HeldSample* cp = nullptr;
                for (const HeldSample& c : g_frame.copy)
                    if (c.valid && c.placed && (!cp || c.build > cp->build)) cp = &c;
                follow_obj(2, cp ? cp->john : -1, m);
            }
        }
        g_copy_draws.fetch_add(1, std::memory_order_relaxed);
        g_copy_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
        if (g_corr_frame != g_body_frame) g_copy_stale_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
        // ([Holsters] InteractionHighlights=parts) the copy: the gun in hand's draw moved, so its model and the copy's frame now
        t_glow = nullptr;
        if (holster::interaction_highlights() == 3) {
            if (!glow_c) glow_c.emplace();
            if (glow_ctx(1, holster::gun_in_hand(), cj.kind == 1 ? 0 : reinterpret_cast<uintptr_t>(rec[0x12]), cj.kind == 1 ? nullptr : cj.sm, cj.kind == 2 ? cj.rc : nullptr,
                         cj.kind == 3 ? reinterpret_cast<const float*>(cj.mset + 0x10) : nullptr, cj.kind == 1 ? cj.rc : nullptr, &*glow_c))
                t_glow = &*glow_c;
        }
        o_DrawVisEntity(ctx, rec, pass, bucket, a5, a6, a7);
        t_glow = glow_outer;
    }
    if (own) main_inst::own_mark(false);
    t_cull_show = 0;
    wind_release(wind);
    if (set_at) {
        std::memcpy(set_at, save.data(), set_n * sizeof(float));
        const float* e = set_at;  // (the player's: the edit over, its matrices the game's again)
        g_pset_edit.compare_exchange_strong(e, nullptr, std::memory_order_acq_rel);
    }
    if (set_at) {  // (the set is the game's again: no rigid copy for a later draw of it outside this call; a call nested in
        t_pc_s = pc_s_in;  // the player's puts back the outer call's, the review)
        t_pc_frame = pc_frame_in;
    }
    if (rec_at) std::memcpy(rec_at, rec_save, sizeof(rec_save));
    if (leave_at) std::memcpy(leave_at, leave_orig, sizeof(leave_orig));  // (round 78) the game's own matrices again (after rec_at's)
}

void dump_bones(const std::string& file, std::string* out) {
    uintptr_t skel = player_skeleton(), data = 0, bones = 0;
    uint16_t count = 0;
    if (!skel || !rd(skel + 8, &data) || !data || !rd(data, &bones) || !rd(data + 0x30, &count)) {
        *out = "ERROR no skeleton";
        return;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, file.c_str(), "w") != 0 || !f) {
        *out = "ERROR cannot write " + file;
        return;
    }
    for (int i = 0; i < count; ++i) {
        uintptr_t name = 0, parent = 0;
        char s[40] = {};
        rd(bones + i * 0x110, &name);
        rd(bones + i * 0x110 + 0x20, &parent);
        for (int k = 0; k < 39 && name; ++k)
            if (!rd(name + k, &s[k]) || !s[k]) break;
        int pi = parent ? static_cast<int>((parent - bones) / 0x110) : -1;
        std::fprintf(f, "%d %s %d\n", i, s, pi);
    }
    std::fclose(f);
    *out = std::to_string(count) + " bones to " + file;
}

}  // namespace

// ---- [Holsters] InteractionHighlights=parts (run 29 item 2): the glow on the gun's surface, its switches and its status
bool part_glow_live(int id) {
    return id >= 0 && id < holster::kHlCount && log::now_ms() - g_glow_live_ms[id].load(std::memory_order_relaxed) < 100.0;
}
void glow_force_off(bool off) { g_glow_off.store(off, std::memory_order_relaxed); }
void glow_passes(uint32_t mask) { g_glow_passes.store(mask & 0xffu, std::memory_order_relaxed); }
void glow_reset() {
    for (auto* a : {&g_glow_ctx, &g_glow_rec, &g_glow_nostream, &g_glow_onfail, &g_glow_offfail, &g_glow_empty, &g_glow_nogeom, &g_glow_recmis,
                    &g_glow_nopacket, &g_glow_sel_built, &g_glow_sel_over, &g_glow_otherset, &g_glow_proofs, &g_glow_proof_refused})
        a->store(0, std::memory_order_relaxed);
    for (auto& a : g_glow_by_pass) a.store(0, std::memory_order_relaxed);
    std::lock_guard lock(g_glow_note_mx);
    for (GlowNote& n : g_glow_note) n = GlowNote{};
}
std::string glow_status() {
    char b[600];
    std::snprintf(b, sizeof(b),
                  "recorded %llu (gun calls %llu) by pass %llu %llu %llu %llu, forced off %d, passes %#x | not recorded: no stream %llu, on marker %llu, "
                  "no triangles %llu, no geometry %llu, another set %llu; off marker failed %llu, record mismatched %llu, no packet %llu | selections built "
                  "%llu, over the frame's budget %llu; buffers proven %llu, refused %llu",
                  static_cast<unsigned long long>(g_glow_rec.load()), static_cast<unsigned long long>(g_glow_ctx.load()),
                  static_cast<unsigned long long>(g_glow_by_pass[0].load()), static_cast<unsigned long long>(g_glow_by_pass[1].load()),
                  static_cast<unsigned long long>(g_glow_by_pass[2].load()), static_cast<unsigned long long>(g_glow_by_pass[3].load()),
                  g_glow_off.load() ? 1 : 0, g_glow_passes.load(), static_cast<unsigned long long>(g_glow_nostream.load()),
                  static_cast<unsigned long long>(g_glow_onfail.load()), static_cast<unsigned long long>(g_glow_empty.load()),
                  static_cast<unsigned long long>(g_glow_nogeom.load()), static_cast<unsigned long long>(g_glow_otherset.load()),
                  static_cast<unsigned long long>(g_glow_offfail.load()),
                  static_cast<unsigned long long>(g_glow_recmis.load()), static_cast<unsigned long long>(g_glow_nopacket.load()),
                  static_cast<unsigned long long>(g_glow_sel_built.load()), static_cast<unsigned long long>(g_glow_sel_over.load()),
                  static_cast<unsigned long long>(g_glow_proofs.load()), static_cast<unsigned long long>(g_glow_proof_refused.load()));
    std::string r = b;
    GlowNote notes[holster::kHlCount];
    {
        std::lock_guard lock(g_glow_note_mx);
        for (int i = 0; i < holster::kHlCount; ++i) notes[i] = g_glow_note[i];
    }
    holster::Highlight hl[holster::kHlCount];
    float cam[16] = {};
    const bool cam_ok = zone_rings::highlights_shown(hl, cam);
    const double now = log::now_ms();
    static const char* const kKind[3] = {"none", "bones", "point"};
    for (int i = 0; i < holster::kHlCount; ++i) {
        const GlowNote& n = notes[i];
        if (!n.rec && !n.why && !n.all) continue;
        char e[420];
        int k = std::snprintf(e, sizeof(e), " | place %s gun %d weapon %d rec %llu age %.0f ms by %s tris %u of %u runs %u bones %#llx why %u shown %.2f",
                              holster::highlight_name(i), n.role, n.weapon, static_cast<unsigned long long>(n.rec), n.ms > 0.0 ? now - n.ms : -1.0,
                              kKind[n.kind < 3 ? n.kind : 0], n.tris, n.all, n.nruns, static_cast<unsigned long long>(n.bones), n.why, n.shown);
        if (n.box && k > 0 && static_cast<size_t>(k) < sizeof(e)) {
            k += std::snprintf(e + k, sizeof(e) - k, " box (%.3f %.3f %.3f)-(%.3f %.3f %.3f)", n.lo[0], n.lo[1], n.lo[2], n.hi[0], n.hi[1], n.hi[2]);
            // where each eye of the last frame sees the selection's box (its eight corners' bounds, 0-1 across the view): the
            // tests' mask of the part's own outline
            for (int eye = 0; eye < 2 && cam_ok && k > 0 && static_cast<size_t>(k) < sizeof(e); ++eye) {
                float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
                int seen = 0;
                for (int c = 0; c < 8; ++c) {
                    float uv[2];
                    if (!zone_rings::project_world(cam, n.corner[c], eye, uv)) continue;
                    ++seen;
                    for (int a = 0; a < 2; ++a) lo[a] = std::fmin(lo[a], uv[a]), hi[a] = std::fmax(hi[a], uv[a]);
                }
                if (seen == 8) k += std::snprintf(e + k, sizeof(e) - k, " %s uv (%.4f %.4f)-(%.4f %.4f)", eye ? "R" : "L", lo[0], lo[1], hi[0], hi[1]);
                else k += std::snprintf(e + k, sizeof(e) - k, " %s uv none", eye ? "R" : "L");
            }
            // (round 52; after the uv items, which r61 parses right after the box) its vertex furthest along +x
            if (k > 0 && static_cast<size_t>(k) < sizeof(e))
                k += std::snprintf(e + k, sizeof(e) - k, " far (%.4f %.4f %.4f)", n.outer[0], n.outer[1], n.outer[2]);
        }
        r += e;
    }
    return r;
}

bool gun_part_point(int weapon, int driver, float out[3]) {
    for (const GunModel& m : kGunModels) {
        if (m.weapon != weapon) continue;
        for (int i = 0; i < m.n && i < 4; ++i) {
            const GunPart& gp = m.part[i];
            if (gp.drv != driver || gp.deg == 0.0f) continue;  // (a slide alone has no hinge point)
            for (int k = 0; k < 3; ++k) out[k] = gp.pivot[k] + gp.t0[k];
            return true;
        }
    }
    return false;
}
const char* piece_key(int i) {
    static const char* const k[kPieces] = {"Holster", "Sheath", "Satchel", "Lasso", "Sling"};
    return i >= 0 && i < kPieces ? k[i] : "?";
}
Piece piece(int i) {
    std::lock_guard lock(g_draw_mutex);
    return i >= 0 && i < kPieces ? g_piece[i] : Piece{};
}
void set_piece(int i, const Piece& p, bool save) {
    if (i < 0 || i >= kPieces) return;
    Piece was;
    {
        std::lock_guard lock(g_draw_mutex);
        was = g_piece[i];
        g_piece[i] = p;
    }
    if (!save) return;
    char v[64];
    const std::string k = std::string("OwnBelt") + piece_key(i);
    if (std::memcmp(was.d, p.d, sizeof(p.d))) {
        std::snprintf(v, sizeof(v), "%.3f %.3f %.3f", p.d[0], p.d[1], p.d[2]);
        config::set("Holsters", (k + "Offset").c_str(), v);
    }
    if (std::memcmp(was.turn, p.turn, sizeof(p.turn))) {
        std::snprintf(v, sizeof(v), "%.1f %.1f %.1f", p.turn[0], p.turn[1], p.turn[2]);
        config::set("Holsters", (k + "Turn").c_str(), v);
    }
}
bool own_pieces() { return g_own_pieces.load(); }
bool rigid_pieces() { return g_rigid_pieces.load(); }
void set_rigid_pieces(bool on, bool save) {
    if (g_rigid_pieces.exchange(on) != on) log::info("[body] the moved pieces drawn %s", on ? "whole (OwnBeltRigid)" : "on their bones (stretching)");
    if (save) config::set("Holsters", "OwnBeltRigid", on ? "1" : "0");
}
void save_piece(int i) {
    if (i < 0 || i >= kPieces) return;
    const Piece p = piece(i);
    char v[64];
    const std::string k = std::string("OwnBelt") + piece_key(i);
    std::snprintf(v, sizeof(v), "%.3f %.3f %.3f", p.d[0], p.d[1], p.d[2]);
    config::set("Holsters", (k + "Offset").c_str(), v);
    std::snprintf(v, sizeof(v), "%.1f %.1f %.1f", p.turn[0], p.turn[1], p.turn[2]);
    config::set("Holsters", (k + "Turn").c_str(), v);
}
bool left_holster() { return lh_active(); }
// [Body] ArmMotionVectors (run 12 item 9; the menu's Advanced tab since 2026-10-13)
bool gun_steady() { return g_gun_steady.load(std::memory_order_relaxed); }
void set_gun_steady(bool on, bool save) {
    if (g_gun_steady.exchange(on) != on) log::info("[body] the gun held still against the float's steps (GunSteady): %s", on ? "on" : "off");
    if (save) config::set("Hands", "GunSteady", on ? "1" : "0");
}
bool no_flutter() { return g_no_flutter_cfg.load(std::memory_order_relaxed); }
void set_no_flutter(bool on, bool save) {
    if (g_no_flutter_cfg.exchange(on) != on) log::info("[body] no cloth flutter on John's draws (NoFlutter): %s", on ? "on" : "off");
    if (save) config::set("Body", "NoFlutter", on ? "1" : "0");
}
bool arm_motion_vectors() { return g_arm_mv.load(std::memory_order_relaxed); }
void set_arm_motion_vectors(bool on, bool save) {
    if (g_arm_mv.exchange(on) != on) log::info("[body] the arms' motion vectors %s", on ? "from their last drawn place (ArmMotionVectors)" : "the game's");
    if (save) config::set("Body", "ArmMotionVectors", on ? "1" : "0");
}
void set_left_holster(bool on, bool save) {
    if (g_left_holster.exchange(on) != on) log::info("[body] a mirrored holster at the left hip %s", on ? "drawn (OwnBeltLeftHolster)" : "off");
    if (save) config::set("Holsters", "OwnBeltLeftHolster", on ? "1" : "0");
}
Piece left_holster_piece() {
    std::lock_guard lock(g_draw_mutex);
    return g_lh_piece;
}
void set_left_holster_piece(const Piece& p, bool save) {
    Piece was;
    {
        std::lock_guard lock(g_draw_mutex);
        was = g_lh_piece;
        g_lh_piece = p;
    }
    if (!save) return;
    char v[64];
    if (std::memcmp(was.d, p.d, sizeof(p.d))) {
        std::snprintf(v, sizeof(v), "%.3f %.3f %.3f", p.d[0], p.d[1], p.d[2]);
        config::set("Holsters", "OwnBeltLeftHolsterOffset", v);
    }
    if (std::memcmp(was.turn, p.turn, sizeof(p.turn))) {
        std::snprintf(v, sizeof(v), "%.1f %.1f %.1f", p.turn[0], p.turn[1], p.turn[2]);
        config::set("Holsters", "OwnBeltLeftHolsterTurn", v);
    }
}
void save_left_holster_piece() {
    const Piece p = left_holster_piece();
    char v[64];
    std::snprintf(v, sizeof(v), "%.3f %.3f %.3f", p.d[0], p.d[1], p.d[2]);
    config::set("Holsters", "OwnBeltLeftHolsterOffset", v);
    std::snprintf(v, sizeof(v), "%.1f %.1f %.1f", p.turn[0], p.turn[1], p.turn[2]);
    config::set("Holsters", "OwnBeltLeftHolsterTurn", v);
}
bool left_holster_place(const float right[3], float out[3]) {
    std::lock_guard lock(g_draw_mutex);
    if (!lh_active() || g_body_frame - g_lh_frame > 30) return false;
    const float* n = g_lh_plane[0];
    const float* c = g_lh_plane[1];
    const float s = 2.0f * ((right[0] - c[0]) * n[0] + (right[1] - c[1]) * n[1] + (right[2] - c[2]) * n[2]);
    for (int k = 0; k < 3; ++k) out[k] = right[k] - s * n[k] + g_lh_delta[k];
    return true;
}
bool left_holster_delta(float d[3]) {
    std::lock_guard lock(g_draw_mutex);
    if (!lh_active() || g_body_frame - g_lh_frame > 30) return false;
    std::memcpy(d, g_lh_delta, sizeof(g_lh_delta));
    return true;
}
// the holster geometry among the player's recent drawables: a fifth or more on the pistol bone and half on the pelvis
// (run 11 item 3's rule); its bones (any weight) and the pistol bone's own cluster (bind)
void left_holster_scan() {
    const int pb = g_rig.piece[0];
    if (pb <= 0) return;
    int pelvis = g_rig.pelvis;
    const uint64_t frame_now = g_body_frame;
    for (int si = 0; si < 8; ++si) {
        const uintptr_t d = g_player_drawables[si].load();
        if (!d || frame_now - g_player_drawn_at[si].load() > 2) continue;  // (drawn now: a stale one may be freed)
        uintptr_t list = 0, models = 0;
        uint16_t nm = 0;
        if (!rd(d + 0x20 + 0x30, &list) || !list || !rd(list, &models) || !rd(list + 8, &nm)) continue;
        for (int mi = 0; mi < nm && mi < 40; ++mi) {
            uintptr_t m = 0, geoms = 0;
            uint16_t ng = 0;
            if (!rd(models + mi * 8, &m) || !m || !rd(m + 0x2e, &ng) || !rd(m + 8, &geoms) || !geoms) continue;
            for (int gi = 0; gi < ng && gi < 48; ++gi) {
                uintptr_t g = 0;
                if (!rd(geoms + gi * 8, &g) || !g) continue;
                GeomScan* gs = new GeomScan;
                if (scan_geom_checked(g, gs)) {
                    float total = 0;
                    for (int bi = 0; bi < 256; ++bi) total += gs->w[bi];
                    const float fa = total > 0 ? gs->w[pb] / total : 0, fp = total > 0 && pelvis >= 0 ? gs->w[pelvis] / total : 0;
                    if (fa >= 0.2f && fp >= 0.5f && gs->nmain[pb]) {
                        g_lh.model.store(0, std::memory_order_release);
                        int nb = 0;
                        for (int bi = 0; bi < 256 && nb < 16; ++bi)
                            if (gs->w[bi] > 0.0f) g_lh.bones[nb++] = static_cast<uint8_t>(bi);
                        g_lh.nb = nb;
                        g_lh.cx = 0.5f * (gs->mlo[pb][0] + gs->mhi[pb][0]);
                        g_lh.top[0] = g_lh.cx;
                        g_lh.top[1] = gs->mhi[pb][1];
                        g_lh.top[2] = 0.5f * (gs->mlo[pb][2] + gs->mhi[pb][2]);
                        g_lh.gi.store(static_cast<uint32_t>(gi), std::memory_order_relaxed);
                        g_lh.model.store(m, std::memory_order_release);
                        g_lh_scans.fetch_add(1, std::memory_order_relaxed);
                        log::info("[body] the left holster's copy: model %d geometry %d (%u vertices, %d bones; the holster's plane x %.3f, its top "
                                  "(%.3f %.3f %.3f))", mi, gi, gs->vcount, nb, g_lh.cx, g_lh.top[0], g_lh.top[1], g_lh.top[2]);
                        delete gs;
                        return;
                    }
                }
                delete gs;
            }
        }
    }
}
void left_holster_refresh() {
    static uintptr_t last = 0;
    static double last_ms = -1e9;
    uintptr_t fp = 0;
    for (int si = 0; si < 8; ++si)
        if (g_body_frame - g_player_drawn_at[si].load(std::memory_order_relaxed) <= 120) fp = fp * 31 + g_player_drawables[si].load(std::memory_order_relaxed);
    const double now = log::now_ms();
    if (!fp || (fp == last && g_lh.model.load() != 0) || now - last_ms < 1000.0) return;
    last = fp;
    last_ms = now;
    left_holster_scan();
}
// [Holsters] OwnBeltRigid: each piece's geometries among the player's recent drawables (four or more vertices whose
// heaviest weight is on the piece's bone) and the bones every vertex of them uses (up to 48; more: not drawn rigid)
void pieces_geom_scan() {
    g_pgeom_n.store(0, std::memory_order_release);
    int n = 0;
    const uint64_t frame_now = g_body_frame;
    for (int si = 0; si < 8 && n < 8; ++si) {
        const uintptr_t d = g_player_drawables[si].load();
        if (!d || frame_now - g_player_drawn_at[si].load() > 2) continue;  // (drawn now: a stale one may be freed)
        uintptr_t list = 0, models = 0;
        uint16_t nm = 0;
        if (!rd(d + 0x20 + 0x30, &list) || !list || !rd(list, &models) || !rd(list + 8, &nm)) continue;
        for (int mi = 0; mi < nm && mi < 40 && n < 8; ++mi) {
            uintptr_t m = 0, geoms = 0;
            uint16_t ng = 0;
            if (!rd(models + mi * 8, &m) || !m || !rd(m + 0x2e, &ng) || !rd(m + 8, &geoms) || !geoms) continue;
            for (int gi = 0; gi < ng && gi < 48 && n < 8; ++gi) {
                uintptr_t g = 0;
                if (!rd(geoms + gi * 8, &g) || !g) continue;
                GeomScan* gs = new GeomScan;
                if (scan_geom_checked(g, gs)) {
                    for (int pc = 0; pc < body::kPieces && n < 8; ++pc) {
                        const int pb = g_rig.piece[pc];
                        if (pb <= 0 || pb >= 256 || gs->nmain[pb] < 4) continue;
                        int nb = 0;
                        bool fits = true;
                        uint8_t bones[48];
                        for (int bi = 0; bi < 256; ++bi)
                            if (gs->w[bi] > 0.0f) {
                                if (nb == 48) {
                                    fits = false;
                                    break;
                                }
                                bones[nb++] = static_cast<uint8_t>(bi);
                            }
                        if (!fits) {
                            log::warn("[body] the %s's geometry (model %d geometry %d) uses more than 48 bones: not drawn rigid", body::piece_key(pc), mi, gi);
                            continue;
                        }
                        PieceGeom& e = g_pgeom[n++];
                        e.model.store(0, std::memory_order_release);
                        e.piece = pc;
                        e.nb = nb;
                        std::memcpy(e.bones, bones, sizeof(bones));
                        e.gi.store(static_cast<uint32_t>(gi), std::memory_order_relaxed);
                        e.model.store(m, std::memory_order_release);
                        log::info("[body] the %s drawn rigid: model %d geometry %d (%u of its vertices on it, %d bones)", body::piece_key(pc), mi, gi,
                                  static_cast<unsigned>(gs->nmain[pb]), nb);
                    }
                }
                delete gs;
            }
        }
    }
    g_pgeom_n.store(n, std::memory_order_release);
    g_pgeom_scans.fetch_add(1, std::memory_order_relaxed);
}
void pieces_geom_refresh() {
    static uintptr_t last = 0;
    static double last_ms = -1e9;
    uintptr_t fp = 0;
    for (int si = 0; si < 8; ++si)
        if (g_body_frame - g_player_drawn_at[si].load(std::memory_order_relaxed) <= 120) fp = fp * 31 + g_player_drawables[si].load(std::memory_order_relaxed);
    const double now = log::now_ms();
    if (!fp || (fp == last && g_pgeom_scans.load() != 0) || now - last_ms < 1000.0) return;
    last = fp;
    last_ms = now;
    pieces_geom_scan();
}
std::string left_holster_status() {
    char b[320];
    float d[3] = {};
    const bool ok = left_holster_delta(d);
    float j[3], tp[2][3];
    {
        std::lock_guard lock(g_draw_mutex);
        std::memcpy(j, g_lh_joint, sizeof(j));
        std::memcpy(tp, g_lh_tops, sizeof(tp));
    }
    std::snprintf(b, sizeof(b), "left holster %d mode %d: geometry %s (gi %u, %d bones), scans %llu, copies drawn %llu, its move (%.4f %.4f %.4f), joint (%.3f %.3f %.3f)%s",
                  g_left_holster.load() ? 1 : 0, g_lh_mode.load(), g_lh.model.load() ? "found" : "none", g_lh.gi.load(), g_lh.nb,
                  static_cast<unsigned long long>(g_lh_scans.load()), static_cast<unsigned long long>(g_lh_draws.load()), d[0], d[1], d[2], j[0], j[1],
                  j[2], ok ? "" : " (not drawn lately)");
    char b2[160];
    std::snprintf(b2, sizeof(b2), ", tops: the right (%.3f %.3f %.3f) the copy (%.3f %.3f %.3f)", tp[0][0], tp[0][1], tp[0][2], tp[1][0], tp[1][1], tp[1][2]);
    return std::string(b) + b2;
}
bool piece_delta(int i, float dlt[3]) {
    if (i < 0 || i >= kPieces) return false;
    std::lock_guard lock(g_draw_mutex);
    const PieceDiag& d = g_piece_diag[i];
    if (!d.ok || g_body_frame - d.frame > 30) return false;
    for (int k = 0; k < 3; ++k) dlt[k] = d.want[k] - d.piv[k];
    return true;
}
bool piece_where(int i, float joint[3]) {
    if (i < 0 || i >= kPieces) return false;
    std::lock_guard lock(g_draw_mutex);
    const PieceDiag& d = g_piece_diag[i];
    if (!d.ok || g_body_frame - d.frame > 30) return false;
    std::memcpy(joint, d.got, sizeof(d.got));
    return true;
}
// 2026-10-13 (the user: "Two options should be selectable. One for the visual holster and the holster ring to always be
// at the same spot. One for them to be moved seperately"): a piece's move this frame as the rigid one it is (world): its
// joint before (piv) and after (want), its turn Rw about it: a point p on it is at want + Rw (p - piv)
bool piece_rigid(int i, float piv[3], float want[3], float Rw[9]) {
    if (i < 0 || i >= kPieces) return false;
    std::lock_guard lock(g_draw_mutex);
    const PieceDiag& d = g_piece_diag[i];
    if (!d.ok || g_body_frame - d.frame > 30) return false;
    float tw[3];
    piece_world(g_piece[i], d.R, Rw, tw);
    std::memcpy(piv, d.piv, sizeof(d.piv));
    std::memcpy(want, d.want, sizeof(d.want));
    return true;
}
bool left_holster_rigid(float piv[3], float want[3], float Rw[9]) {
    std::lock_guard lock(g_draw_mutex);
    if (!lh_active() || g_body_frame - g_lh_frame > 30) return false;
    float tw[3];
    piece_world(g_lh_piece, g_lh_R, Rw, tw);
    std::memcpy(piv, g_lh_piv, sizeof(g_lh_piv));
    std::memcpy(want, g_lh_joint, sizeof(g_lh_joint));
    return true;
}
// 2026-10-13 (the user: "they hold their position relative to eachother. Moving one should move the other the same
// way"): a piece's offset change as a world move and back, in the pelvis's frame of the last player set (every piece's
// and the left holster's offsets are in it: piece_world's tw = R (d.x, d.y, -d.z))
bool pelvis_frame(float R[9]) {
    std::lock_guard lock(g_draw_mutex);
    for (const PieceDiag& d : g_piece_diag)
        if (d.ok && g_body_frame - d.frame <= 30) {
            std::memcpy(R, d.R, sizeof(d.R));
            return true;
        }
    // (the belt off, or no piece drawn lately: the last player set's pelvis, however old: a reset in a cutscene or a
    // menu still moves the rings with the pieces, by the frame John last stood in)
    if (!g_pelvis_R_ok) return false;
    std::memcpy(R, g_pelvis_R, sizeof(g_pelvis_R));
    return true;
}
bool piece_shift_world(const Piece& from, const Piece& to, float dw[3]) {
    float R[9];
    if (!pelvis_frame(R)) return false;
    const float db[3] = {to.d[0] - from.d[0], to.d[1] - from.d[1], -(to.d[2] - from.d[2])};
    for (int r = 0; r < 3; ++r) dw[r] = R[r * 3] * db[0] + R[r * 3 + 1] * db[1] + R[r * 3 + 2] * db[2];
    return true;
}
bool piece_shifted(const Piece& from, const float dw[3], Piece* to) {
    float R[9];
    if (!to || !pelvis_frame(R)) return false;
    float db[3];
    for (int r = 0; r < 3; ++r) db[r] = R[r] * dw[0] + R[3 + r] * dw[1] + R[6 + r] * dw[2];  // R^T dw
    *to = from;
    to->d[0] = std::fmin(0.6f, std::fmax(-0.6f, from.d[0] + db[0]));
    to->d[1] = std::fmin(0.6f, std::fmax(-0.6f, from.d[1] + db[1]));
    to->d[2] = std::fmin(0.6f, std::fmax(-0.6f, from.d[2] - db[2]));
    return true;
}
bool left_holster_move(float tw[3]) {
    std::lock_guard lock(g_draw_mutex);
    const PieceDiag& d = g_piece_diag[0];  // (the pelvis's frame of the last player set)
    if (!lh_active() || !d.ok || g_body_frame - d.frame > 30) return false;
    float Rw[9];
    piece_world(g_lh_piece, d.R, Rw, tw);
    return true;
}
bool grab_from(const float piv[3], const float R[9], const float Rd[9], const float h0[3], const float h1[3], const Piece& p0, Piece* out);
bool piece_grab(int i, const float Rd[9], const float h0[3], const float h1[3], const Piece& p0, Piece* out) {
    if (i < 0 || i >= kPieces || !out) return false;
    float piv[3], R[9];
    {
        std::lock_guard lock(g_draw_mutex);
        const PieceDiag& d = g_piece_diag[i];
        if (!d.ok || g_body_frame - d.frame > 30) return false;
        std::memcpy(piv, d.piv, sizeof(piv));
        std::memcpy(R, d.R, sizeof(R));
    }
    return grab_from(piv, R, Rd, h0, h1, p0, out);
}
// 2026-10-13 (the user: "The left sidearm holster is unmovable by controls or move by hand option"): the mirrored copy
// taken by hand as a piece is: its joint (moved) to grip near, its joint before its own move and the pelvis's frame
bool left_holster_where(float j[3]) {
    std::lock_guard lock(g_draw_mutex);
    if (!lh_active() || g_body_frame - g_lh_frame > 30) return false;
    std::memcpy(j, g_lh_joint, sizeof(g_lh_joint));
    return true;
}
bool left_holster_grab(const float Rd[9], const float h0[3], const float h1[3], const Piece& p0, Piece* out) {
    float piv[3], R[9];
    {
        std::lock_guard lock(g_draw_mutex);
        if (!lh_active() || g_body_frame - g_lh_frame > 30) return false;
        std::memcpy(piv, g_lh_piv, sizeof(piv));
        std::memcpy(R, g_lh_R, sizeof(R));
    }
    return grab_from(piv, R, Rd, h0, h1, p0, out);
}
bool grab_from(const float piv[3], const float R[9], const float Rd[9], const float h0[3], const float h1[3], const Piece& p0, Piece* out) {
    if (!out) return false;
    float Rw0[9], tw0[3], Rw[9], Rl[9], t[9], Rt[9];
    piece_world(p0, R, Rw0, tw0);
    // the hand's move since the grip (Rd about the hand, from h0 to h1) put on the piece's joint and turn
    float j0[3], jn[3];
    for (int k = 0; k < 3; ++k) j0[k] = piv[k] + tw0[k] - h0[k];
    for (int r = 0; r < 3; ++r) jn[r] = Rd[r * 3] * j0[0] + Rd[r * 3 + 1] * j0[1] + Rd[r * 3 + 2] * j0[2] + h1[r];
    mul3(Rd, Rw0, Rw);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) Rt[r * 3 + c] = R[c * 3 + r];
    mul3(Rt, Rw, t);
    mul3(t, R, Rl);  // in the bind frame: Ry(turn) Rx(tilt) Rz(lean)
    const float sa = std::fmax(-1.0f, std::fmin(1.0f, -Rl[5]));
    const float a = std::asin(sa), b = std::atan2(Rl[2], Rl[8]), c = std::atan2(Rl[3], Rl[4]);
    float tw[3], db[3];
    for (int k = 0; k < 3; ++k) tw[k] = jn[k] - piv[k];
    for (int r = 0; r < 3; ++r) db[r] = R[r] * tw[0] + R[3 + r] * tw[1] + R[6 + r] * tw[2];  // R^T tw
    Piece p;
    p.d[0] = std::fmin(0.6f, std::fmax(-0.6f, db[0]));
    p.d[1] = std::fmin(0.6f, std::fmax(-0.6f, db[1]));
    p.d[2] = std::fmin(0.6f, std::fmax(-0.6f, -db[2]));
    p.turn[0] = a * 57.29578f;
    p.turn[1] = b * 57.29578f;
    p.turn[2] = c * 57.29578f;
    *out = p;
    return true;
}
void set_own_pieces(bool on, bool save) {
    if (g_own_pieces.exchange(on) != on) log::info("[body] John's own belt's pieces %s", on ? "moved as set (OwnBelt)" : "as the game draws them");
    if (save) config::set("Holsters", "OwnBelt", on ? "1" : "0");
}



bool hide_enabled() {
    std::lock_guard lock(g_cfg_mutex);
    return g_cfg_hide;
}
void set_hide_enabled(bool on) {
    std::lock_guard lock(g_cfg_mutex);
    g_cfg_hide = on;
    log::info("[body] head and hat hide switch %s", on ? "on" : "off");
}
int stance() {
    std::lock_guard lock(g_cfg_mutex);
    return g_cfg_stance;
}
void set_stance(int s) {
    std::lock_guard lock(g_cfg_mutex);
    g_cfg_stance = s < 0 || s > 3 ? 1 : s;
    log::info("[body] stance %s", g_cfg_stance == 0 ? "the game's" : g_cfg_stance == 1 ? "upper body upright" : g_cfg_stance == 2 ? "lean removed"
                                                                                                             : "still (only the arms and legs move)");
}
float body_back() {
    std::lock_guard lock(g_cfg_mutex);
    return g_cfg_back;
}
void set_body_back(float m) {
    std::lock_guard lock(g_cfg_mutex);
    g_cfg_back = m < 0 ? 0 : m > 0.5f ? 0.5f : m;
}
bool follows_head() {
    std::lock_guard lock(g_cfg_mutex);
    return g_cfg_follow;
}
void set_follows_head(bool on) {
    std::lock_guard lock(g_cfg_mutex);
    g_cfg_follow = on;
    log::info("[body] the body %s", on ? "moves with the headset" : "stays where the character is");
}

bool locks_facing() {
    std::lock_guard lock(g_cfg_mutex);
    return g_cfg_lock;
}
void set_locks_facing(bool on) {
    std::lock_guard lock(g_cfg_mutex);
    g_cfg_lock = on;
    log::info("[body] facing %s", on ? "locked to the camera's heading" : "the character's");
}
float torso_pitch() {
    std::lock_guard lock(g_cfg_mutex);
    return g_cfg_pitch;
}
void set_torso_pitch(float deg) {
    std::lock_guard lock(g_cfg_mutex);
    g_cfg_pitch = deg < -30 ? -30 : deg > 30 ? 30 : deg;
}

bool hand_correction(int h, float A[9], float a[3], bool* ik) {
    if (h < 0 || h > 1) return false;
    HandCorr c;
    if (!seq_copy(g_hand_seq, &c, &g_hand_corr[h], sizeof(c))) return false;
    if (!c.ms || log::now_ms() - c.ms > 250) return false;  // no player set drawn lately: not in first person
    std::memcpy(A, c.A, sizeof(c.A));
    std::memcpy(a, c.a, sizeof(c.a));
    if (ik) *ik = c.ik;
    return true;
}

bool item_correction(int h, float A[9], float a[3], bool* ik, double* ad) {
    if (h < 0 || h > 1) return false;
    HandCorr c;
    if (!seq_copy(g_hand_seq, &c, &g_item_corr[h], sizeof(c))) return false;
    if (!c.ms || log::now_ms() - c.ms > 250) return false;  // no player set drawn lately: not in first person
    std::memcpy(A, c.A, sizeof(c.A));
    std::memcpy(a, c.a, sizeof(c.a));
    if (ad) std::memcpy(ad, c.ad, sizeof(c.ad));
    if (ik) *ik = c.ik;
    return true;
}

bool item_gun(int h, float A[9], float a[3], bool* ik, float gun[16], uintptr_t* W, uint64_t* frame) {  // (item 1b, after optimumbox's fork, MIT)
    if (h < 0 || h > 1) return false;
    HandCorr c;
    if (!seq_copy(g_hand_seq, &c, &g_item_corr[h], sizeof(c))) return false;
    if (!c.ms || log::now_ms() - c.ms > 250 || !c.gun_ok) return false;  // (one copy: the correction and its build's matrix together)
    std::memcpy(A, c.A, sizeof(c.A));
    std::memcpy(a, c.a, sizeof(c.a));
    std::memcpy(gun, c.gun, sizeof(c.gun));
    if (ik) *ik = c.ik;
    if (W) *W = c.W;
    if (frame) *frame = c.frame;
    return true;
}
bool drawn_gun(float R[2][9], float o[2][3], uint64_t* frame, bool* older, double* age_ms) {  // (item 1b's check, read only)
    DrawnGun d;
    if (!seq_copy(g_dg_seq, &d, &g_dg, sizeof(d)) || !d.ok) return false;
    const double age = log::now_ms() - d.ms;
    if (age > 250.0) return false;
    std::memcpy(R, d.R, sizeof(d.R));
    std::memcpy(o, d.o, sizeof(d.o));
    if (frame) *frame = d.frame;
    if (older) *older = d.older;
    if (age_ms) *age_ms = age;
    return true;
}
void set_drawn_gun_capture(bool on) { g_dg_on.store(on, std::memory_order_relaxed); }
bool drawn_second(float R[9], float o[3], uint64_t* frame, bool* copy) {  // (round 38 check 2's check, read only)
    DrawnGun d;
    if (!seq_copy(g_dg2_seq, &d, &g_dg2, sizeof(d)) || !d.ok || log::now_ms() - d.ms > 250.0) return false;
    std::memcpy(R, d.R[0], sizeof(d.R[0]));
    std::memcpy(o, d.o[0], sizeof(d.o[0]));
    if (frame) *frame = d.frame;  // (the review: either may be left out)
    if (copy) *copy = d.copy;
    return true;
}
void set_drawn_second_capture(bool on) { g_dg2_on.store(on, std::memory_order_relaxed); }
void set_drawn_second_scope(bool on) { g_dg2_scope.store(on, std::memory_order_relaxed); }
void set_drawn_second_bolt(bool on) { g_dg2_bolt.store(on, std::memory_order_relaxed); }
bool second_drawn(SecondDrawn* out, double* age_ms) {  // (round 38 check 2)
    SecDrawnPub p;
    if (!out || !seq_copy(g_hand_seq, &p, &g_sec_pub, sizeof(p)) || !p.ok) return false;
    const double age = log::now_ms() - p.d.ms;
    if (age > 250.0) return false;
    *out = p.d;
    if (age_ms) *age_ms = age;
    return true;
}
bool secondary_placed(uintptr_t W, float m[16]) {  // (round 38 check 2: the second gun's shot map, aim.cpp)
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_sec_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        const Placed p = g_sec_placed;
        if (g_sec_seq.load(std::memory_order_acquire) != s0) continue;
        if (!W || p.W != W || p.carry || log::now_ms() - p.ms > 100.0) return false;
        std::memcpy(m, p.m, sizeof(p.m));
        return true;
    }
    return false;
}
void second_drawn_counts(uint64_t* pubs, uint64_t* tw) {
    *pubs = g_sec_drawn_pubs.load(std::memory_order_relaxed);
    *tw = g_sec_drawn_tw.load(std::memory_order_relaxed);
}
void held_match_counts(uint64_t* newest, uint64_t* older) {
    *newest = g_held_newest.load(std::memory_order_relaxed);
    *older = g_held_older.load(std::memory_order_relaxed);
}

bool gun_in_gun_hand() { return g_gun_hand_cfg.load(); }
void set_gun_in_gun_hand(bool on) {
    if (g_gun_hand_cfg.exchange(on) != on) log::info("[body] left-handed: the gun in John's left hand, each arm on its own controller %d", on ? 1 : 0);
    config::set("Hands", "GunInGunHand", on ? "1" : "0");
}
bool fixed_gun_grip() { return g_fixed_grip.load(std::memory_order_relaxed); }
float gun_kick() { return g_recoil.load(std::memory_order_relaxed); }
void set_gun_kick(float k, bool save) {
    k = std::isfinite(k) ? std::fmin(2.0f, std::fmax(0.0f, k)) : 0.0f;
    if (g_recoil.exchange(k) != k) log::info("[body] the guns' kick (GunKick): %.2f%s", k, save ? "" : " (the session)");
    if (save) {
        char b[16];
        std::snprintf(b, sizeof(b), "%.2f", k);
        config::set("Hands", "GunKick", b);
    }
}
bool gun_collision() { return g_coll_cfg.load(std::memory_order_relaxed); }
void set_gun_collision(bool on, bool save) {
    if (g_coll_cfg.exchange(on) != on) log::info("[body] the guns' collisions (GunCollision): %d%s", on ? 1 : 0, save ? "" : " (the session)");
    if (save) config::set("Hands", "GunCollision", on ? "1" : "0");
}
bool collision_guns(CollGuns* out) {
    if (!out) return false;
    std::lock_guard lock(g_coll_mutex);
    if (!g_coll_pub.ms || log::now_ms() - g_coll_pub.ms > 250.0) return false;
    *out = g_coll_pub;
    return true;
}
bool collision_sound(float at[3]) {
    std::lock_guard lock(g_coll_mutex);
    if (!g_coll_sound_ok) return false;
    g_coll_sound_ok = false;
    std::memcpy(at, g_coll_sound_at, sizeof(g_coll_sound_at));
    return true;
}
bool two_handed_steer() { return g_two_steer_cfg.load(std::memory_order_relaxed); }
void set_two_handed_steer(bool on) {
    if (g_two_steer_cfg.exchange(on) != on) log::info("[body] two-handed: the front hand %s", on ? "turns the gun" : "drawn on the gun only");
    config::set("Reload", "TwoHandedSteer", on ? "1" : "0");
}
bool fixed_sidearm_grip() { return g_fixed_sidearm.load(std::memory_order_relaxed); }
void set_fixed_sidearm_grip(bool on, bool save) {
    if (g_fixed_sidearm.exchange(on) != on) log::info("[body] FixedSidearmGrip: the sidearms held by their aiming hold %d", on ? 1 : 0);
    if (save) config::set("Hands", "FixedSidearmGrip", on ? "1" : "0");
}
void set_fixed_gun_grip(bool on, bool save) {
    if (g_fixed_grip.exchange(on) != on) log::info("[body] FixedGunGrip: the long guns held by their aiming hold %d", on ? 1 : 0);
    if (save) config::set("Hands", "FixedGunGrip", on ? "1" : "0");
}
int transplant_fingers() { return g_fingers_cfg.load(); }
void set_transplant_fingers(int mode) {
    g_fingers_cfg = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    config::set("Hands", "TransplantFingers", g_fingers_cfg.load() == 0 ? "off" : g_fingers_cfg.load() == 2 ? "open" : "swap");
}

bool placed_item(uintptr_t W, float m[16]) {
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_placed_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        Placed p = g_placed;
        if (g_placed_seq.load(std::memory_order_acquire) != s0) continue;
        if (!W || p.W != W || log::now_ms() - p.ms > 100.0) return false;
        std::memcpy(m, p.m, sizeof(p.m));
        return true;
    }
    return false;
}

// [Reload] CarriedBolt (actions.cpp): the carried gun's own frame (world; its axes as columns: x right, y up, z back) as
// the game update placed it at the front hand (place_carry), within 150 ms. Any thread (the second gun's seqlock).
float carry_ease_ms() { return g_carry_ease_ms.load(std::memory_order_relaxed); }
double carry_ease_end() { return g_carry_ease_end.load(std::memory_order_relaxed); }
bool steadied_fire_clip() { return (worked_flags() & kSteady) && in_fire_clip(); }
bool carry_frame(float R[9], float o[3], bool drawn) {
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_sec_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        const Placed p = g_sec_placed;
        if (g_sec_seq.load(std::memory_order_acquire) != s0) continue;
        if (!p.carry || p.john < 0 || log::now_ms() - p.ms > 150.0) return false;
        if (drawn) {  // (C37b) the carried gun as drawn, while drawn (within 50 ms)
            std::lock_guard lock(g_cmodel_mx);
            if (g_cmodel.ok && log::now_ms() - g_cmodel.ms < 50.0) {
                std::memcpy(R, g_cmodel.R, sizeof(g_cmodel.R));
                std::memcpy(o, g_cmodel.o, sizeof(g_cmodel.o));
                g_cmodel_uses.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        for (int j = 0; j < 3; ++j) {  // the rows' axes, made unit, as columns
            const float l = std::sqrt(p.m[j * 4] * p.m[j * 4] + p.m[j * 4 + 1] * p.m[j * 4 + 1] + p.m[j * 4 + 2] * p.m[j * 4 + 2]);
            if (!(l > 1e-6f) || !std::isfinite(l)) return false;
            for (int i = 0; i < 3; ++i) R[i * 3 + j] = p.m[j * 4 + i] / l;
        }
        for (int i = 0; i < 3; ++i) o[i] = p.m[12 + i];
        return std::isfinite(o[0]) && std::isfinite(o[1]) && std::isfinite(o[2]);
    }
    return false;
}

bool two_hand_P(uintptr_t W, float P[16], float hand0[3], float rot0[9], int fj) {
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_two_T_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        TwoHandT c = g_two_T;
        const TwoHandT cs = g_two_T_still;
        const bool moving = g_two_T_moving;
        if (g_two_T_seq.load(std::memory_order_acquire) != s0) continue;
        if (!W || c.W != W || log::now_ms() - c.ms > 100.0 || !c.hand0_ok || (fj >= 0 && c.fj != fj)) return false;  // (C16: the caller's front hand)
        // (round 55) let go while moving: the still capture of this gun, 3 s old at most, its fj the same
        // (the review: carried rigidly to the latest capture's front target here, so the hand-over's own use of P, the
        // take-back point on the gun, sees the gun at the hand of now)
        float Pm[16];
        if (moving && cs.W == W && cs.hand0_ok && cs.fj == c.fj && c.ms - cs.ms <= 3000.0 && carry_rigid(cs.P, cs.hand0, cs.rot0, c.hand0, c.rot0, Pm)) {
            std::memcpy(c.P, Pm, sizeof(c.P));
            g_two_T_still_used.fetch_add(1, std::memory_order_relaxed);
        }
        std::memcpy(P, c.P, sizeof(c.P));
        std::memcpy(hand0, c.hand0, sizeof(c.hand0));
        std::memcpy(rot0, c.rot0, sizeof(c.rot0));
        return true;
    }
    return false;
}
bool carry_old_test() { return g_carry_old.load(std::memory_order_relaxed); }
void put_away_leaving(int weapon) {
    const uintptr_t wmgr = g_held_wmgr.load(std::memory_order_relaxed);
    uintptr_t item = 0;
    if (!wmgr || !raw(wmgr + 0x80, &item, sizeof(item)) || !item) return;
    leave_end(4);  // (a put-away still under way: its line now)
    g_leave_at[0].store(g_leave_updates.load());
    g_leave_at[1].store(g_leave_hidden.load());
    g_leave_at[2].store(g_leave_near.load());
    g_leave_weapon.store(weapon, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_leave_mx);
        for (uint64_t& f : g_leave_m_frame) f = 0;
    }
    g_leave_inst.store(0, std::memory_order_relaxed);
    g_leave_probe_left.store(6, std::memory_order_relaxed);
    // (round 81) its model: the held item's drawable, drawn within 300 ms
    g_leave_drawable.store(log::now_ms() - g_held_drawable_ms.load(std::memory_order_relaxed) < 300.0 ? g_held_drawable.load(std::memory_order_relaxed) : 0,
                           std::memory_order_relaxed);
    g_leave_model_told.store(0, std::memory_order_relaxed);
    const double now = log::now_ms();
    g_leave_begun.store(now, std::memory_order_relaxed);
    g_leave_until.store(now + 1500.0, std::memory_order_relaxed);
    g_leave_item.store(item, std::memory_order_release);
    g_leave_puts.fetch_add(1, std::memory_order_relaxed);
}
void set_leave_subst(bool on) {
    if (g_leave_subst_on.exchange(on) != on) log::info("[holster] a leaving item drawn at its kept holstered place: %d (the test's)", on ? 1 : 0);
}
bool leaving_W(uintptr_t W) {
    // (2026-11-04, round 79 on the other PC: the torch's flame "flickers at the left of belt when put away": shown again at
    // the holstered torch as it ended) a second more after it is holstered
    if (W && W == g_leave_last_W.load(std::memory_order_relaxed) && log::now_ms() < g_leave_last_until.load(std::memory_order_relaxed)) return true;
    const uintptr_t li = leave_live();
    if (!li || !W) return false;
    uintptr_t iw = 0;
    return raw(li + 0xa0, &iw, sizeof(iw)) && iw == W;
}
std::string put_away_leaving_status() {
    char b[480];
    std::snprintf(b, sizeof(b),
                  "leaving: put-aways %llu, its placements kept %llu, draws hidden %llu, its draws missed %llu, ended holstered %llu in hand %llu timed out %llu "
                  "unread %llu, drawn at the holster %llu (from where it was hung %.0f mm, %llu checks; its travel %.0f mm), the lasso's rope away %llu (as its coil %llu; the coil kept %llu, far %llu); "
                  "its model hidden away from its way %llu; subst %d",
                  static_cast<unsigned long long>(g_leave_puts.load()), static_cast<unsigned long long>(g_leave_updates.load()),
                  static_cast<unsigned long long>(g_leave_hidden.load()), static_cast<unsigned long long>(g_leave_near.load()),
                  static_cast<unsigned long long>(g_leave_ends[0].load()),
                  static_cast<unsigned long long>(g_leave_ends[1].load()), static_cast<unsigned long long>(g_leave_ends[2].load()),
                  static_cast<unsigned long long>(g_leave_ends[3].load()), static_cast<unsigned long long>(g_leave_subst.load()),
                  static_cast<double>(g_subst_err_mm.load()), static_cast<unsigned long long>(g_subst_checks.load()), static_cast<double>(g_subst_travel_mm.load()),
                  static_cast<unsigned long long>(g_leave_rope.load()), static_cast<unsigned long long>(g_leave_rope_coil.load()),
                  static_cast<unsigned long long>(g_coil_kept.load()), static_cast<unsigned long long>(g_coil_far.load()),
                  static_cast<unsigned long long>(g_leave_model_hidden.load()), g_leave_subst_on.load() ? 1 : 0);
    return b;
}
// "skel carry trace <ms>" (tests/sim/r32_carry_move.py): for that long, every 90 ms or more (before_scene, once a frame),
// a log line: the long gun as last placed (the newer of the item in hand's placement and the carry's: its axis rows and
// origin, world), John's front hand's target (its place, its axes as columns: as that placement's update read it, else
// now), the gun hand's interaction spot, the take-back point (the frame end's, and the front controller's frame's), the
// carry's state (and the ms since its hand-over) and T's settling; and the gun as last drawn with the front target its
// frame drew the hands for (carry_drawn_note). Only the carried gun's placements while carried (the review: a sidearm
// drawn meanwhile is the item in hand). Read only.
void carry_trace_tick() {
    static double last = 0.0;
    const double now = log::now_ms();
    const bool on = now < g_carry_trace_until.load(std::memory_order_relaxed);
    g_carry_trace_on.store(on, std::memory_order_relaxed);
    if (!on || now - last < 90.0) return;
    last = now;
    const uintptr_t cw = dual::state().carry ? dual::secondary_W(nullptr) : 0;  // the carried W (0: none carried)
    Placed a{}, c{};
    bool a_ok = false, c_ok = false;
    for (int tries = 0; tries < 8 && !a_ok; ++tries) {
        const uint32_t s0 = g_placed_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        a = g_placed;
        a_ok = g_placed_seq.load(std::memory_order_acquire) == s0;
    }
    for (int tries = 0; tries < 8 && !c_ok; ++tries) {
        const uint32_t s0 = g_sec_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        c = g_sec_placed;
        c_ok = g_sec_seq.load(std::memory_order_acquire) == s0;
    }
    a_ok = a_ok && a.W && (!cw || a.W == cw) && now - a.ms < 150.0;
    c_ok = c_ok && c.W && c.carry && (!cw || c.W == cw) && now - c.ms < 150.0;
    CarryDrawn dr;
    uint64_t dr_ago = 0;
    {
        std::lock_guard lock(g_draw_mutex);
        dr = g_carry_drawn;
        dr_ago = g_body_frame - dr.frame;
    }
    // 0 none, 1 the item in hand on its hand, 2 the carried gun's put-away (in hand or not yet holstered), 3 the carry
    const Placed* g = a_ok && (!c_ok || a.ms >= c.ms) ? &a : c_ok ? &c : nullptr;
    const int src = !g ? 0 : g == &c ? (c.put ? 2 : 3) : a.follow_j >= 0 ? 2 : 1;
    BodyPoints bp;
    const bool pts = body_points(&bp);
    const int gj = pts ? bp.gun : 1, fj = 1 - gj;
    float gp[3] = {}, tp[3] = {}, tc[3] = {};
    bool by_gun = false;
    if (pts) holster::grab_point(bp, gj, gp);
    const bool tk = holster::carry_take_point(tp, tc, &by_gun);
    const dual::State ds = dual::state();
    float z[12] = {};
    if (g)
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 3; ++k) z[r * 3 + k] = g->m[r * 4 + k];
    const bool paired = g && g->pair_ok;  // (the target this placement was made with: no frame of lag between the two)
    const float* h = paired ? g->pair_t : bp.hand[fj];
    const float* R = paired ? g->pair_r : bp.target_rot[fj];
    log::info("[body] carry trace %.0f: src %d age %.0f paired %d x (%.5f %.5f %.5f) y (%.5f %.5f %.5f) z (%.5f %.5f %.5f) o (%.4f %.4f %.4f) | front %d ok %d "
              "t (%.4f %.4f %.4f) r (%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f) | grip (%.4f %.4f %.4f) | take %d by gun %d (%.4f %.4f %.4f) "
              "ctl (%.4f %.4f %.4f) | carry %d since %.0f T %d returning %d settle %d puts %llu",
              now, src, g ? now - g->ms : -1.0, paired ? 1 : 0, z[0], z[1], z[2], z[3], z[4], z[5], z[6], z[7], z[8], z[9], z[10], z[11], fj, pts && bp.hand_ok[fj] ? 1 : 0,
              h[0], h[1], h[2], R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7], R[8], gp[0], gp[1], gp[2], tk ? 1 : 0, by_gun ? 1 : 0, tp[0], tp[1], tp[2],
              tc[0], tc[1], tc[2], ds.carry ? 1 : 0, ds.carry ? now - ds.carry_ms : -1.0, ds.carry_T_ok ? 1 : 0, dual::carry_returning() ? 1 : 0,
              g_carry_settle_n.load(),
              static_cast<unsigned long long>(g_carry_puts.load()));
    // (its own line, joined to the trace's by the time they share)
    log::info("[body] carry drawn %.0f: ok %d src %d ago %llu R (%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f) o (%.4f %.4f %.4f) | "
              "t (%.4f %.4f %.4f) r (%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f) | wrist %d w (%.4f %.4f %.4f) "
              "wr (%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f) miss %.4f | arm s (%.4f %.4f %.4f) dl %.4f len %.4f d %.4f ks %.3f",
              now, dr.ok ? 1 : 0, dr.src, static_cast<unsigned long long>(dr_ago), dr.R[0], dr.R[1], dr.R[2], dr.R[3], dr.R[4], dr.R[5], dr.R[6],
              dr.R[7], dr.R[8], dr.o[0], dr.o[1], dr.o[2], dr.t[0], dr.t[1], dr.t[2], dr.r[0], dr.r[1], dr.r[2], dr.r[3], dr.r[4], dr.r[5], dr.r[6],
              dr.r[7], dr.r[8], dr.w_ok ? 1 : 0, dr.w_t[0], dr.w_t[1], dr.w_t[2], dr.w_r[0], dr.w_r[1], dr.w_r[2], dr.w_r[3], dr.w_r[4], dr.w_r[5],
              dr.w_r[6], dr.w_r[7], dr.w_r[8], dr.w_miss, dr.w_s[0], dr.w_s[1], dr.w_s[2], dr.w_dl, dr.w_len, dr.w_d, dr.w_ks);
}

bool body_points(BodyPoints* out) {
    BodyPoints p;
    if (!seq_copy(g_hand_seq, &p, &g_points, sizeof(p))) return false;
    if (!p.ms || log::now_ms() - p.ms > 250) return false;
    *out = p;
    return true;
}

int hidden_geometry() { return g_geom_mode.load(); }
void set_hidden_geometry(int mode) {
    mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    if (g_geom_mode.exchange(mode) != mode) log::info("[body] hidden geometry: %s", mode == 2 ? "filter" : mode == 1 ? "skip" : "draw");
    config::set("Body", "HiddenGeometry", mode == 2 ? "filter" : mode == 1 ? "skip" : "draw");
}

const char* grip_source_name() {
    static const char* const kSrc[] = {"the weapon's own grip", "the template (another gun's)", "the built-in grip", "the Sawed-off's (the Double-barrel's grip)"};
    static const char* const kPose[] = {" lowered", " aiming"};
    static thread_local char b[64];
    const int s = g_grip_src_pub.load(std::memory_order_relaxed), p = g_grip_pose_pub.load(std::memory_order_relaxed);
    std::snprintf(b, sizeof(b), "%s%s", s >= 0 && s < 4 ? kSrc[s] : "none", p == 0 || p == 1 ? kPose[p] : "");
    return b;
}

bool auto_shows() {
    std::lock_guard lock(g_cfg_mutex);
    return g_cfg_auto_show;
}
void set_auto_shows(bool on) {
    std::lock_guard lock(g_cfg_mutex);
    g_cfg_auto_show = on;
}
void set_body_with_camera(bool on, bool save) {
    if (g_body_with_cam.exchange(on) != on)
        log::info("[body] riding, the still body %s", on ? "kept where it sat under the camera ([Horse] BodyWithCamera)" : "re-placed on a 0.3 m jump (the old)");
    if (save) config::set("Horse", "BodyWithCamera", on ? "1" : "0");
}
bool body_with_camera() { return g_body_with_cam.load(); }

bool locks_torso() {
    std::lock_guard lock(g_cfg_mutex);
    return g_cfg_lock_torso;
}
int front_pose(int w) { return w >= 0 && w < kGripW ? g_front_pose[w].load() : 0; }
int front_pose_seen(int w) { return w >= 0 && w < kGripW ? static_cast<int>(g_front_seen[w].load()) : 0; }
void set_front_pose(int w, int pose, bool save) {
    if (w < 0 || w >= kGripW || pose < 0 || pose > 2) return;
    g_front_pose[w].store(pose);
    static const char* const kName[3] = {"auto", "lowered", "aiming"};
    log::info("[body] the %s's front hand pose: %s", holster::weapon_label(w), kName[pose]);
    if (save && *holster::weapon_token(w)) config::set((std::string("Weapon.") + holster::weapon_token(w)).c_str(), "FrontHandPose", kName[pose]);
}
void set_locks_torso(bool on) {
    std::lock_guard lock(g_cfg_mutex);
    g_cfg_lock_torso = on;
}

void on_visibility_build() {  // sampled always (the left-handed transplant needs the side); HeldPropFix gates the use
    aim::throw_hold_tick();      // [Gestures] ThrowByGrip: a held throw's clip pinned, or let go
    HeldSample h;
    uintptr_t W = 0, wmgr = 0;
    h.valid = aim::held_item_matrix(h.m, &h.left, &W, &wmgr);
    h.W = h.valid ? W : 0;
    if (h.valid) {
        std::memcpy(h.game, h.m, sizeof(h.game));
        aim::weapon_ik_offsets(W, h.ik, h.ik_hold);
        h.mo_ok = aim::weapon_muzzle_offset(W, h.mo);
        h.aiming = aim::aiming();
    }
    g_held_wmgr.store(wmgr, std::memory_order_relaxed);
    const uint64_t build = g_vis_builds.fetch_add(1, std::memory_order_relaxed);
    h.build = build + 1;
    if (h.valid && g_held_fix_cfg.load(std::memory_order_relaxed) && g_held_at_hand_cfg.load(std::memory_order_relaxed)) {
        // placed at the drawn hand in this frame's update: its records carry that matrix
        for (int tries = 0; tries < 8; ++tries) {
            const uint32_t s0 = g_placed_seq.load(std::memory_order_acquire);
            if (s0 & 1) continue;
            Placed p = g_placed;
            if (g_placed_seq.load(std::memory_order_acquire) != s0) continue;
            if (p.W == W && p.build == build) {
                std::memcpy(h.m, p.m, sizeof(h.m));
                std::memcpy(h.A, p.A, sizeof(h.A));
                std::memcpy(h.a, p.a, sizeof(h.a));
                std::memcpy(h.ad, p.ad, sizeof(h.ad));
                h.placed = true;
                h.follow_j = p.follow_j;  // ([Hands] CarryByForegrip: the carried gun's put-away, at the front hand's target)
                h.pair_ok = p.pair_ok;
                std::memcpy(h.pair_t, p.pair_t, sizeof(h.pair_t));
                std::memcpy(h.pair_r, p.pair_r, sizeof(h.pair_r));
            }
            break;
        }
    }
    g_held_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    g_held_ring[g_held_next++ % kHeldRing] = h;
    g_held_seq.fetch_add(1, std::memory_order_release);
    // [Hands] DualWield: the second gun, placed at the free hand in this frame's update (its records carry that matrix)
    HeldSample s2;
    s2.sec = true;
    s2.build = build + 1;
    if (const uintptr_t sw = dual::secondary_W(nullptr)) {
        for (int tries = 0; tries < 8; ++tries) {
            const uint32_t s0 = g_sec_seq.load(std::memory_order_acquire);
            if (s0 & 1) continue;
            Placed p = g_sec_placed;
            if (g_sec_seq.load(std::memory_order_acquire) != s0) continue;
            if (p.W == sw && p.build == build && p.john >= 0) {
                s2.valid = s2.placed = true;
                s2.W = p.W;
                s2.john = p.john;
                s2.left = p.john == 0;
                std::memcpy(s2.m, p.m, sizeof(s2.m));
                std::memcpy(s2.game, p.m, sizeof(s2.game));
                std::memcpy(s2.A, p.A, sizeof(s2.A));
                std::memcpy(s2.a, p.a, sizeof(s2.a));
                std::memcpy(s2.ad, p.ad, sizeof(s2.ad));
                s2.follow_j = p.follow_j;  // ([Hands] CarryByForegrip: placed rigid at its front target, T not yet made)
                s2.pair_ok = p.pair_ok;
                std::memcpy(s2.pair_t, p.pair_t, sizeof(s2.pair_t));
                std::memcpy(s2.pair_r, p.pair_r, sizeof(s2.pair_r));
                s2.put = p.put;
            }
            break;
        }
    }
    g_sec_ring_seq.fetch_add(1, std::memory_order_acq_rel);
    g_sec_ring[g_sec_next++ % kHeldRing] = s2;
    g_sec_ring_seq.fetch_add(1, std::memory_order_release);
    // [Hands] DualWieldCopy: the copy, placed at the free hand in this frame's update (C; its draws are the held item's)
    HeldSample s3;
    s3.sec = true;
    s3.build = build + 1;
    if (const uintptr_t cw = dual::copy_W()) {
        for (int tries = 0; tries < 8; ++tries) {
            const uint32_t s0 = g_copy_seq.load(std::memory_order_acquire);
            if (s0 & 1) continue;
            Placed p = g_copy_placed;
            if (g_copy_seq.load(std::memory_order_acquire) != s0) continue;
            if (p.W == cw && p.build == build && p.john >= 0) {
                s3.valid = s3.placed = true;
                s3.W = p.W;
                s3.john = p.john;
                s3.left = p.john == 0;
                std::memcpy(s3.m, p.m, sizeof(s3.m));
                std::memcpy(s3.game, p.m, sizeof(s3.game));
                std::memcpy(s3.A, p.A, sizeof(s3.A));
                std::memcpy(s3.a, p.a, sizeof(s3.a));
                std::memcpy(s3.ad, p.ad, sizeof(s3.ad));
                std::memcpy(s3.mshift, p.mshift, sizeof(s3.mshift));
            }
            break;
        }
    }
    g_copy_ring_seq.fetch_add(1, std::memory_order_acq_rel);
    g_copy_ring[g_copy_next++ % kHeldRing] = s3;
    g_copy_ring_seq.fetch_add(1, std::memory_order_release);
}

HandCfg hand_cfg() {
    std::lock_guard lock(g_cfg_mutex);
    return g_hand_cfg;
}
float clamp_stretch(float m) { return !(m >= 1.0f) ? 1.0f : m > 1.6f ? 1.6f : m; }
void set_hand_cfg(const HandCfg& c) {
    std::lock_guard lock(g_cfg_mutex);
    g_hand_cfg = c;
    g_hand_cfg.stretch_max = clamp_stretch(c.stretch_max);
}

bool install() {
    image_mem::init();  // the checked reads' fast path (the working set's page check inside RDR.exe's image)
    {
        std::lock_guard lock(g_cfg_mutex);
        g_cfg_hide = config::get_bool("Body", "HideHead", true);
        g_cfg_back = config::get_float("Body", "BodyBack", 0.04f);
        g_cfg_lock = config::get_bool("Body", "LockFacing", true);
        g_hand_cfg.ik = config::get_bool("Hands", "ArmIK", true);
        g_hand_cfg.wrist_offset = config::get_float("Hands", "WristOffset", 0.065f);
        {
            std::string sh = config::get_string("Body", "Show", "full");
            g_hand_cfg.show = sh == "arms" ? 1 : sh == "hands" ? 2 : 0;
        }
        g_hand_cfg.yaw = config::get_float("Hands", "HandYaw", 0.0f);
        g_hand_cfg.pitch = config::get_float("Hands", "HandPitch", -68.0f);
        g_hand_cfg.roll = config::get_float("Hands", "HandRoll", 0.0f);
        {
            const float cs = config::get_float("Body", "CollapseScale", 1e-7f);
            g_collapse_cfg = !(cs >= 1e-9f) ? 1e-9f : cs > 1e-4f ? 1e-4f : cs;
        }
        g_wind_still_cfg = config::get_bool("Body", "HiddenNoFlutter", true);
        g_no_flutter_cfg = config::get_bool("Body", "NoFlutter", true);
        g_collapse_root_cfg = config::get_bool("Body", "CollapseRoot", true);
        {
            const std::string hg = config::get_string("Body", "HiddenGeometry", "filter");
            g_geom_mode = hg == "skip" ? 1 : hg == "filter" ? 2 : 0;
        }
        g_held_fix_cfg = config::get_bool("Body", "HeldPropFix", true);
        g_held_at_hand_cfg = config::get_bool("Body", "HeldPropAtHand", true);
        {  // [Weapon.<Gun>] FrontHandPose: the long guns' front hand (run 9 item 3b: the aiming hold by default, the user's request)
            static const char* const kPoseName[3] = {"auto", "lowered", "aiming"};
            std::string poses;
            for (int w = 8; w <= 34; ++w) {  // (2026-11-02, round 67 check 3: the Explosive Rifle and the Blunderbuss too)
                if (w > 20 && w != 31 && w != 34) continue;
                const char* tok = holster::weapon_token(w);
                if (!*tok) continue;
                const std::string v = config::get_string((std::string("Weapon.") + tok).c_str(), "FrontHandPose", "aiming");
                g_front_pose[w] = v == "lowered" ? 1 : v == "auto" ? 0 : 2;
                poses += std::string(" ") + tok + "=" + kPoseName[g_front_pose[w].load()];
            }
            log::info("[body] the front hand's pose per long gun (FrontHandPose):%s", poses.c_str());
        }
        g_lasso_cfg = config::get_bool("Body", "LassoAtHands", true);
        g_gun_hand_cfg = config::get_bool("Hands", "GunInGunHand", true);
        g_pin_cfg = config::get_bool("Hands", "TransplantPin", true);
        g_mirror_cfg = config::get_bool("Hands", "TransplantMirror", true);
        g_own_follow = config::get_bool("Hands", "OwnModelFollow", true);
        g_gun_mv = config::get_bool("Hands", "GunMotionVectors", true);
        g_stale_fix = config::get_bool("Hands", "StaleGunFix", true);
        {  // [Hands] GunKick (2026-10-27, off; after optimumbox's recoil ([Hands] Recoil), 0.8.11.10)
            // (the review: the key is not the fork's [Hands] Recoil, which it ships at 1.0: a player bringing the fork's ini
            // would start with ours on; that key is not read, and said so)
            const float rk = config::get_float("Hands", "GunKick", 0.0f);
            g_recoil = std::isfinite(rk) ? std::fmin(2.0f, std::fmax(0.0f, rk)) : 0.0f;
            if (g_recoil.load() > 0.0f) log::info("[body] the guns' kick (GunKick): %.2f (the drawn gun only: the aim and the shots stay on the barrel as held)", g_recoil.load());
            if (!config::get_string("Hands", "Recoil", "").empty())
                log::info("[body] [Hands] Recoil (optimumbox's fork's key) is not read: this mod's gun kick is [Hands] GunKick (%.2f)", g_recoil.load());
        }
        g_coll_cfg = config::get_bool("Hands", "GunCollision", false);  // (2026-10-29, GOAL-weapons-handling item 4)
        if (g_coll_cfg.load()) log::info("[body] the guns' collisions (GunCollision): on (the drawn guns only: the aim and the shots stay on the barrel as held)");
        g_arm_mv = config::get_bool("Body", "ArmMotionVectors", false);  // run 12 item 9
        g_body_with_cam = config::get_bool("Horse", "BodyWithCamera", true);  // run 12 item 8 (on: cycles GL2/GF1 and their captures)
        {  // run 12 item 7: run 11's props belt retired (its keys left in a user ini do nothing)
            std::string on;
            for (const char* k : {"PropBelt", "ShowKnife", "ShowSatchel", "ShowLassoCoil", "ShowAmmoPouch"})
                if (config::get_bool("Holsters", k, false)) on += std::string(on.empty() ? "" : ", ") + k;
            if (!on.empty())
                log::info("[body] retired in run 12 (0.8.x): the belt of the game's props ([Holsters] %s on in your ini: ignored); "
                          "John's own belt's pieces instead (the Holsters tab's \"John's belt\")", on.c_str());
        }
        g_own_pieces = config::get_bool("Holsters", "OwnBelt", false);  // run 12 item 2
        g_rigid_pieces = config::get_bool("Holsters", "OwnBeltRigid", true);  // 2026-10-13: the moved pieces drawn rigid (drawn from a set copy)
        g_left_holster = config::get_bool("Holsters", "OwnBeltLeftHolster", false);  // run 12 item 5
        {
            std::lock_guard lock(g_draw_mutex);
            body::Piece& p = g_lh_piece;
            sscanf_s(config::get_string("Holsters", "OwnBeltLeftHolsterOffset", "0 0 0").c_str(), "%f %f %f", &p.d[0], &p.d[1], &p.d[2]);
            sscanf_s(config::get_string("Holsters", "OwnBeltLeftHolsterTurn", "0 0 0").c_str(), "%f %f %f", &p.turn[0], &p.turn[1], &p.turn[2]);
        }
        {
            std::lock_guard lock(g_draw_mutex);
            for (int i = 0; i < body::kPieces; ++i) {
                body::Piece& p = g_piece[i];
                const std::string k = std::string("OwnBelt") + body::piece_key(i);
                sscanf_s(config::get_string("Holsters", (k + "Offset").c_str(), "0 0 0").c_str(), "%f %f %f", &p.d[0], &p.d[1], &p.d[2]);
                sscanf_s(config::get_string("Holsters", (k + "Turn").c_str(), "0 0 0").c_str(), "%f %f %f", &p.turn[0], &p.turn[1], &p.turn[2]);
            }
        }
        g_fixed_grip = config::get_bool("Hands", "FixedGunGrip", false);
        g_gun_steady = config::get_bool("Hands", "GunSteady", true);
        g_fixed_sidearm = config::get_bool("Hands", "FixedSidearmGrip", false);
        g_follow_auto = config::get_bool("Debug", "FollowLog", true);
        load_gun_rels();
        g_copy_grip_cfg = config::get_bool("Hands", "CopyGrip", true);
        g_carry_ease_ms = std::clamp(config::get_float("Hands", "CarryEaseMs", 150.0f), 0.0f, 1000.0f);  // (2026-10-31, step 3, R10)
        g_same_frame_cfg = config::get_bool("Reload", "TwoHandedSteady", true);
        g_two_steer_cfg = config::get_bool("Reload", "TwoHandedSteer", true);
        g_snap_drawn_cfg = config::get_bool("Reload", "SnapToDrawnGun", true);
        g_shotlog_cfg = config::get_bool("Debug", "TwoHandShotLog", true);
        log::info("[body] the front hand's snap from the gun as drawn with FixedGunGrip (SnapToDrawnGun) %d", g_snap_drawn_cfg.load() ? 1 : 0);
        g_twist_cfg = config::get_bool("Hands", "ArmTwist", true);
        g_fallback_cfg = config::get_bool("Reload", "GripFallback", true);
        g_sawed_grip = config::get_bool("Reload", "SawedOffGrip", true);
        g_steady_grip = config::get_bool("Reload", "SteadyRing", true);
        {
            const float ts = config::get_float("Hands", "ForearmTwistShare", 0.5f);
            g_twist_share = !(ts >= 0.0f) ? 0.0f : ts > 1.0f ? 1.0f : ts;
        }
        {
            const std::string fm = config::get_string("Hands", "TransplantFingers", "swap");
            g_fingers_cfg = fm == "off" ? 0 : fm == "open" ? 2 : 1;
        }
        g_hand_cfg.stretch = config::get_bool("Hands", "ArmStretch", true);
        g_hand_cfg.stretch_max = clamp_stretch(config::get_float("Hands", "ArmStretchMax", 1.3f));
        g_cfg_pitch = config::get_float("Body", "TorsoPitch", 2.0f);
        g_cfg_lock_torso = config::get_bool("Body", "LockTorso", true);
        g_cfg_auto_show = config::get_bool("Body", "AutoShow", true);
        std::string st = config::get_string("Body", "Stance", "upright");
        g_cfg_stance = st == "game" ? 0 : st == "lean" ? 2 : st == "still" ? 3 : 1;
        g_cfg_height_tau = config::get_float("Body", "HeightSmoothing", 0.35f);
        g_cfg_still_feet = config::get_bool("Body", "StillFeet", false);
        g_cfg_still_belt = config::get_bool("Body", "StillBelt", true);
        g_cfg_follow = config::get_bool("Body", "BodyFollowsHead", false);
        g_cfg_up_bone = config::get_string("Body", "UprightBone", "spine00");
        g_cfg_fwd_sign = config::get_float("Body", "ForwardSign", 1.0f) < 0 ? -1.0f : 1.0f;
        log::info("[body] shown: %s; forearms and hands when crouching, in cover or riding %d",
                  g_hand_cfg.show == 2 ? "hands" : g_hand_cfg.show == 1 ? "forearms and hands" : "the whole body", g_cfg_auto_show ? 1 : 0);
        log::info("[body] arm stretch %s (at most %.2fx John's reach)", g_hand_cfg.stretch ? "on" : "off", g_hand_cfg.stretch_max);
        log::info("[body] hidden parts: collapse %.0e, the root too %d; no cloth flutter on the player's draws while parts are hidden %d; "
                  "hidden geometry: %s",
                  g_collapse_cfg.load(), g_collapse_root_cfg.load() ? 1 : 0, g_wind_still_cfg.load() ? 1 : 0,
                  g_geom_mode.load() == 2 ? "filter" : g_geom_mode.load() == 1 ? "skip" : "draw");
        log::info("[body] the item in hand known by its game matrix (never hidden, on the hand) %d, placed at the drawn hand %d",
                  g_held_fix_cfg.load() ? 1 : 0, g_held_at_hand_cfg.load() ? 1 : 0);
        log::info("[body] the arms' roll from the IK %d (the forearm's roll bone takes %.2f of the hand's twist)", g_twist_cfg.load() ? 1 : 0,
                  g_twist_share.load());
        log::info("[body] left-handed: the gun in John's left hand, each arm on its own controller %d (fingers %s, pin %d)",
                  g_gun_hand_cfg.load() ? 1 : 0, g_fingers_cfg.load() == 0 ? "off" : g_fingers_cfg.load() == 2 ? "open" : "swap", g_pin_cfg.load() ? 1 : 0);
    }
    bool ok = hooks::install("RDR DrawVisEntity (body)", reinterpret_cast<void*>(anchors::addr(anchors::Id::DrawVisEntity)), hk_DrawVisEntity,
                             &o_DrawVisEntity);
    ok &= hooks::install("RDR one geometry's draw (hidden parts not drawn)", reinterpret_cast<void*>(anchors::addr(anchors::Id::DrawModelGeometry)),
                         hk_draw_geom, &o_draw_geom);
    ok &= hooks::install("RDR draw recorder (the hidden triangles' filter)", reinterpret_cast<void*>(anchors::addr(anchors::Id::RecorderDraw)),
                         hk_rec_draw, &o_rec_draw);
    if (g_lasso_cfg.load()) {  // (the anchors verified: as the other hooks here)
        ok &= hooks::install("RDR the lasso's draw (its rope at the drawn hands)", reinterpret_cast<void*>(anchors::addr(anchors::Id::LassoRopeDraw)),
                             hk_lasso_draw, &o_lasso_draw);
        ok &= hooks::install("RDR a rope's points draw (the lasso's corrected)", reinterpret_cast<void*>(anchors::addr(anchors::Id::RopePointsDraw)),
                             hk_rope_points, &o_rope_points);
    }
    if (g_held_fix_cfg.load() && g_held_at_hand_cfg.load()) {
        g_gun_place_ret = anchors::addr(anchors::Id::GunPropPlaceRet);
        ok &= hooks::install("RDR object matrix (the held prop at the drawn hand)", reinterpret_cast<void*>(anchors::addr(anchors::Id::ObjectSetMatrix)),
                             hk_obj_set_matrix, &o_obj_set_matrix);
    }
    return ok;
}

void before_scene(const float* cam) {
    carry_trace_tick();  // ("skel carry trace": while asked for)
    {  // the filter's new entries, logged here (never in the draw hook), at most 4 a frame
        std::lock_guard lock(g_cull_note_mutex);
        for (int k = 0; k < 4 && g_cull_note_logged < g_cull_note_n; ++k, ++g_cull_note_logged) {
            if (g_cull_note_n - g_cull_note_logged > 64) g_cull_note_logged = g_cull_note_n - 64;
            const CullNote& n = g_cull_notes[g_cull_note_logged % 64];
            static const char* const kKind[5] = {"?", "skip", "draw", "filter", "rejected"};
            log::limited("body.cull", 400, "[body] hidden triangles: geom %llx show %u: %u tris (%u verts), keep %u, drop %u, any %u, %u runs, "
                         "reading %u, weights 255 %u, kind %s%s", static_cast<unsigned long long>(n.geom), n.show, n.tris, n.verts, n.keep, n.drop,
                         n.any, n.nruns, n.reading, n.w255, kKind[n.kind < 5 ? n.kind : 0], n.why ? " (why " : "");
        }
    }
    int o = g_override.load(std::memory_order_relaxed);
    bool active = o == 1 || (o == 0 && pose::anchor_active());
    if (active != g_active.load(std::memory_order_relaxed)) {
        g_active = active;
        log::info("[body] first-person body %s", active ? "on" : "off");
    }
    g_frame.valid = false;
    if (!active) return;
    bool hide, follow, lock_facing;
    float back, sign, pitch_deg;
    int stance;
    std::string up_name;
    {
        std::lock_guard lock(g_cfg_mutex);
        hide = g_cfg_hide;
        follow = g_cfg_follow;
        back = g_cfg_back;
        sign = g_cfg_fwd_sign;
        stance = g_cfg_stance;
        up_name = g_cfg_up_bone;
        lock_facing = g_cfg_lock;
        pitch_deg = g_cfg_pitch;
    }
    uintptr_t skel = player_skeleton(), data = 0, mtx = 0;
    if (!skel || !rd(skel + 8, &data) || !data || !rd(skel + 0x28, &mtx) || !mtx) return;
    {
        std::lock_guard lock(g_draw_mutex);  // the rig is read by the draws
        if (!build_rig(data, up_name)) return;
    }
    int top = g_rig.head > g_rig.up ? g_rig.head : g_rig.up;
    top = g_rig.pelvis > top ? g_rig.pelvis : top;
    for (int hh = 0; hh < 2; ++hh) top = g_rig.wrist[hh] > top ? g_rig.wrist[hh] : top;
    for (int i = 0; i < 4; ++i) top = g_rig.chain[i] > top ? g_rig.chain[i] : top;
    if (!readable(mtx, static_cast<size_t>(top + 1) * 0x40)) return;
    const float* w = reinterpret_cast<const float*>(mtx);  // the root bone's world matrix: rows x, y, z, translation
    auto local = [&](int bone, float* out) {
        const float* m = reinterpret_cast<const float*>(mtx + static_cast<uintptr_t>(bone) * 0x40);
        float d[3] = {m[12] - w[12], m[13] - w[13], m[14] - w[14]};
        for (int k = 0; k < 3; ++k) out[k] = d[0] * w[k * 4 + 0] + d[1] * w[k * 4 + 1] + d[2] * w[k * 4 + 2];
    };
    Frame f;
    f.skel = skel;
    f.hide = hide;
    if (cam) {
        std::memcpy(f.cam, cam, sizeof(f.cam));
        f.cam_ok = true;
    }
    local(g_rig.head, f.head_local);
    const float* hw = reinterpret_cast<const float*>(mtx + static_cast<uintptr_t>(g_rig.head) * 0x40 + 0x30);
    for (int k = 0; k < 3; ++k) f.head_world[k] = hw[k];
    f.stance = g_rig.up >= 0 ? stance : 0;
    if (g_rig.up >= 0) local(g_rig.up, f.up_local);
    for (int i = 0; i < 4; ++i)
        if (g_rig.chain[i] >= 0) local(g_rig.chain[i], f.chain_local[i]);
    if (g_rig.pelvis >= 0) local(g_rig.pelvis, f.pelvis_local);
    if (cam) {
        for (int k = 0; k < 3; ++k) {
            g_cam_diag[k] = cam[12 + k];
            g_cam_diag[3 + k] = f.head_world[k];
            g_cam_diag[6 + k] = w[12 + k];
        }
    }
    // the arms on the controllers
    HandCfg hc;
    bool auto_show = false;
    {
        std::lock_guard lock(g_cfg_mutex);
        hc = g_hand_cfg;
        f.lock_torso = g_cfg_lock_torso;
        auto_show = g_cfg_auto_show;
    }
    uint32_t flags = 0;
    {
        RdrvrActorState st{};
        if (api::actor_state(&st)) {
            std::memcpy(f.actor_world, st.pos, sizeof(f.actor_world));
            f.actor_valid = true;
            flags = st.flags;
        }
    }
    g_actor_flags.store(flags, std::memory_order_relaxed);
    {  // [Body] Stance=still: the bind pose's origin, the head's height low-passed as the camera's (pose.cpp)
        static bool s_ok = false, prev_ok = false;
        static float s_h = 0.0f, prev_t[3] = {};
        static double s_ms = 0.0;
        const double now = log::now_ms();
        if (f.stance == 3 && f.actor_valid && g_rig.head >= 0) {
            float tau;
            bool feet;
            {
                std::lock_guard lock(g_cfg_mutex);
                tau = g_cfg_height_tau;
                feet = g_cfg_still_feet;
            }
            // the height by the head's (stage 1, the user's "looks really good"): the camera's height follows the head, so
            // the body stays rigid with the view (by the pelvis's alone, the head's own bob and look moved it 6-11 mm up and
            // down against the camera: run 10 cycles F1, F2). With StillFeet, plus the pelvis's own place against the
            // head's, low-passed over 3 s (run 10, cycle D5: by the head's alone the pinned pelvis sat lower than the
            // animation's, the head being lower against the pelvis than in the bind pose: the knees bent to 153-159 deg
            // standing against the animation's 167)
            const int hb = g_rig.head;
            const float jh = g_rig.jbind[static_cast<size_t>(hb) * 3 + 1];
            const float hy = f.head_world[1] - f.actor_world[1];
            const double dt = (now - s_ms) * 0.001;
            static float s_pd = 0.0f;  // the pelvis's height against the head's, each from its bind place (StillFeet)
            static bool s_pd_ok = false;
            if (feet && g_rig.pelvis >= 0) {
                const float pd = (reinterpret_cast<const float*>(mtx + static_cast<uintptr_t>(g_rig.pelvis) * 0x40 + 0x30)[1] -
                                  g_rig.jbind[static_cast<size_t>(g_rig.pelvis) * 3 + 1]) -
                                 (f.head_world[1] - jh);
                if (!std::isfinite(pd) || std::fabs(pd) > 0.5f) {
                } else if (!s_pd_ok || dt > 1.0 || dt < 0.0) {
                    s_pd = pd;
                    s_pd_ok = true;
                } else {
                    s_pd += (pd - s_pd) * static_cast<float>(1.0 - std::exp(-dt / 3.0));
                }
            } else {
                s_pd_ok = false;
            }
            bool hold = false;  // run 10 item 8b: a gun drawn standing, the height held as the view's is (pose.cpp)
            {
                RdrvrActorState ast{};
                hold = s_ok && api::actor_state(&ast) && pose::holds_armed_height(ast);
            }
            if (!s_ok || !std::isfinite(s_h) || dt > 1.0 || std::fabs(hy - s_h) > 1.0f) s_h = hy;  // a first frame, a stall, a jump
            else if (hold) {  // only a rise let through (a crouch left with the gun out), as the view's
                if (hy > s_h && tau > 1e-3f) s_h += (hy - s_h) * static_cast<float>(1.0 - std::exp(-dt / tau));
            } else if (tau > 1e-3f) s_h += (hy - s_h) * static_cast<float>(1.0 - std::exp(-dt / tau));
            else s_h = jh;  // 0: the standing height always
            s_ok = true;
            f.still_t[0] = f.actor_world[0];
            f.still_t[1] = f.actor_world[1] + s_h - jh + (s_pd_ok ? s_pd : 0.0f);
            f.still_t[2] = f.actor_world[2];
            // held under this frame's camera: the actor's position comes from the script thread (a tick old, 18 mm at
            // a walk) and the camera from the frame, so the origin's offset from the camera, in the view's heading, is
            // low-passed (0.5 s) and the origin put at the camera less it. A camera far from the actor (a script's, a
            // cutscene's, a teleport): the actor's place, the filter restarted.
            static bool o_ok = false;
            static float o_lp[3] = {};
            if (f.cam_ok && dt >= 0.0) {
                const float h = pose::torso_heading_deg() * 0.0174532925f, ch = std::cos(h), sh = std::sin(h);
                const float d[3] = {f.cam[12] - f.still_t[0], f.cam[13] - f.still_t[1], f.cam[14] - f.still_t[2]};
                // right (cos h, -sin h), up, forward (-sin h, -cos h)
                const float ov[3] = {ch * d[0] - sh * d[2], d[1], -sh * d[0] - ch * d[2]};
                const float jump = std::sqrt((ov[0] - o_lp[0]) * (ov[0] - o_lp[0]) + (ov[1] - o_lp[1]) * (ov[1] - o_lp[1]) +
                                             (ov[2] - o_lp[2]) * (ov[2] - o_lp[2]));
                if (!std::isfinite(ov[0] + ov[1] + ov[2]) || ov[0] * ov[0] + ov[2] * ov[2] > 1.0f || std::fabs(ov[1] - jh) > 1.5f) {
                    o_ok = false;  // not the anchored camera
                    ++g_still_off_cam;
                } else {
                    // learned only while the camera stands (moving, the actor's lag behind it grows and shrinks, and the
                    // height's smoothing differs from the camera's by the bob's remainder: the offset held, so the body
                    // stays rigid with the camera; a crouch or a slope moves the camera, and the body with it)
                    static float c_last[3] = {}, c_speed = 0.0f;
                    if (o_ok && dt > 1e-4) {
                        const float dx = f.cam[12] - c_last[0], dz = f.cam[14] - c_last[2];
                        const float sp = std::sqrt(dx * dx + dz * dz) / static_cast<float>(dt);
                        c_speed += (sp - c_speed) * static_cast<float>(1.0 - std::exp(-dt / 0.2));
                    }
                    c_last[0] = f.cam[12];
                    c_last[2] = f.cam[14];
                    const float a = static_cast<float>(1.0 - std::exp(-dt / 0.5));
                    const bool riding = (flags & RDRVR_ACTOR_MOUNTED) != 0;
                    const bool keep = riding && g_body_with_cam.load(std::memory_order_relaxed);  // [Horse] BodyWithCamera
                    if (o_ok && jump > 0.3f && jump <= 1.5f && dt <= 1.0 && keep) g_still_kept_riding.fetch_add(1, std::memory_order_relaxed);
                    if (!o_ok || jump > (keep ? 1.5f : 0.3f) || dt > 1.0) {
                        if (o_ok) {
                            g_still_resets.fetch_add(1, std::memory_order_relaxed);
                            if (riding) g_still_resets_riding.fetch_add(1, std::memory_order_relaxed);
                        }
                        std::memcpy(o_lp, ov, sizeof(o_lp));
                        c_speed = 0.0f;
                    } else {
                        if (c_speed < 0.1f)
                            for (int k = 0; k < 3; ++k) o_lp[k] += (ov[k] - o_lp[k]) * a;
                    }
                    o_ok = true;
                    f.still_t[0] = f.cam[12] - (ch * o_lp[0] - sh * o_lp[2]);
                    f.still_t[1] = f.cam[13] - o_lp[1];
                    f.still_t[2] = f.cam[14] - (-sh * o_lp[0] - ch * o_lp[2]);
                }
            } else {
                o_ok = false;
            }
            std::memcpy(f.still_t_prev, prev_ok ? prev_t : f.still_t, sizeof(f.still_t_prev));
            std::memcpy(prev_t, f.still_t, sizeof(prev_t));
            prev_ok = true;
            f.still = true;
            f.ride_keep = (flags & RDRVR_ACTOR_MOUNTED) && g_body_with_cam.load(std::memory_order_relaxed);
            {
                std::lock_guard lock(g_cfg_mutex);
                f.still_feet = g_cfg_still_feet;
                f.still_belt = g_cfg_still_belt;
            }
        } else {
            s_ok = prev_ok = false;
        }
        s_ms = now;
    }
    // the user's choice (2026-10-04): crouching, in cover, riding or driving show the forearms and hands (or less, if the
    // global setting already shows less), the body getting in the way of the view there
    f.show = hc.show;
    if (auto_show && (flags & (RDRVR_ACTOR_CROUCHING | RDRVR_ACTOR_IN_COVER | RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)) && f.show < 1)
        f.show = 1;
    f.stretch_max = hc.stretch ? hc.stretch_max : 1.0f;
    // 2026-10-13 ([Hands] MountedGunFirstPerson): on a mounted gun the gun is the game's turret: no held item's place
    // in the hand, no second gun, and John's arms left on its handles (the IK below off)
    const bool on_turret = pose::on_mounted_gun();
    // the held item's samples and the second gun's (here, before their first reader: the carried gun's put-away below,
    // carry_put)
    if (!on_turret && seq_copy(g_held_seq, f.held, g_held_ring, sizeof(f.held))) {
        for (const HeldSample& hs : f.held) f.held_any |= hs.valid;
    } else {
        for (HeldSample& hs : f.held) hs.valid = false;
    }
    for (int i = 0; i < kHeldRing; ++i)
        if (f.held[i].build && (f.held_new < 0 || f.held[i].build > f.held[f.held_new].build)) f.held_new = i;
    if (!on_turret && seq_copy(g_sec_ring_seq, f.sec, g_sec_ring, sizeof(f.sec))) {  // [Hands] DualWield: the second gun's samples
        for (const HeldSample& hs : f.sec) f.sec_any |= hs.valid;
    } else {
        for (HeldSample& hs : f.sec) hs.valid = false;
    }
    for (int i = 0; i < kHeldRing; ++i)
        if (f.sec[i].valid && (f.sec_new < 0 || f.sec[i].build > f.sec[f.sec_new].build)) f.sec_new = i;
    for (const HeldSample& hs : f.sec) f.sec_build = hs.build > f.sec_build ? hs.build : f.sec_build;  // valid or not
    {
        RdrvrActorState ws{};
        f.long_gun = api::actor_state(&ws) && reload::is_long_gun(ws.weapon) && reload::two_handed();
        f.weapon = ws.valid ? ws.weapon : -1;
        float fr = 0;
        // (C8, the review: f.gun_j is set below: the same rule here)
        const int fgj = g_gun_hand_cfg.load(std::memory_order_relaxed) || !controls::left_handed() ? controls::gun_hand() : 1;
        holster::gun_adjust(f.weapon, holster::kAdjForegrip, f.fore_off, &fr, nullptr, fgj);  // round 13 item 8: the gun's own, else every gun's (C8: its hand's)
        f.snap = holster::foregrip_snap();
        f.two_blend = holster::two_hand_blend();
        {
            const dual::State ds = dual::state();
            f.carry_j = ds.carry && ds.john >= 0 && ds.john <= 1 ? ds.john : -1;
            // (the newest held sample placed at the front hand: exactly what this frame draws, from the published
            // samples, no read of the game's memory here; the review: the game's item read here could disagree with the
            // drawn build for a frame at the put-away's ends)
            // (r228: or, out of the game's hand but not yet holstered, the newest second gun's sample placed so)
            const HeldSample* hn = newest_held(f.held);
            f.carry_put = (hn && hn->follow_j >= 0) || (f.sec_new >= 0 && f.sec[f.sec_new].build == f.sec_build && f.sec[f.sec_new].put);
            // (the review: only with the option on, so nothing changes with it off)
            f.sec_j = dual::cross_reload() && ds.on && !ds.carry && ds.john >= 0 && ds.john <= 1 ? ds.john : -1;
            // [Hands] GunKick (2026-10-27): the second gun's or the copy's hand (its spring's); never the carry (it never fires)
            f.kick_j[1] = ds.on && !ds.carry && ds.john >= 0 && ds.john <= 1 ? ds.john : -1;
            f.kick_key[1] = f.kick_j[1] < 0 ? -1 : (ds.copy ? 100000 : 0) + (ds.slot + 1) * 100 + ds.weapon + 1;
            f.round_hand = round_draw::held_hand();
        }
        f.barrel_blend = actions::barrel_snap();
        f.part_blend = actions::part_snap(f.part_p, &f.part_turn, &f.part_bolt);
        f.cpart_blend = actions::carried_part_snap(f.cpart_p, f.cpart_R, &f.cpart_turn, &f.cpart_j);
        f.thumb_w = actions::thumb_on_hammer(&f.thumb_wt, &f.thumb_hammer);
        f.sthumb_w = actions::second_thumb_on_hammer(&f.sthumb_wt);  // (C3)
        f.sthumb_j = f.kick_j[1];  // (the second gun's or the copy's hand, as its kick's)
        if (f.thumb_w >= 0 && g_thumb_hold.load(std::memory_order_relaxed)) f.thumb_wt = 1.0f;  // "skel thumb hold on": on the spur (a test aid)
        {
            int ow = -1;
            const float am = actions::open_amount(&ow);
            f.barrel_open = ow == 15 || ow == 16 ? am : 0.0f;  // the break actions (their barrels' hinge is the same)
        }
        f.pump = actions::pump_drawn();
    }
    f.collapse = g_collapse_cfg.load(std::memory_order_relaxed);
    f.collapse_root = g_collapse_root_cfg.load(std::memory_order_relaxed);
    f.still_cloth = g_wind_still_cfg.load(std::memory_order_relaxed);
    f.no_flutter = g_no_flutter_cfg.load(std::memory_order_relaxed);
    {
        static bool stretch_logged = false;
        if (!stretch_logged && g_stretches.load(std::memory_order_relaxed)) {
            stretch_logged = true;
            log::info("[body] arm stretch: an arm lengthened past John's reach (the first this session)");
        }
    }
    float grip_wp[2][3] = {};  // each hand's controller grip (world), for the two-handed aim below
    // [Reload] LeverControl=hold (2026-10-28): the drawn gun held while the stick is held and the lever is worked: held
    // two-handed, carried by the front hand (gun_front, below, as LeverControl=front: the carry taken only on a full hold),
    // else the gun hand's controller pose held (gun_lock, its place too)
    const bool hold_lk = actions::lever_hold_lock();
    static bool hold_front_on = false;
    {
        const float tb = holster::two_hand_blend();
        hold_front_on = hold_lk && (hold_front_on ? tb > 0.5f : tb > 0.99f);
    }
    const bool hold_front = hold_front_on;
    {  // left-handed with GunInGunHand: each arm on its own controller, the gun drawn in John's left hand
        const bool lefty = controls::left_handed(), gih = g_gun_hand_cfg.load(std::memory_order_relaxed);
        f.gun_j = gih || !lefty ? controls::gun_hand() : 1;  // each arm on its own controller: the gun hand's (it drew)
        for (int hh = 0; hh < 2; ++hh) f.ctrl[hh] = gih || !lefty ? hh : 1 - hh;
        f.pin = g_pin_cfg.load(std::memory_order_relaxed);
        f.mirror = g_mirror_cfg.load(std::memory_order_relaxed);
    }
    for (int hh = 0; hh < 2 && hc.ik; ++hh) {
        if (g_rig.arm[hh] < 0 || g_rig.elbow[hh] < 0 || g_rig.wrist[hh] < 0) continue;
        // f.ctrl: each arm its own controller; without GunInGunHand, left-handed, John's right hand (the gun's) on the
        // left controller and his left on the right one (the arms cross). The grip frames of both controllers point the
        // same way, so the calibration and the palm stay John's hand's
        hands::Hand hd = hands::get(f.ctrl[hh]);
        const float* hp = hd.grip_valid ? hd.grip_pos : hd.pos;
        const float* hr = hd.grip_valid ? hd.grip_rot : hd.rot;
        if (!(hd.grip_valid || hd.valid)) continue;
        float hr_lock[4], hp_lock[3];
        if (hh == f.gun_j) {  // [Reload] LeverControl=lock: the gun's orientation held while the lever is worked (hold: its place too)
            gun_lock(hr, hp, hold_lk && !hold_front && !f.carry_put, hr_lock, hp_lock);  // (not the carried gun's put-away)
            hr = hr_lock;
            hp = hp_lock;
        }
        float wp[3], wrr[9];
        if (!camera_lever::local_to_world(cam, hp, hr, wp, wrr)) continue;
        // the calibration turn in the grip frame (mirrored yaw and roll for the left hand)
        float sy = hh ? 1.0f : -1.0f;
        float yaw = hc.yaw * sy * 0.0174532925f, pit = hc.pitch * 0.0174532925f, rol = hc.roll * sy * 0.0174532925f;
        float ry[9], rx[9], rz[9], t1[9], off[9], rr[9];
        const float ay[3] = {0, 1, 0}, ax_[3] = {1, 0, 0}, az[3] = {0, 0, 1};
        axis_angle(ay, yaw, ry);
        axis_angle(ax_, pit, rx);
        axis_angle(az, rol, rz);
        mul3(ry, rx, t1);
        mul3(t1, rz, off);
        mul3(wrr, off, rr);
        std::memcpy(f.ik_r[hh], rr, sizeof(rr));
        std::memcpy(grip_wp[hh], wp, sizeof(wp));
        for (int k = 0; k < 3; ++k) f.ik_t[hh][k] = wp[k] + rr[k * 3 + 2] * hc.wrist_offset;  // back along the grip's +z
        // the elbow's hint: down, out to the side, a little back (the camera's heading)
        float hd_ = pose::torso_heading_deg() * 0.0174532925f;  // run 9: the horse's while the ride's view is turned
        float fwd[3] = {-std::sin(hd_), 0, -std::cos(hd_)}, right[3] = {std::cos(hd_), 0, -std::sin(hd_)};
        float out = hh ? 1.0f : -1.0f;
        for (int k = 0; k < 3; ++k) f.ik_pole[hh][k] = (k == 1 ? -1.0f : 0.0f) + 0.6f * out * right[k] - 0.3f * fwd[k];
        local(g_rig.arm[hh], f.arm_local[hh]);
        local(g_rig.elbow[hh], f.elbow_local[hh]);
        local(g_rig.wrist[hh], f.wrist_local[hh]);
        f.ik[hh] = !pose::on_mounted_gun();  // (2026-10-13: a mounted gun: the targets kept for its aim, the arms left on its handles)
        if (hh == 1 && g_ik_test.load()) {  // along the actor's forward (the root bone's -z), level
            float fx = -w[8], fz = -w[10], fl = std::sqrt(fx * fx + fz * fz);
            if (fl > 1e-3f) {
                f.ik_t[1][0] = f.head_world[0] + fx / fl * 0.3f;
                f.ik_t[1][1] = f.head_world[1] - 0.1f;
                f.ik_t[1][2] = f.head_world[2] + fz / fl * 0.3f;
            }
        }
        if (hh == 1)
            for (int k = 0; k < 3; ++k) g_cam_diag[9 + k] = f.ik_t[1][k];
    }
    // two-handed long guns (holster.cpp: the front hand on the foregrip): John's gun hand (the right) turns so the
    // barrel points from its grip to the front hand's, by the blend's share of the turn (rigid: the aim follows)
    {
        const float tb = holster::two_hand_blend();
        float bt[3];
        const int gj = f.gun_j, fj = 1 - gj;
        bool bt_ok = false;
        if (g_same_frame_cfg.load(std::memory_order_relaxed)) {
            std::lock_guard lock(g_draw_mutex);
            if (g_bt_body_ok && g_body_frame - g_bt_body_frame <= 2) {
                std::memcpy(bt, g_bt_body, sizeof(bt));
                bt_ok = true;
            }
        }
        if (!bt_ok) bt_ok = aim::barrel_in_target(bt);
        {  // [Reload] LeverParts: through John's fire clip the barrel is the one before it (the clip swings the game's
           // gun, and the drawn one is held still in this target: the turn must not follow the swing)
            static float bt_rest[3] = {};
            static bool rest_ok = false;
            const bool steady = (worked_flags() & kSteady) != 0;
            if (bt_ok && (!steady || !in_fire_clip())) {
                std::memcpy(bt_rest, bt, sizeof(bt));
                rest_ok = true;
            } else if (steady && rest_ok) {
                std::memcpy(bt, bt_rest, sizeof(bt));
                bt_ok = true;
            }
        }
        // ([Reload] TwoHandedSteer off: the front hand only drawn on the gun, the gun the gun hand's alone)
        const bool stock = holster::physical_stock();  // (read once a frame: the review)
        // ([Hands] CarryByForegrip: not on the carried gun's put-away: the gun hand has let go, its target is the controller's)
        if (tb > 0.0f && f.ik[0] && f.ik[1] && bt_ok && g_two_steer_cfg.load(std::memory_order_relaxed) && !stock && !f.carry_put) {
            {  // "skel lag": the barrel-in-target's change since the last frame's
                static float last[3] = {};
                static uint64_t last_frame = ~0ull;  // the frame of `last`: compared only with the frame before
                float bn[3] = {bt[0], bt[1], bt[2]};
                norm(bn);
                if (last_frame + 1 == g_body_frame && !g_bt_jit_rebase) {
                    const float c = std::clamp(bn[0] * last[0] + bn[1] * last[1] + bn[2] * last[2], -1.0f, 1.0f);
                    const float deg = std::acos(c) * 57.29578f;
                    std::lock_guard lock(g_draw_mutex);
                    g_bt_jit_max = std::fmax(g_bt_jit_max, deg);
                    g_bt_jit_sum += deg;
                    ++g_bt_jit_n;
                    if (deg > 0.5f) ++g_bt_jit_over;
                }
                std::memcpy(last, bn, sizeof(bn));
                last_frame = g_body_frame;
                g_bt_jit_rebase = false;
            }
            const float* R = f.ik_r[gj];
            float u[3], d[3];
            for (int k = 0; k < 3; ++k) {
                u[k] = R[k * 3] * bt[0] + R[k * 3 + 1] * bt[1] + R[k * 3 + 2] * bt[2];
                d[k] = grip_wp[fj][k] - grip_wp[gj][k];
            }
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            float ax[3];
            cross(u, d, ax);
            const float sn = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            if (dl > 0.05f && sn > 1e-6f) {
                for (int k = 0; k < 3; ++k) ax[k] /= sn;
                const float cs = (u[0] * d[0] + u[1] * d[1] + u[2] * d[2]) / dl;
                float Rt[9], Rn[9];
                axis_angle(ax, std::atan2(sn / dl, cs) * tb, Rt);
                mul3(Rt, R, Rn);
                std::memcpy(f.ik_r[gj], Rn, sizeof(Rn));
                for (int k = 0; k < 3; ++k) f.ik_t[gj][k] = grip_wp[gj][k] + Rn[k * 3 + 2] * hc.wrist_offset;
                g_two_diag = std::atan2(sn / dl, cs) * 57.29578f;
            }
        }
        // [Reload] PhysicalStock (run 14, after optimumbox's 0.8.10.4): every long gun turned in the gun hand's target by
        // [Reload] StockTurn; the calibration's sample learns it: with U = R C0 (the target as the gun was drawn on it),
        // the drawn gun's barrel and top in U's axes (F) and where they should be (D: the barrel along the gun hand's
        // controller to the front hand's, the top toward the world's up), X = D F^T turns the one onto the other in U's
        // frame, so the new turn is C0 X (the gun is rigid in the target's frame)
        // (not with the gun drawn in John's left hand, GunInGunHand left-handed: its drawn frame is not published there,
        // and a right-handed turn would be the mirror's wrong one: the review)
        const bool stock_here = stock && f.long_gun && f.ik[gj] && gj == 1 && !f.carry_put;  // (not the carried gun's put-away: let go)
        if (!stock_here && holster::stock_calibrate_due())
            log::warn("[body] stock calibration: %s, nothing learned",
                      !stock ? "the physical stock off" : !f.long_gun ? "no long gun in hand (or two-handed long guns off)"
                      : gj != 1 ? "not with the gun in John's left hand" : "the gun hand not tracked");
        holster::stock_calibrate_expire();
        if (stock_here && f.ik[fj]) {  // ("skel stock": the controllers' line)
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = grip_wp[fj][k] - grip_wp[gj][k];
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dl > 1e-4f && std::isfinite(dl)) {
                for (int k = 0; k < 3; ++k) g_stock_line[k].store(d[k] / dl, std::memory_order_relaxed);
                g_stock_span.store(dl, std::memory_order_relaxed);
                g_stock_line_ms.store(log::now_ms(), std::memory_order_release);
            }
        }
        if (stock_here) {
            const float* R = f.ik_r[gj];
            float deg[3];
            holster::stock_turn(deg);
            if (holster::stock_calibrate_due()) {
                float G[9];
                bool g_ok;
                {
                    std::lock_guard lock(g_draw_mutex);
                    g_ok = g_points.gun_frame_ok;
                    std::memcpy(G, g_points.gun_frame_R, sizeof(G));
                }
                float C0[9];
                stock_matrix(deg, C0);
                float U[9];
                mul3(R, C0, U);
                if (g_ok && f.ik[fj]) {
                    float d[3];
                    for (int k = 0; k < 3; ++k) d[k] = grip_wp[fj][k] - grip_wp[gj][k];
                    float out[3];
                    if (stock_solve(U, C0, G, d, out) && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2])) {
                        holster::set_stock_turn(out);
                        std::memcpy(deg, out, sizeof(deg));
                        log::info("[body] stock calibrated: every long gun turned (%.2f %.2f %.2f) deg in the gun hand, its barrel along the "
                                  "stock and its top up",
                                  out[0], out[1], out[2]);
                    } else {
                        log::warn("[body] stock calibration: the hands too close or the gun's frame degenerate, nothing learned");
                    }
                } else {
                    log::warn("[body] stock calibration: %s, nothing learned", g_ok ? "the front hand not tracked" : "no drawn long gun");
                }
            }
            float C[9], Rn[9];
            if (stock_matrix(deg, C)) {
                mul3(R, C, Rn);
                std::memcpy(f.ik_r[gj], Rn, sizeof(Rn));
                for (int k = 0; k < 3; ++k) f.ik_t[gj][k] = grip_wp[gj][k] + Rn[k * 3 + 2] * hc.wrist_offset;
            }
        }
        // [Reload] LeverControl=front: the gun in the front hand while its lever is worked (two-handed; the carry taken only
        // on a full hold, not half way through the blend's rise: the review)
        static bool front_on = false;
        const bool front_want = ((actions::lever_front() && (front_on ? tb > 0.5f : tb > 0.99f)) || hold_front) && !f.carry_put;  // (LeverControl=hold two-handed)
        if (actions::lever_front() && !front_want)
            log::limited("body.front", 10, "[body] lever front: waiting for a full two-handed hold (blend %.3f, ik %d %d)", tb, f.ik[0] ? 1 : 0,
                         f.ik[1] ? 1 : 0);
        front_on = front_want;
        gun_front(f.ik[0] && f.ik[1], front_want, f.ik_r[fj], f.ik_t[fj], f.ik_r[gj], f.ik_t[gj]);
    }
    f.pitch = pitch_deg * 0.0174532925f;
    // the facing lock: the angle about the world's up from the actor's forward (the root's -z) to the camera's
    // (heading h faces (-sin h, -cos h))
    float h = pose::torso_heading_deg() * 0.0174532925f;  // run 9: the horse's while the ride's view is turned
    float cfx = -std::sin(h), cfz = -std::cos(h);
    f.view_h = h;
    {
        float ax = -w[8] * sign, az = -w[10] * sign, al = std::sqrt(ax * ax + az * az);
        if (lock_facing && al > 1e-3f) {
            ax /= al;
            az /= al;
            f.yaw = std::atan2(az * cfx - ax * cfz, ax * cfx + az * cfz);
            f.lock = true;
        }
    }
    // the shift, in the world: back along the root's forward (the model faces the root bone's -z: measured, a +z shift
    // moved the body in front of the anchored camera), plus the head's offset since recentre
    // when the body follows the headset (along the camera's heading: forward (-sin h, -cos h), right (cos h, -sin h))
    float sw[3] = {0, 0, 0};
    float fx = f.lock ? cfx : -w[8] * sign, fz = f.lock ? cfz : -w[10] * sign, fl = std::sqrt(fx * fx + fz * fz);
    if (fl > 1e-3f && back > 0) {
        sw[0] -= fx / fl * back;
        sw[2] -= fz / fl * back;
    }
    float off[3];
    if (follow && camera_lever::head_offset(off)) {
        sw[0] += std::cos(h) * off[0] + std::sin(h) * off[2];
        sw[2] += -std::sin(h) * off[0] + std::cos(h) * off[2];
    }
    for (int k = 0; k < 3; ++k) f.shift_bind[k] = sw[0] * w[k * 4 + 0] + sw[1] * w[k * 4 + 1] + sw[2] * w[k * 4 + 2];
    f.shift = sw[0] != 0 || sw[2] != 0;
    for (int k = 0; k < 3; ++k) f.shift_world[k] = sw[k];
    std::memcpy(f.w_root, w, sizeof(f.w_root));
    if (g_rig.up >= 0) std::memcpy(f.up_world, reinterpret_cast<const float*>(mtx + static_cast<uintptr_t>(g_rig.up) * 0x40 + 0x30), 12);
    int amax = 0;
    for (int b : g_rig.attach) amax = b > amax ? b : amax;
    if (readable(mtx, static_cast<size_t>(amax + 1) * 0x40)) {
        f.attach_pos.reserve(g_rig.attach.size() * 3);
        for (int b : g_rig.attach) {
            const float* m = reinterpret_cast<const float*>(mtx + static_cast<uintptr_t>(b) * 0x40 + 0x30);
            f.attach_pos.insert(f.attach_pos.end(), m, m + 3);
        }
    }
    f.held_fix = g_held_fix_cfg.load(std::memory_order_relaxed);
    if (!seq_copy(g_copy_ring_seq, f.copy, g_copy_ring, sizeof(f.copy)))  // [Hands] DualWieldCopy: the copy's samples
        for (HeldSample& hs : f.copy) hs.valid = false;
    g_skel_game.store(skel, std::memory_order_relaxed);  // the game thread's placement of the second gun
    for (int hb = 0; hb < 2; ++hb) g_att_bone[hb].store(g_rig.att_wrist[hb], std::memory_order_relaxed);
    {  // the draws' first test (most of the world's draws are far from both)
        auto grow = [](float* b, const float* p, float pad) {
            for (int k = 0; k < 3; ++k) {
                b[k] = std::fmin(b[k], p[k] - pad);
                b[3 + k] = std::fmax(b[3 + k], p[k] + pad);
            }
        };
        for (const HeldSample& hs : f.held)
            if (hs.valid) grow(f.held_box, hs.m + 12, 2e-3f);
        for (const HeldSample& hs : f.sec)
            if (hs.valid) grow(f.sec_box, hs.m + 12, 2e-3f);
        for (size_t i = 0; i * 3 + 2 < f.attach_pos.size(); ++i) grow(f.attach_box, &f.attach_pos[i * 3], 0.5f);
    }
    {  // the side the game holds the item in hand on, and the transplant
        const HeldSample* last = newest_held(f.held);
        f.item_side = last ? (last->left ? 0 : 1) : -1;
        // (2026-11-01, GOAL-offhand-changes C10; round 64, the user: drawn by the left hand the lasso should look as drawn by
        // the right, most of the rope in the left hand, the loop in the right) the lasso never transplanted: the game's own
        // hands (its loop in John's right, its coil in his left); the gun hand stays the drawing one (its aim: LassoAim=hand;
        // the review: gun_j 1 aimed it by the idle controller)
        f.xfer = f.gun_j == 0 && f.item_side >= 0 && f.item_side != f.gun_j && f.weapon != 21;
        f.fingers = f.xfer && g_rig.mirror_ok ? g_fingers_cfg.load(std::memory_order_relaxed) : 0;
        // round 13: any second gun's hand too (DualWield's long guns and other sidearms), not only the copy's
        // ([Hands] CarryByForegrip: not the carried long gun's hand, which holds its foregrip, not a grip)
        // (2026-10-28, [Reload] PistolMagazines, r223: the free hand pointing, the magazine out of its index tip) the free hand
        // holding a magazine takes the gun hand's grip too, mirrored: it closes on the magazine's base as on a grip. Not
        // behind [Hands] CopyGrip (the review): that is the copy's option; this hold is the magazine's own (its pose is
        // built for the closed hand), so with PistolMagazines it always closes
        const int mh = round_draw::held_mag_hand();
        const bool mag_grip = mh >= 0 && mh == 1 - f.gun_j;
        f.copy_grip = g_rig.mirror_ok && f.item_side >= 0 && !dual::state().carry &&
                      ((g_copy_grip_cfg.load(std::memory_order_relaxed) && (dual::copy_W() != 0 || dual::secondary_W(nullptr) != 0)) || mag_grip);
    }
    {  // the last frame: the item was in hand, and did any of its draws come?
        static bool was_held = false;
        static uint64_t held_run = 0;  // the frames in hand since the item came into it
        const uint64_t n = g_held_frame_draws.exchange(0, std::memory_order_relaxed);
        if (was_held) {
            g_held_frames.fetch_add(1, std::memory_order_relaxed);
            if (!n) {
                g_held_missed.fetch_add(1, std::memory_order_relaxed);
                g_held_miss_at[held_run < 3 ? held_run : held_run < 10 ? 3 : 4].fetch_add(1, std::memory_order_relaxed);
                for (int i = 0; i < 3; ++i) g_held_miss_last[i].store(g_held_miss_last[i + 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
                g_held_miss_last[3].store(held_run + 1, std::memory_order_relaxed);
            }
            ++held_run;
        } else {
            held_run = 0;
        }
        // (run 12 item 11, cycle FK1: with the fists selected after a put-away the game keeps an item in hand that is never
        // drawn, and every such frame counted as missed: only a gun in hand counts)
        was_held = f.held_any && reload::is_gun(f.weapon);
    }
    {  // run 6 item 1c: every 10 s while an item is in hand, a line for the headset's logs (the user's lag at doors and
       // when dual-wielding could not be reproduced in the simulator): the held draws by pass and the stale ones (made
       // before the frame's first player set, with last frame's correction; pass 0 is the shadow pre-pass, 1 and 3 the
       // eyes'), those of the second gun or the own model, the draws matched to an older sample, frames in hand without a
       // draw. Counts since the last line.
        static double last_ms = 0;
        static uint32_t h0[8] = {}, s0[8] = {}, o0[8] = {}, os0[8] = {}, c0[8] = {}, cs0[8] = {};
        static uint64_t older0 = 0, newest0 = 0, frames0 = 0, missed0 = 0, cp0 = 0, cr0 = 0, cn0 = 0;
        const double now = log::now_ms();
        if (now - last_ms >= 10000.0) {
            last_ms = now;
            uint32_t hb[8], sb[8], ob[8], osb[8], cb[8], csb[8];
            for (int i = 0; i < 8; ++i) {
                cb[i] = g_copy_by_pass[i].load(std::memory_order_relaxed);
                csb[i] = g_copy_stale_by_pass[i].load(std::memory_order_relaxed);
                hb[i] = g_held_by_pass[i].load(std::memory_order_relaxed);
                sb[i] = g_stale_by_pass[i].load(std::memory_order_relaxed);
                ob[i] = g_other_by_pass[i].load(std::memory_order_relaxed);
                osb[i] = g_other_stale_by_pass[i].load(std::memory_order_relaxed);
            }
            const uint64_t older = g_held_older.load(), newest = g_held_newest.load(), frames = g_held_frames.load(), missed = g_held_missed.load();
            const uint64_t cp = g_copy_placements.load(), cr = g_copy_refused.load(), cn = g_copy_nomatch.load();
            if (frames != frames0 && g_follow_auto.load(std::memory_order_relaxed)) {  // [Debug] FollowLog
                std::lock_guard lock(g_draw_mutex);
                if (!g_follow_manual) {  // run 9: not over a test's recording (item1_follow read 0 frames: the re-arm reset it)
                    if (g_follow_n > 30) follow_report(" auto");
                    g_follow_pass = 1;
                    g_follow_n = 0;
                    g_follow_rec = true;
                }
            }
            GunMvStats mv;  // [Hands] GunMotionVectors, since the last line
            {
                std::lock_guard lock(g_draw_mutex);
                mv = g_gun_mv_log;
                g_gun_mv_log = GunMvStats{};
            }
            const float mv_mean = mv.n ? static_cast<float>(mv.sum / static_cast<double>(mv.n)) : 0.0f;
            if (frames != frames0 || ob[1] != o0[1] || cp != cp0 || cr != cr0)
                log::info("[lag] 10 s: held draws by pass 0/1/3 %u/%u/%u, stale %u/%u/%u | the second gun or own model %u/%u/%u, stale %u/%u/%u | "
                          "matched the newest %llu, an older sample %llu | frames in hand %llu, without a draw %llu | the copy: placed %llu, refused "
                          "%llu (why %d), unmatched %llu, draws by pass 0/1/3 %u/%u/%u, stale %u/%u/%u | the motion vectors: the game's last frame "
                          "of the gun off where it was drawn mean %.1f max %.1f mm, drawn from last frame's %llu%s, none %llu, jumps %llu | early draws moved "
                          "by the wrist (StaleGunFix %s) %llu since start",
                          hb[0] - h0[0], hb[1] - h0[1], hb[3] - h0[3], sb[0] - s0[0], sb[1] - s0[1], sb[3] - s0[3], ob[0] - o0[0], ob[1] - o0[1], ob[3] - o0[3],
                          osb[0] - os0[0], osb[1] - os0[1], osb[3] - os0[3], static_cast<unsigned long long>(newest - newest0),
                          static_cast<unsigned long long>(older - older0), static_cast<unsigned long long>(frames - frames0),
                          static_cast<unsigned long long>(missed - missed0), static_cast<unsigned long long>(cp - cp0),
                          static_cast<unsigned long long>(cr - cr0), g_copy_why.load(), static_cast<unsigned long long>(cn - cn0), cb[0] - c0[0],
                          cb[1] - c0[1], cb[3] - c0[3], csb[0] - cs0[0], csb[1] - cs0[1], csb[3] - cs0[3], mv_mean, mv.max,
                          static_cast<unsigned long long>(mv.fixed), g_gun_mv.load(std::memory_order_relaxed) ? "" : " (off: the game's kept)",
                          static_cast<unsigned long long>(mv.none), static_cast<unsigned long long>(mv.cut), g_stale_fix.load() ? "on" : "off",
                          static_cast<unsigned long long>(g_stale_shifted.load()));
            std::memcpy(c0, cb, sizeof(c0));
            std::memcpy(cs0, csb, sizeof(cs0));
            cp0 = cp, cr0 = cr, cn0 = cn;
            std::memcpy(h0, hb, sizeof(h0));
            std::memcpy(s0, sb, sizeof(s0));
            std::memcpy(o0, ob, sizeof(o0));
            std::memcpy(os0, osb, sizeof(os0));
            older0 = older, newest0 = newest, frames0 = frames, missed0 = missed;
        }
    }
    coll_frame(f);  // [Hands] GunCollision (2026-10-29): the guns and arms held apart (the moves kick_frame puts on the kicks)
    kick_frame(f);  // [Hands] GunKick (2026-10-27): the springs to this frame, its kick of each (the draws put it on)
    f.valid = true;
    std::lock_guard lock(g_draw_mutex);
    g_frame = std::move(f);
    ++g_body_frame;
    g_prop_bones.erase(std::remove_if(g_prop_bones.begin(), g_prop_bones.end(),
                                      [](const PropBone& pb) { return g_body_frame - pb.frame > 450; }),
                       g_prop_bones.end());

}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string word, sub;
    in >> word >> sub;
    if (sub == "q" || sub == "f" || sub == "s") {
        // skel q|f <hex addr> [n]: n (<= 64) qwords or floats at an absolute address; skel s <hex addr>: a string.
        // Read only, each read checked with VirtualQuery.
        std::string a_s;
        int n = 8;
        in >> a_s >> n;
        uintptr_t a = static_cast<uintptr_t>(std::strtoull(a_s.c_str(), nullptr, 16));
        if (n < 1 || n > 64) n = 8;
        std::string r;
        char b[64];
        if (sub == "s") {
            for (int i = 0; i < 120; ++i) {
                char c = 0;
                if (!rd(a + i, &c) || !c) break;
                r += (c >= 32 && c < 127) ? c : '?';
            }
            return "\"" + r + "\"";
        }
        for (int i = 0; i < n; ++i) {
            if (sub == "q") {
                uint64_t v = 0;
                if (!rd(a + i * 8, &v)) return r + " (unreadable)";
                std::snprintf(b, sizeof(b), "%s%llx", i ? " " : "", static_cast<unsigned long long>(v));
            } else {
                float v = 0;
                if (!rd(a + i * 4, &v)) return r + " (unreadable)";
                std::snprintf(b, sizeof(b), "%s%.4f", i ? " " : "", v);
            }
            r += b;
        }
        return r;
    }
    if (sub == "vq" || sub == "vqfast") {
        // skel vq: the checked reads' VirtualQuery per scene frame since the last "skel vq" (and the slowest call); skel
        // vqfast on|off: their fast path, the working set's page check inside RDR.exe's image (image_mem.h; the session
        // only, on at start)
        std::string v;
        in >> v;
        if (sub == "vqfast" && (v == "on" || v == "off")) image_mem::set_fast(v == "on");
        static image_mem::Snap s0{};
        static uint64_t n0 = 0;
        image_mem::Snap s1{};
        image_mem::snap(&s1);
        uint64_t us = 0, n1 = 0;
        camera_lever::scene_time(&us, &n1);
        const std::string r = image_mem::format(s0, s1, n1 - n0, 2);
        s0 = s1, n0 = n1;
        return r;
    }
    if (sub == "bones") {
        // skel bones <file>: index, name and parent index of each of the player's bones
        std::string file, out;
        in >> file;
        if (file.empty()) return "ERROR usage: skel bones <file>";
        dump_bones(file, &out);
        return out;
    }
    if (sub == "ik") {
        std::lock_guard lock(g_draw_mutex);
        std::string r;
        char b[300];
        for (int h = 0; h < 2; ++h) {
            const float* d = g_ik_diag[h];
            std::snprintf(b, sizeof(b), "%s S (%.2f %.2f %.2f) E (%.2f %.2f %.2f) W (%.2f %.2f %.2f) T (%.2f %.2f %.2f) l1 %.2f l2 %.2f d %.2f drawn W (%.2f %.2f %.2f) | ",
                          h ? "R" : "L", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[16], d[17], d[18]);
            r += b;
        }
        {
            float nr = 0, fr = 0;
            camera_lever::scene_clip(&nr, &fr);
            std::snprintf(b, sizeof(b), "near %.3f far %.1f | ", nr, fr);
            r += b;
        }
        std::snprintf(b, sizeof(b), "torso shift %.3f (max %.3f) | ", g_torso_diag[0], g_torso_diag[1]);
        r += b;
        g_torso_diag[1] = 0;
        const float* c = g_cam_diag;
        std::snprintf(b, sizeof(b), "cam (%.2f %.2f %.2f) head (%.2f %.2f %.2f) root (%.2f %.2f %.2f) R target (%.2f %.2f %.2f)", c[0], c[1], c[2],
                      c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11]);
        r += b;
        return r;
    }
    if (sub == "gun") {  // skel gun [gunhand on|off | fingers swap|open|off | pin on|off] (the session only)
        std::string v, x;
        in >> v >> x;
        if (v == "gunhand") g_gun_hand_cfg = x == "on";
        if (v == "fingers") g_fingers_cfg = x == "off" ? 0 : x == "open" ? 2 : 1;
        if (v == "pin") g_pin_cfg = x == "on";
        if (v == "mirror") g_mirror_cfg = x == "on";
        if (v == "twist") {
            if (x == "on" || x == "off") g_twist_cfg = x == "on";
            char tb[200];
            std::snprintf(tb, sizeof(tb), "arm twist %s: the forearm's roll correction L %.1f (%.1f .. %.1f) R %.1f (%.1f .. %.1f) deg, roll bone %llu",
                          g_twist_cfg.load() ? "on" : "off", g_fore_roll[0][0], g_fore_roll[0][1], g_fore_roll[0][2], g_fore_roll[1][0],
                          g_fore_roll[1][1], g_fore_roll[1][2], static_cast<unsigned long long>(g_twists.load()));
            for (auto& fr : g_fore_roll) fr[1] = 1e9f, fr[2] = -1e9f;
            return tb;
        }
        int gun = 1, side = -1, ctrl0 = 0;
        bool xf = false;
        {
            std::lock_guard lock(g_draw_mutex);
            gun = g_frame.gun_j;
            side = g_frame.item_side;
            xf = g_item_xfer;
            ctrl0 = g_frame.ctrl[0];
        }
        char b[400];
        std::snprintf(b, sizeof(b),
                      "gun hand %s (John's left follows controller %d), the game's item in its %s hand, transplant %s (made %llu, prop draws %llu, pin "
                      "shift %.3f m at the %s; knuckles %.4f m from the mirrored hold, wrist %.4f) | gunhand %d fingers %d pin %d mirror %d | mirror plane n (%.3f %.3f %.3f) "
                      "c %.3f off %.4f m: %s",
                      gun ? "right" : "left", ctrl0, side == 0 ? "left" : side == 1 ? "right" : "no", xf ? "on" : "off",
                      static_cast<unsigned long long>(g_xfers.load()), static_cast<unsigned long long>(g_xfer_draws.load()), g_xfer_shift,
                      g_xfer_pin_mode == 1   ? "muzzle"
                      : g_xfer_pin_mode == 2 ? "fist"
                      : g_xfer_pin_mode == 3 ? "attachment"
                      : g_xfer_pin_mode == 4 ? "mirror"
                                             : "-",
                      g_xfer_knuckle_err, g_xfer_grip_err, g_gun_hand_cfg.load() ? 1 : 0, g_fingers_cfg.load(), g_pin_cfg.load() ? 1 : 0, g_mirror_cfg.load() ? 1 : 0,
                      g_rig.mir_n[0], g_rig.mir_n[1], g_rig.mir_n[2], g_rig.mir_c, g_rig.mir_res,
                      g_rig.mirror_ok ? "the finger mirror on" : "the finger mirror off");
        return b;
    }
    if (sub == "grips" && (line.find(" steadyring on") != std::string::npos || line.find(" steadyring off") != std::string::npos)) {
        // skel grips steadyring on|off: SteadyRing (the session); the samples counted afresh
        g_steady_grip = line.find(" steadyring on") != std::string::npos;
        std::memset(g_grip_n, 0, sizeof(g_grip_n));
        return std::string("the learned grips ") + (g_steady_grip.load() ? "steadied (the mean of 30, then kept)" : "re-learned each frame");
    }
    if (sub == "stock") {  // skel stock: the controllers' line while the physical stock holds a long gun (read only, a test)
        const double ms = g_stock_line_ms.load(std::memory_order_acquire);
        if (ms <= 0.0) return "stock line: none (the physical stock has not held a long gun)";
        char b[200];
        std::snprintf(b, sizeof(b), "stock line (%.4f %.4f %.4f), the controllers %.3f m apart, %.0f ms ago", g_stock_line[0].load(std::memory_order_relaxed),
                      g_stock_line[1].load(std::memory_order_relaxed), g_stock_line[2].load(std::memory_order_relaxed), g_stock_span.load(std::memory_order_relaxed),
                      log::now_ms() - ms);
        return b;
    }
    if (sub == "two" && line.find(" sawed ") != std::string::npos) {  // skel two sawed on|off: SawedOffGrip (the session only)
        g_sawed_grip = line.find(" sawed on") != std::string::npos;
        return std::string("the Sawed-off's own grip (the Double-barrel's), a borrowed grip's pose fixed, the hold short of the muzzle ") +
               (g_sawed_grip.load() ? "on" : "off");
    }
    if (sub == "two" && line.find(" snapdrawn ") != std::string::npos) {  // skel two snapdrawn on|off: SnapToDrawnGun (the session only)
        g_snap_drawn_cfg = line.find(" snapdrawn on") != std::string::npos;
        return std::string("the snap from the gun as drawn with FixedGunGrip ") + (g_snap_drawn_cfg.load() ? "on" : "off") + " (frames " +
               std::to_string(g_snap_drawn_frames.load()) + ")";
    }
    if (sub == "two" && line.find(" clipshared ") != std::string::npos) {  // skel two clipshared on|off: the old shared hold (a test)
        g_clip_shared_test = line.find(" clipshared on") != std::string::npos;
        return std::string("the snap's fire-clip hold ") + (g_clip_shared_test.load() ? "shared by both sets (the old fault, a test)" : "per set");
    }
    if (sub == "two" && line.find(" steer ") != std::string::npos) {  // skel two steer on|off: TwoHandedSteer (the session only)
        g_two_steer_cfg = line.find(" steer on") != std::string::npos;
        return std::string("two-handed: the front hand ") + (g_two_steer_cfg.load() ? "turns the gun" : "drawn on the gun only");
    }
    if (sub == "two" && line.find(" steady ") != std::string::npos) {  // skel two steady on|off: TwoHandedSteady (the session only)
        g_same_frame_cfg = line.find(" steady on") != std::string::npos;
        return std::string("two-handed steady (the same frame's barrel, the grip kept while held) ") + (g_same_frame_cfg.load() ? "on" : "off");
    }
    if (sub == "two") {  // skel two: the game's left wrist and palm in the held gun's frame (x right, y up, z back)
        TwoDiag t;
        GripRel gr[2];
        int pose = 0, src = -1;
        float miss = -1, gO[9] = {};
        {
            std::lock_guard lock(g_draw_mutex);
            t = g_two_diag_m;
            const int w = g_frame.weapon;
            if (w >= 0 && w < kGripW) {
                gr[0] = g_grips[w][0];
                gr[1] = g_grips[w][1];
            }
            std::memcpy(gO, g_grip_cur.O, sizeof(gO));
            src = g_grip_src;
            pose = g_grip_pose;
            miss = g_snap_miss;
        }
        char b3[260];
        {
            const int w = g_frame.weapon;
            const int fp = w >= 0 && w < kGripW ? g_front_pose[w].load() : 0;
            const uint32_t seen = w >= 0 && w < kGripW ? g_front_seen[w].load() : 0;
            std::snprintf(b3, sizeof(b3), " | front hand pose %s (seen: lowered %d fingers %d, aiming %d fingers %d), fixed-pose finger frames %llu, grip now (%.3f %.3f %.3f)",
                          fp == 1 ? "lowered" : fp == 2 ? "aiming" : "automatic", seen & 1 ? 1 : 0, seen & 4 ? 1 : 0, seen & 2 ? 1 : 0, seen & 8 ? 1 : 0,
                          static_cast<unsigned long long>(g_front_finger_frames.load()), g_grip_cur.p[0], g_grip_cur.p[1], g_grip_cur.p[2]);
        }
        char b2[520];
        std::snprintf(b2, sizeof(b2), " || learned for weapon %d: lowered %d (%.3f %.3f %.3f) aiming %d (%.3f %.3f %.3f), pose now %s, grip from %s, "
                      "learns %llu, snaps %llu, the drawn front wrist %.3f m from the snap | grip turn %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                      g_frame.weapon, gr[0].valid ? 1 : 0, gr[0].p[0], gr[0].p[1], gr[0].p[2], gr[1].valid ? 1 : 0, gr[1].p[0], gr[1].p[1], gr[1].p[2],
                      pose ? "aiming" : "lowered", src == 0 ? "the weapon" : src == 1 ? "the template" : src == 2 ? "the built-in" : src == 3 ? "the Double-barrel's" : "none",
                      static_cast<unsigned long long>(g_grip_learns.load()), static_cast<unsigned long long>(g_snaps.load()), miss, gO[0], gO[1],
                      gO[2], gO[3], gO[4], gO[5], gO[6], gO[7], gO[8]);
        log::info("[body] skel two%s", b2);
        char b[600];
        std::snprintf(b, sizeof(b),
                      "two (frame %llu, aiming %d, placed %d): game gun: wrist (%.3f %.3f %.3f) palm (%.3f %.3f %.3f) | placed gun: wrist (%.3f %.3f %.3f) "
                      "palm (%.3f %.3f %.3f) | IKOffset (%.3f %.3f %.3f) IKOffsetHold (%.3f %.3f %.3f)",
                      static_cast<unsigned long long>(t.frame), t.aiming ? 1 : 0, t.placed ? 1 : 0, t.wrist_game[0], t.wrist_game[1], t.wrist_game[2],
                      t.palm_game[0], t.palm_game[1], t.palm_game[2], t.wrist_placed[0], t.wrist_placed[1], t.wrist_placed[2], t.palm_placed[0],
                      t.palm_placed[1], t.palm_placed[2], t.ik[0], t.ik[1], t.ik[2], t.ik_hold[0], t.ik_hold[1], t.ik_hold[2]);
        return std::string(b) + b2 + b3;
    }
    if (sub == "frontpose") {  // skel frontpose auto|lowered|aiming: the gun in hand's FrontHandPose (the session only)
        std::string v;
        in >> v;
        const int w = g_frame.weapon;
        if (w < 0 || w >= kGripW) return "frontpose: no gun in hand";
        set_front_pose(w, v == "lowered" ? 1 : v == "aiming" ? 2 : 0, false);
        return std::string("frontpose: weapon ") + std::to_string(w) + " " + (front_pose(w) == 1 ? "lowered" : front_pose(w) == 2 ? "aiming" : "automatic");
    }
    if (sub == "armmv") {  // skel armmv [on|off] [reset]: [Body] ArmMotionVectors (the session) and its counters
        std::string x, y;
        in >> x >> y;
        if (x == "on" || x == "off") g_arm_mv = x == "on";
        char b[280];
        std::lock_guard lock(g_draw_mutex);
        const ArmMvStats& m = g_arm_mv_cmd;
        std::snprintf(b, sizeof(b),
                      "armmv %d: the right wrist's previous half off where it was drawn mean %.1f max %.1f mm (%llu draws), drawn from last "
                      "frame's %llu, none %llu, jumps %llu",
                      g_arm_mv.load() ? 1 : 0, m.n ? static_cast<float>(m.sum / static_cast<double>(m.n)) : 0.0f, m.max, static_cast<unsigned long long>(m.n),
                      static_cast<unsigned long long>(m.fixed), static_cast<unsigned long long>(m.none), static_cast<unsigned long long>(m.cut));
        if (x == "reset" || y == "reset") g_arm_mv_cmd = ArmMvStats{};
        return b;
    }
    if (sub == "collide") {
        // skel collide [on|off] [measure on|off] [reset] | skel collide guns: [Hands] GunCollision (2026-10-29; the session, never
        // the ini), its counts and the closest each kind of pair came since the reset (as held, as moved, as drawn: the drawn
        // needs "skel recoil trace on"); "measure on" takes and measures the bodies with the option off too (nothing moved);
        // "guns": the guns as the collisions took them (world: the origin, the axes, the body's ends, the hammer's spur)
        std::string x, y;
        in >> x >> y;
        if (x == "on" || x == "off") set_gun_collision(x == "on", false);
        if (x == "measure" && (y == "on" || y == "off")) g_coll_measure = y == "on";
        if (x == "nosnap" && (y == "on" || y == "off")) g_coll_nosnap = y == "on";
        if (x == "handmove" && (y == "on" || y == "off")) g_coll_handmove = y == "on";  // (round 43's control: "off" slides the gun off the hand)
        if (x == "guns") {
            CollGuns cg;
            if (!collision_guns(&cg)) return "collide guns: none (the option off, or none within 250 ms)";
            std::string o = "collide guns: frame " + std::to_string(cg.frame);
            char b[520];
            static const char* const kName[3] = {"in hand", "second", "carried"};
            for (int g = 0; g < 3; ++g) {
                const CollGun& c = cg.gun[g];
                if (!c.ok) continue;
                float pa[3], pb[3], sp[3];
                for (int k = 0; k < 3; ++k) {
                    pa[k] = c.o[k] + c.R[k * 3] * c.a[0] + c.R[k * 3 + 1] * c.a[1] + c.R[k * 3 + 2] * c.a[2];
                    pb[k] = c.o[k] + c.R[k * 3] * c.b[0] + c.R[k * 3 + 1] * c.b[1] + c.R[k * 3 + 2] * c.b[2];
                    sp[k] = c.o[k] + c.R[k * 3] * c.spur[0] + c.R[k * 3 + 1] * c.spur[1] + c.R[k * 3 + 2] * c.spur[2];
                }
                std::snprintf(b, sizeof(b),
                              " | %s: weapon %d hand %d o (%.4f %.4f %.4f) x (%.4f %.4f %.4f) y (%.4f %.4f %.4f) z (%.4f %.4f %.4f) body a (%.4f %.4f %.4f) "
                              "b (%.4f %.4f %.4f) r %.3f spur %d (%.4f %.4f %.4f)",
                              kName[g], c.weapon, c.john, c.o[0], c.o[1], c.o[2], c.R[0], c.R[3], c.R[6], c.R[1], c.R[4], c.R[7], c.R[2], c.R[5], c.R[8], pa[0],
                              pa[1], pa[2], pb[0], pb[1], pb[2], c.r, c.spur_ok ? 1 : 0, sp[0], sp[1], sp[2]);
                o += b;
            }
            return o;
        }
        CollStats st;
        {
            std::lock_guard lock(g_coll_mutex);
            if (x == "reset" || y == "reset") {
                CollStats& s = g_coll_st;
                for (int c = 0; c < 4; ++c) s.held_min[c] = s.moved_min[c] = s.drawn_min[c] = 1e9f, s.held_n[c] = s.drawn_n[c] = 0;
                for (float& m : s.max_push) m = 0.0f;
                s.frames = s.moved = s.begun = s.sounds = s.releases = s.unresolved = s.hand_frames = s.drawn_frames = 0;
                s.pulses[0] = s.pulses[1] = 0;
                s.us_sum = 0.0, s.us_max = 0.0f, s.us_n = 0;
                std::memset(s.hand_in, 0, sizeof(s.hand_in));
                log::reset_limited("body.coll.");  // (its contact lines counted again: a test's)
            }
            st = g_coll_st;
        }
        auto mm = [](float v) { return v < 1e8f ? v * 1000.0f : 9999.0f; };  // (9999: none measured)
        char b[2600];
        std::snprintf(b, sizeof(b),
                      "collide %d (measure %d, nosnap %d, handmove %d) | hands: holding %d%d loading %d%d grip %.2f %.2f | frames %llu, the guns moved %llu, a hand moved %llu | contacts now %d of %d pairs:%s | begun %llu, pulses "
                      "left %llu right %llu, sounds %llu, let go %llu, unresolved %llu | bodies %d%d%d%d%d weapons %d %d %d | push mm: gun in hand (%.1f %.1f "
                      "%.1f) second (%.1f %.1f %.1f) left hand (%.1f %.1f %.1f) right hand (%.1f %.1f %.1f) | max push mm %.1f %.1f %.1f %.1f %.1f | "
                      "held min mm: guns %.1f hand %.1f forearm %.1f own %.1f (frames %llu %llu %llu %llu) | moved min mm: guns %.1f hand %.1f forearm "
                      "%.1f own %.1f | drawn min mm: guns %.1f hand %.1f forearm %.1f own %.1f (frames %llu %llu %llu %llu; %llu drawn frames) | now "
                      "held mm: guns %.1f hand %.1f forearm %.1f own %.1f drawn mm: guns %.1f hand %.1f forearm %.1f own %.1f | cost us mean %.1f max %.1f "
                      "(%llu frames) | drawn barrels: gun in hand %d (%.4f %.4f %.4f) second %d (%.4f %.4f %.4f) | the other hand in mm (the most): gun "
                      "in hand body %.1f grip %.1f second body %.1f grip %.1f",
                      g_coll_cfg.load() ? 1 : 0, g_coll_measure.load() ? 1 : 0, g_coll_nosnap.load() ? 1 : 0, g_coll_handmove.load() ? 1 : 0, st.holding[0] ? 1 : 0, st.holding[1] ? 1 : 0,
                      st.loading[0] ? 1 : 0, st.loading[1] ? 1 : 0, st.grip[0], st.grip[1], static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.moved),
                      static_cast<unsigned long long>(st.hand_frames), st.contacts, st.pairs, st.list, static_cast<unsigned long long>(st.begun),
                      static_cast<unsigned long long>(st.pulses[0]), static_cast<unsigned long long>(st.pulses[1]), static_cast<unsigned long long>(st.sounds),
                      static_cast<unsigned long long>(st.releases), static_cast<unsigned long long>(st.unresolved), st.body_ok[0] ? 1 : 0, st.body_ok[1] ? 1 : 0,
                      st.body_ok[2] ? 1 : 0, st.body_ok[3] ? 1 : 0, st.body_ok[4] ? 1 : 0, st.weapon[0], st.weapon[1], st.weapon[2], st.push[0][0] * 1000.0f,
                      st.push[0][1] * 1000.0f, st.push[0][2] * 1000.0f, st.push[1][0] * 1000.0f, st.push[1][1] * 1000.0f, st.push[1][2] * 1000.0f,
                      st.push[3][0] * 1000.0f, st.push[3][1] * 1000.0f, st.push[3][2] * 1000.0f, st.push[4][0] * 1000.0f, st.push[4][1] * 1000.0f,
                      st.push[4][2] * 1000.0f, st.max_push[0] * 1000.0f, st.max_push[1] * 1000.0f, st.max_push[2] * 1000.0f, st.max_push[3] * 1000.0f,
                      st.max_push[4] * 1000.0f, mm(st.held_min[0]), mm(st.held_min[1]), mm(st.held_min[2]), mm(st.held_min[3]),
                      static_cast<unsigned long long>(st.held_n[0]), static_cast<unsigned long long>(st.held_n[1]), static_cast<unsigned long long>(st.held_n[2]),
                      static_cast<unsigned long long>(st.held_n[3]), mm(st.moved_min[0]), mm(st.moved_min[1]), mm(st.moved_min[2]), mm(st.moved_min[3]),
                      mm(st.drawn_min[0]), mm(st.drawn_min[1]), mm(st.drawn_min[2]), mm(st.drawn_min[3]), static_cast<unsigned long long>(st.drawn_n[0]),
                      static_cast<unsigned long long>(st.drawn_n[1]), static_cast<unsigned long long>(st.drawn_n[2]), static_cast<unsigned long long>(st.drawn_n[3]),
                      static_cast<unsigned long long>(st.drawn_frames), mm(st.held_now[0]), mm(st.held_now[1]), mm(st.held_now[2]), mm(st.held_now[3]),
                      mm(st.drawn_now[0]), mm(st.drawn_now[1]), mm(st.drawn_now[2]), mm(st.drawn_now[3]),
                      st.us_n ? static_cast<float>(st.us_sum / static_cast<double>(st.us_n)) : 0.0f, st.us_max, static_cast<unsigned long long>(st.us_n),
                      st.barrel_ok[0] ? 1 : 0, st.barrel[0][0], st.barrel[0][1], st.barrel[0][2], st.barrel_ok[1] ? 1 : 0, st.barrel[1][0], st.barrel[1][1],
                      st.barrel[1][2], st.hand_in[0][0] * 1000.0f, st.hand_in[0][1] * 1000.0f, st.hand_in[1][0] * 1000.0f, st.hand_in[1][1] * 1000.0f);
        return b;
    }
    if (sub == "recoil") {
        // skel recoil [<0-2>] [reset]: [Hands] GunKick (2026-10-27; the strength for the session, never the ini), the springs and
        // the counts | skel recoil trace on|off (the drawn gun of each spring, each frame) | skel recoil trace <0|1>: around that
        // spring's last shot, the drawn gun's turn up against its hand's target, its slide back, the lift of its foregrip, the aim's
        // move, the motion vectors (each frame's previous half against last frame's drawn) and the early draws against the main
        // ones | skel recoil pulse <0|1> <ms>: a test impulse every ms (0 off; the session)
        std::string x, y, z;
        in >> x >> y >> z;
        if (x == "trace" && (y == "on" || y == "off")) {
            {
                std::lock_guard lock(g_draw_mutex);
                g_kick_trace_n[0] = g_kick_trace_n[1] = 0;
                g_kick_trace_frame[0] = g_kick_trace_frame[1] = ~0ull;
            }
            g_kick_trace_on = y == "on";
            return std::string("recoil trace ") + (y == "on" ? "on" : "off");
        }
        if (x == "trace" && (y == "0" || y == "1")) {
            const int r = y == "1" ? 1 : 0;
            std::vector<KickTrace> v;
            {
                std::lock_guard lock(g_draw_mutex);
                const uint64_t n = g_kick_trace_n[r];
                for (uint64_t i = n > static_cast<uint64_t>(kKickTraceN) ? n - kKickTraceN : 0; i < n; ++i) v.push_back(g_kick_trace[r][i % kKickTraceN]);
            }
            if (v.empty()) return "recoil trace " + y + ": no frames (skel recoil trace on first)";
            const double S = v.back().shot_ms, now = log::now_ms();
            if (S < 0.0) return "recoil trace " + y + ": no shot yet (" + std::to_string(v.size()) + " frames)";
            float b0[3] = {}, u0[3] = {}, a0[3] = {}, p0[3] = {}, fd = 0.0f;
            int nb = 0, na = 0, nfd = 0;
            for (const KickTrace& e : v)
                if (e.ms < S && e.ms > S - 400.0) {
                    for (int k = 0; k < 3; ++k) b0[k] += e.bt[k], u0[k] += e.ut[k], p0[k] += e.pt[k];
                    ++nb;
                    if (e.aim_ok) {
                        for (int k = 0; k < 3; ++k) a0[k] += e.aim[k];
                        ++na;
                    }
                    if (e.fore_d > 0.0f) fd += e.fore_d, ++nfd;
                }
            if (!nb) return "recoil trace " + y + ": no frame in the 400 ms before the shot";
            for (int k = 0; k < 3; ++k) p0[k] /= static_cast<float>(nb);
            norm(b0);
            norm(u0);
            if (na) norm(a0);
            // the foregrip's lift: a point that far ahead along the barrel (the published foregrip's distance, else 0.45 m)
            const float dfore = nfd ? fd / static_cast<float>(nfd) : 0.45f;
            auto ang = [](const float* p, const float* q) {
                float pl = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if (pl < 1e-6f || ql < 1e-6f) return -1.0f;
                return std::acos(std::fmin(1.0f, std::fmax(-1.0f, (p[0] * q[0] + p[1] * q[1] + p[2] * q[2]) / (pl * ql)))) * 57.29578f;
            };
            char b[200];
            std::string samples;
            float peak = -1e9f, peak_t = 0.0f, peak_s = 0.0f, at300 = 0.0f, at300_t = -1.0f, last = 0.0f, last_t = 0.0f, aim_max = 0.0f;
            float slide_max = 0.0f, lift_max = 0.0f, mv_deg = 0.0f, mv_mm = 0.0f, move_deg = 0.0f, early_deg = 0.0f, early_mm = 0.0f;
            float d_turn = 0.0f, d_slide = 0.0f, d_lift = 0.0f;  // the kick as delivered (the drawn root against itself before it)
            int n_after = 0, n_aim = 0, n_mv = 0, n_early = 0, n_skin = 0, n_d = 0;
            const KickTrace* pe = nullptr;
            for (const KickTrace& e : v) {
                if (e.ms < S - 100.0 || e.ms > S + 700.0) {
                    pe = &e;
                    continue;
                }
                // the motion vectors: this frame's previous half where last frame's was drawn (consecutive frames)
                if (pe && pe->frame + 1 == e.frame) {
                    const float zc[3] = {pe->cur[2], pe->cur[6], pe->cur[10]}, zp[3] = {e.prev[2], e.prev[6], e.prev[10]};
                    const float yc[3] = {pe->cur[1], pe->cur[5], pe->cur[9]}, yp[3] = {e.prev[1], e.prev[5], e.prev[9]};
                    const float zn[3] = {e.cur[2], e.cur[6], e.cur[10]};
                    const float dd[3] = {e.prev[3] - pe->cur[3], e.prev[7] - pe->cur[7], e.prev[11] - pe->cur[11]};
                    mv_deg = std::fmax(mv_deg, std::fmax(ang(zc, zp), ang(yc, yp)));
                    mv_mm = std::fmax(mv_mm, 1000.0f * std::sqrt(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2]));
                    move_deg = std::fmax(move_deg, ang(zc, zn));  // the drawn motion itself (frame to frame)
                    ++n_mv;
                }
                pe = &e;
                if (e.early_ok) {  // the frame's early draw (the pre-pass) against its main draw
                    const float dd[3] = {e.early_pt[0] - e.pt[0], e.early_pt[1] - e.pt[1], e.early_pt[2] - e.pt[2]};
                    early_deg = std::fmax(early_deg, ang(e.early_bt, e.bt));
                    early_mm = std::fmax(early_mm, 1000.0f * std::sqrt(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2]));
                    ++n_early;
                }
                if (e.ms < S) continue;
                const float t = static_cast<float>(e.ms - S);
                const float cb = e.bt[0] * b0[0] + e.bt[1] * b0[1] + e.bt[2] * b0[2], ub = e.bt[0] * u0[0] + e.bt[1] * u0[1] + e.bt[2] * u0[2];
                const float pitch = std::atan2(ub, cb) * 57.29578f;  // the barrel's turn toward the gun's top (+: muzzle up)
                float aim = -1.0f;
                if (na && e.aim_ok) {
                    aim = ang(e.aim, a0);
                    aim_max = std::fmax(aim_max, aim);
                    ++n_aim;
                }
                // the slide back (along the barrel before the shot, toward the shooter) and the foregrip's lift (square to it,
                // toward the gun's top: the point dfore ahead of the root, now against before)
                const float dp[3] = {e.pt[0] - p0[0], e.pt[1] - p0[1], e.pt[2] - p0[2]};
                const float slide = -(dp[0] * b0[0] + dp[1] * b0[1] + dp[2] * b0[2]);
                float lift = 0.0f;
                for (int k = 0; k < 3; ++k) lift += (dp[k] + dfore * (e.bt[k] - b0[k])) * u0[k];
                if (!std::isfinite(pitch)) continue;
                slide_max = std::fmax(slide_max, slide);
                lift_max = std::fmax(lift_max, std::fabs(lift));
                if (e.skinned) ++n_skin;
                const float dl = e.d_ok ? -dfore * e.d_r1z + e.d_r1t : 0.0f;
                if (e.d_ok) {
                    d_turn = std::fmax(d_turn, e.d_turn);
                    d_slide = std::fmax(d_slide, e.d_slide);
                    d_lift = std::fmax(d_lift, std::fabs(dl));
                    ++n_d;
                }
                if (pitch > peak) peak = pitch, peak_t = t, peak_s = e.s;
                if (at300_t < 0.0f && t >= 300.0f) at300 = pitch, at300_t = t;
                last = pitch, last_t = t;
                ++n_after;
                if (n_after <= 64) {
                    std::snprintf(b, sizeof(b), " %.0f:%+.2f:%.2f:%.1f:%.1f:%.1f", t, pitch, aim, slide * 1000.0f, lift * 1000.0f,
                                  e.d_ok ? e.d_slide * 1000.0f : -1.0f);
                    samples += b;
                }
            }
            // (the drawn barrel against the aim's before the shot: the root's -z is the barrel, as BarrelAim's W +0x80 is)
            const float ab = na ? ang(b0, a0) : -1.0f;
            char h[1100];
            std::snprintf(h, sizeof(h),
                          "recoil trace %d: shot %.0f ms ago, %d frames before, %d after (%d skinned) | peak %+.2f deg at %.0f ms (s %.2f), at 300 ms "
                          "%+.2f (%.0f ms), at the end %+.2f (%.0f ms) | the aim moved at most %.2f deg (%d frames); the drawn barrel off the aim's "
                          "before %.2f deg | slide back at most %.1f mm, the foregrip %.2f m ahead lifted at most %.1f mm | delivered (the drawn root "
                          "against itself before the kick): turn at most %.2f deg, slide at most %.1f mm, the foregrip's lift at most %.1f mm (%d "
                          "frames) | motion vectors: the previous half off last frame's drawn at most %.2f deg %.1f mm (%d pairs; the drawn move at "
                          "most %.2f deg a frame) | early draws %d, off the main draw at most %.2f deg %.1f mm | t:turn:aim:slide:lift:delivered",
                          r, now - S, nb, n_after, n_skin, n_after ? peak : 0.0f, peak_t, peak_s, at300, at300_t, last, last_t, aim_max, n_aim, ab,
                          slide_max * 1000.0f, dfore, lift_max * 1000.0f, d_turn, d_slide * 1000.0f, d_lift * 1000.0f, n_d, mv_deg, mv_mm, n_mv,
                          move_deg, n_early, early_deg, early_mm);
            return h + samples;
        }
        if (x == "pulse" && (y == "0" || y == "1")) {
            const int ms = std::atoi(z.c_str());
            g_kick_pulse_ms[y == "1" ? 1 : 0] = ms > 0 ? std::min(ms, 10000) : 0;
            return "recoil pulse " + y + " " + std::to_string(g_kick_pulse_ms[y == "1" ? 1 : 0].load()) + " ms";
        }
        if (!x.empty() && x != "reset") {
            char* end = nullptr;
            const float v = std::strtof(x.c_str(), &end);
            if (end && end != x.c_str() && !*end) set_gun_kick(v, false);
            else return "skel recoil [<0-2>] [reset] | trace on|off | trace <0|1> | pulse <0|1> <ms>";
        }
        KickSpring sp[2];
        {
            std::lock_guard lock(g_kick_mutex);
            sp[0] = g_kick[0];
            sp[1] = g_kick[1];
            if (x == "reset" || y == "reset") g_kick[0].peak_s = g_kick[1].peak_s = 0.0f;
        }
        const double now = log::now_ms();
        std::string o;
        char b[480];
        std::snprintf(b, sizeof(b), "recoil %.2f (GunKick; two-handed: the turn x%.2f, the foregrip's lift at most %.0f mm, the slide kept), trace %d",
                      g_recoil.load(), kKickTwoTurn, kKickForeLift * 1000.0f, g_kick_trace_on.load() ? 1 : 0);
        o = b;
        for (int r = 0; r < 2; ++r) {
            std::snprintf(b, sizeof(b),
                          " | %s: s %.3f v %.2f (peak s %.2f), weapon %d (%.1f deg, %.0f mm), shots %llu, pulses %llu (every %d ms), the last %.0f ms ago, "
                          "now %.2f deg %.1f mm, gun switches %llu | draws kicked %llu, early %llu, no IK %llu, non-finite %llu, sets under their "
                          "record %llu",
                          r ? "the second gun or the copy" : "the gun in hand", sp[r].s, sp[r].v, sp[r].peak_s, sp[r].weapon, sp[r].deg, sp[r].back * 1000.0f,
                          static_cast<unsigned long long>(sp[r].shots), static_cast<unsigned long long>(sp[r].pulses), g_kick_pulse_ms[r].load(),
                          sp[r].shot_ms > 0.0 ? now - sp[r].shot_ms : -1.0, sp[r].now_turn, sp[r].now_back * 1000.0f,
                          static_cast<unsigned long long>(sp[r].switches), static_cast<unsigned long long>(g_kick_draws[r].load()),
                          static_cast<unsigned long long>(g_kick_early[r].load()), static_cast<unsigned long long>(g_kick_noik[r].load()),
                          static_cast<unsigned long long>(g_kick_bad[r].load()), static_cast<unsigned long long>(g_kick_recm[r].load()));
            o += b;
        }
        if (x == "reset" || y == "reset")
            for (int r = 0; r < 2; ++r) g_kick_draws[r] = g_kick_early[r] = g_kick_noik[r] = g_kick_bad[r] = g_kick_recm[r] = 0;
        return o;
    }
    if (sub == "gunmv") {  // skel gunmv [on|off] [reset]: [Hands] GunMotionVectors (the session) and its counters
        std::string x, y;
        in >> x >> y;
        if (x == "on" || x == "off") g_gun_mv = x == "on";
        char b[320];
        std::lock_guard lock(g_draw_mutex);
        const GunMvStats& m = g_gun_mv_cmd;
        std::snprintf(b, sizeof(b),
                      "gunmv %d: the game's last frame of the gun off where it was drawn mean %.1f max %.1f mm (%llu draws), drawn from last "
                      "frame's %llu, none %llu, jumps %llu",
                      g_gun_mv.load() ? 1 : 0, m.n ? static_cast<float>(m.sum / static_cast<double>(m.n)) : 0.0f, m.max, static_cast<unsigned long long>(m.n),
                      static_cast<unsigned long long>(m.fixed), static_cast<unsigned long long>(m.none), static_cast<unsigned long long>(m.cut));
        if (x == "reset" || y == "reset") g_gun_mv_cmd = GunMvStats{};
        return b;
    }
    if (sub == "lag") {  // skel lag [two|ik] [reset]: the held gun against its bones; two-handed; the wrists and near misses
        std::string x, y;
        in >> x >> y;
        const bool reset = x == "reset" || y == "reset";
        char b[480];
        std::lock_guard lock(g_draw_mutex);
        if (x == "two") {
            std::snprintf(b, sizeof(b),
                          "two-handed: the barrel in the target frame to frame at most %.2f deg (mean %.3f, %llu of %llu frames over 0.5), grips "
                          "learned while held %llu, pose flips %llu, the snap point's drift %.1f mm, the front wrist's miss %.1f mm",
                          g_bt_jit_max, g_bt_jit_n ? g_bt_jit_sum / static_cast<float>(g_bt_jit_n) : 0.0f, static_cast<unsigned long long>(g_bt_jit_over),
                          static_cast<unsigned long long>(g_bt_jit_n), static_cast<unsigned long long>(g_two_learns),
                          static_cast<unsigned long long>(g_two_flips), g_two_drift_max * 1000.0f, g_two_miss_max * 1000.0f);
        } else if (x == "ik") {
            std::snprintf(b, sizeof(b),
                          "the drawn wrist from its target: L at most %.1f mm (%llu of %llu frames over 1 cm), R at most %.1f mm (%llu of %llu) | near "
                          "misses %llu, the last: path %d count %d drawable %llx, %.1f mm from the sample, x cos %.4f",
                          g_ik_miss_max[0] * 1000.0f, static_cast<unsigned long long>(g_ik_miss_frames[0]), static_cast<unsigned long long>(g_ik_frames[0]),
                          g_ik_miss_max[1] * 1000.0f, static_cast<unsigned long long>(g_ik_miss_frames[1]), static_cast<unsigned long long>(g_ik_frames[1]),
                          static_cast<unsigned long long>(g_held_nearmiss.load()), g_nearmiss_last.path, g_nearmiss_last.count,
                          static_cast<unsigned long long>(g_nearmiss_last.drawable), g_nearmiss_last.mm, g_nearmiss_last.cos_x);
        } else {
            std::snprintf(b, sizeof(b),
                          "the game's gun in its hand's attachment frame, the largest move since the reset: in this frame's set %.1f mm %.2f deg (%llu "
                          "frames), in the previous frame's set %.1f mm %.2f deg (%llu); held draws: newest %llu, older %llu, near misses %llu",
                          g_lag_now.max_mm, g_lag_now.max_deg, static_cast<unsigned long long>(g_lag_now.n), g_lag_prev.max_mm, g_lag_prev.max_deg,
                          static_cast<unsigned long long>(g_lag_prev.n), static_cast<unsigned long long>(g_held_newest.load()),
                          static_cast<unsigned long long>(g_held_older.load()), static_cast<unsigned long long>(g_held_nearmiss.load()));
        }
        if (reset) {
            g_lag_now = LagRange{};
            g_lag_prev = LagRange{};
            for (int h = 0; h < 2; ++h) g_ik_miss_max[h] = 0, g_ik_miss_frames[h] = 0, g_ik_frames[h] = 0;
            g_bt_jit_max = g_bt_jit_sum = 0.0f;
            g_bt_jit_n = g_bt_jit_over = g_two_learns = g_two_flips = 0;
            g_two_drift_max = g_two_miss_max = 0.0f;
            g_two_drift_rebase = true;
            g_bt_jit_rebase = true;
        }
        return b;
    }
    if (sub == "rope") {  // skel rope [on|off]: [Body] LassoAtHands (the session); the lasso's last rope draw
        std::string v;
        in >> v;
        if (v == "on" || v == "off") g_lasso_cfg = v == "on";
        std::lock_guard lock(g_lasso_mutex);
        const float(*d)[3] = g_lasso_dbg;
        char b[600];
        std::snprintf(b, sizeof(b), "rope at hands %d (the lasso left to the game, its rope out: %llu): lasso draws %llu, points draws %llu, moved %llu; the last: %d points, %d moved | game first (%.3f %.3f %.3f) "
                      "last (%.3f %.3f %.3f) | game wrists L (%.3f %.3f %.3f) R (%.3f %.3f %.3f) | drawn first (%.3f %.3f %.3f) last (%.3f %.3f %.3f)",
                      g_lasso_cfg.load() ? 1 : 0, static_cast<unsigned long long>(g_lasso_left.load()), static_cast<unsigned long long>(g_lasso_draws.load()),
                      static_cast<unsigned long long>(g_lasso_points.load()),
                      static_cast<unsigned long long>(g_lasso_moved.load()), g_lasso_n, g_lasso_moved_n, d[0][0], d[0][1], d[0][2], d[1][0], d[1][1], d[1][2],
                      d[2][0], d[2][1], d[2][2], d[3][0], d[3][1], d[3][2], d[4][0], d[4][1], d[4][2], d[5][0], d[5][1], d[5][2]);
        return b;
    }
    if (sub == "near") {  // skel near [on|off|reset]: the drawables drawn within 0.6 m of the item in hand
        std::string v;
        in >> v;
        if (v == "on" || v == "reset") {
            std::lock_guard lock(g_near_mutex);
            g_nnear = 0;
        }
        if (v == "on" || v == "off") g_near_on = v == "on";
        std::lock_guard lock(g_near_mutex);
        std::string o = std::string("near ") + (g_near_on.load() ? "on" : "off") + ", " + std::to_string(g_nnear) + " drawables:";
        for (int i = 0; i < g_nnear; ++i) {
            char b[200];
            std::snprintf(b, sizeof(b), " | %llx %s%s %d at %.3f (%.3f %.3f %.3f) x%llu, attach bone %d at %.3f",
                          static_cast<unsigned long long>(g_near[i].drawable & 0xffffff), g_near[i].kind == 1 ? "set" : "rigid",
                          (g_near[i].drawable & 1) && g_near[i].kind == 1 ? "(bone0)" : "", g_near[i].count, g_near[i].dmin, g_near[i].at[0],
                          g_near[i].at[1], g_near[i].at[2], static_cast<unsigned long long>(g_near[i].n), g_near[i].abone, g_near[i].adist);
            o += b;
        }
        return o;
    }
    if (sub == "held") {  // skel held [on|off] | skel held athand on|off: the item in hand's draws (the session only)
        std::string v;
        in >> v;
        if (v == "on" || v == "off") g_held_fix_cfg = v == "on";
        if (v == "athand") {
            std::string x;
            in >> x;
            g_held_at_hand_cfg = x == "on";
        }

        HeldSample h[kHeldRing];
        {
            std::lock_guard lock(g_draw_mutex);
            std::memcpy(h, g_frame.held, sizeof(h));
        }
        int valid = 0, left = 0, placed = 0;
        for (const HeldSample& x : h)
            if (x.valid) {
                ++valid;
                left += x.left ? 1 : 0;
                placed += x.placed ? 1 : 0;
            }
        const HeldSample* last = newest_held(h);
        char b[600];
        std::snprintf(b, sizeof(b),
                      "held %s, at the hand %s (hook %d, placements %llu): samples %d (left %d, placed %d), at (%.2f %.2f %.2f), draws %llu, "
                      "frames in hand %llu, missed %llu, head matches refused %llu, wrist attachments %d %d; matched the newest %llu, "
                      "older %llu, stale %llu; seq waits %llu, misses %llu",
                      g_held_fix_cfg.load() ? "on" : "off", g_held_at_hand_cfg.load() ? "on" : "off", o_obj_set_matrix ? 1 : 0,
                      static_cast<unsigned long long>(g_placements.load()), valid, left, placed, last ? last->m[12] : 0.0f, last ? last->m[13] : 0.0f,
                      last ? last->m[14] : 0.0f, static_cast<unsigned long long>(g_held_draws.load()),
                      static_cast<unsigned long long>(g_held_frames.load()), static_cast<unsigned long long>(g_held_missed.load()),
                      static_cast<unsigned long long>(g_head_refused.load()), g_rig.att_wrist[0], g_rig.att_wrist[1],
                      static_cast<unsigned long long>(g_held_newest.load()), static_cast<unsigned long long>(g_held_older.load()),
                      static_cast<unsigned long long>(g_held_stale.load()),
                      static_cast<unsigned long long>(g_seq_waits.load()), static_cast<unsigned long long>(g_seq_misses.load()));
        std::string o = b;
        std::snprintf(b, sizeof(b), "; missed by the frame in hand: 1st %llu, 2nd %llu, 3rd %llu, 4th-10th %llu, later %llu (the last at %llu %llu %llu %llu)",
                      static_cast<unsigned long long>(g_held_miss_at[0].load()), static_cast<unsigned long long>(g_held_miss_at[1].load()),
                      static_cast<unsigned long long>(g_held_miss_at[2].load()), static_cast<unsigned long long>(g_held_miss_at[3].load()),
                      static_cast<unsigned long long>(g_held_miss_at[4].load()), static_cast<unsigned long long>(g_held_miss_last[0].load()),
                      static_cast<unsigned long long>(g_held_miss_last[1].load()), static_cast<unsigned long long>(g_held_miss_last[2].load()),
                      static_cast<unsigned long long>(g_held_miss_last[3].load()));
        return o + b;
    }
    if (sub == "stale") {  // skel stale: the held draws and the stale ones (last frame's correction) by pass
        // (F10, the perf check) skel stale fix on|off: [Hands] StaleGunFix for the session
        if (line.find(" fix on") != std::string::npos) g_stale_fix = true;
        if (line.find(" fix off") != std::string::npos) g_stale_fix = false;
        std::string o = std::string("fix ") + (g_stale_fix.load() ? "on" : "off") + " | held draws by pass:";
        for (int i = 0; i < 8; ++i) o += " " + std::to_string(g_held_by_pass[i].load());
        o += " | stale by pass:";
        for (int i = 0; i < 8; ++i) o += " " + std::to_string(g_stale_by_pass[i].load());
        o += " | the second gun or own model by pass:";
        for (int i = 0; i < 8; ++i) o += " " + std::to_string(g_other_by_pass[i].load());
        o += " | stale:";
        for (int i = 0; i < 8; ++i) o += " " + std::to_string(g_other_stale_by_pass[i].load());
        o += " | moved (StaleGunFix): " + std::to_string(g_stale_shifted.load()) + ", the second gun's and the copy's " + std::to_string(g_stale_shifted_other.load());
        o += " | the second gun's unmoved by why (rec-matrix set, by target, off, no IK, no entry, its frame, its count, a jump, rigid):";
        for (int i = 0; i < 9; ++i) o += " " + std::to_string(g_sec_unmoved[i].load());
        char b[200];
        float* sd = g_shake_diag;
        std::snprintf(b, sizeof(b), " | the gun's attachment in the right wrist, as animated: x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f mm, turn %.2f..%.2f deg",
                      sd[0], sd[3], sd[1], sd[4], sd[2], sd[5], sd[6], sd[7]);
        o += b;
        for (int k = 0; k < 3; ++k) sd[k] = sd[6] = 1e9f, sd[3 + k] = sd[7] = -1e9f;
        return o;
    }
    if (sub == "cuts") {  // skel cuts: the show mode's cut points and their midpoint (world), and the game camera's position
        char b[300];
        std::lock_guard lock(g_draw_mutex);
        std::snprintf(b, sizeof(b), "cuts %d: left (%.4f %.4f %.4f) right (%.4f %.4f %.4f) mid (%.4f %.4f %.4f) | camera (%.4f %.4f %.4f) fwd (%.3f %.3f %.3f)",
                      g_cuts_ok ? 1 : 0, g_cuts[0][0], g_cuts[0][1], g_cuts[0][2], g_cuts[1][0], g_cuts[1][1], g_cuts[1][2], g_cuts[2][0], g_cuts[2][1],
                      g_cuts[2][2], g_frame.cam[12], g_frame.cam[13], g_frame.cam[14], -g_frame.cam[8], -g_frame.cam[9], -g_frame.cam[10]);
        return b;
    }
    if (sub == "drawn") {  // skel drawn [ref]: the gun in hand's set as its parts were last drawn, each bone against bone ref (0)
        int rf = 0, only = -1;
        in >> rf >> only;  // skel drawn <ref> <bone>: that bone alone, with its matrix against ref (D rows)
        std::lock_guard lock(g_draw_mutex);
        if (rf < 0 || rf >= g_drawn_count) rf = 0;
        char b[220];
        std::snprintf(b, sizeof(b), "drawn: weapon %d, %d matrices, against bone %d", g_drawn_weapon, g_drawn_count, rf);
        std::string o = b;
        for (int bo = 0; bo < g_drawn_count; ++bo) {
            if (bo == rf || (only >= 0 && bo != only)) continue;
            float D[12], ax[3] = {}, an = 0;
            rel_set(g_drawn_set[rf], g_drawn_set[bo], D);
            if (only >= 0) {
                std::snprintf(b, sizeof(b), " | D %d %d: %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f", rf, bo, D[0], D[1], D[2], D[3], D[4], D[5], D[6],
                              D[7], D[8], D[9], D[10], D[11]);
                o += b;
            }
            const float Rr[9] = {D[0], D[1], D[2], D[4], D[5], D[6], D[8], D[9], D[10]};
            to_axis_angle(Rr, ax, &an);
            // the pivot the turn keeps: (I - R) p = t, least squares across the axis (p . axis = 0)
            float p[3] = {0, 0, 0};
            if (an > 0.01f) {
                const float t[3] = {D[3], D[7], D[11]};
                // solve with the axis's own component dropped: p = (I - R)^+ t via the 3x3 system plus the axis row
                float A[12] = {1 - Rr[0], -Rr[1], -Rr[2], t[0], -Rr[3], 1 - Rr[4], -Rr[5], t[1], -Rr[6], -Rr[7], 1 - Rr[8], t[2]};
                // every row of I - R is square to the axis and one depends on the others: the shortest is replaced by
                // the axis itself (p . axis = 0)
                int worst = 0;
                float best = 1e30f;
                for (int r = 0; r < 3; ++r) {
                    const float d = A[r * 4] * A[r * 4] + A[r * 4 + 1] * A[r * 4 + 1] + A[r * 4 + 2] * A[r * 4 + 2];
                    if (d < best) best = d, worst = r;
                }
                A[worst * 4] = ax[0], A[worst * 4 + 1] = ax[1], A[worst * 4 + 2] = ax[2], A[worst * 4 + 3] = 0;
                const float det = A[0] * (A[5] * A[10] - A[6] * A[9]) - A[1] * (A[4] * A[10] - A[6] * A[8]) + A[2] * (A[4] * A[9] - A[5] * A[8]);
                if (std::fabs(det) > 1e-9f) {
                    for (int c = 0; c < 3; ++c) {
                        float M[9] = {A[0], A[1], A[2], A[4], A[5], A[6], A[8], A[9], A[10]};
                        M[c] = A[3], M[3 + c] = A[7], M[6 + c] = A[11];
                        p[c] = (M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6])) / det;
                    }
                }
            }
            std::snprintf(b, sizeof(b), " | bone %d: %.1f deg about (%.3f %.3f %.3f) at (%.4f %.4f %.4f), shift (%.4f %.4f %.4f)", bo, an * 57.2958f, ax[0],
                          ax[1], ax[2], p[0], p[1], p[2], D[3], D[7], D[11]);
            o += b;
        }
        return o;
    }
    if (sub == "barrel") {  // skel barrel [reset]: [Reload] BarrelHandSnap's draws, the worst miss, the largest jump
        std::string arg;
        in >> arg;
        char b[260];
        {
            std::lock_guard lock(g_draw_mutex);
            std::snprintf(b, sizeof(b), "barrel snap: weight %.2f (opening %.2f), snapped draws %llu, the drawn front wrist's worst miss %.1f mm (fully on), "
                          "the largest one-frame jump %.1f mm", actions::barrel_snap(), g_frame.barrel_open,
                          static_cast<unsigned long long>(g_barrel_snaps), g_barrel_miss_max * 1000.0f, g_barrel_jump_max * 1000.0f);
            if (arg == "reset") g_barrel_snaps = 0, g_barrel_miss_max = 0.0f, g_barrel_jump_max = 0.0f;
        }
        return b;
    }
    if (sub == "partsnap") {  // skel partsnap [reset]: [Reload] PartHandSnap's draws fully on the part, the drawn wrist's miss
        std::string arg;
        in >> arg;
        // (2026-10-28) the hand's hold, read from the body's points alone (the snap's own state not used): the off hand's
        // drawn wrist and its controller's target in the drawn gun's frame, and the free gun hand's in the carried gun's
        // (body::carry_frame: the game update's placement), each as 9 numbers (G^T X, row-major)
        std::string hold;
        {
            body::BodyPoints bp;
            float Rc[9], oc[3];
            const bool bp_ok = body::body_points(&bp);
            auto in_g = [](const float* G, const float* X, const char* tag) {
                char t[220];
                float M[9];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) M[i * 3 + j] = G[0 * 3 + i] * X[0 * 3 + j] + G[1 * 3 + i] * X[1 * 3 + j] + G[2 * 3 + i] * X[2 * 3 + j];
                std::snprintf(t, sizeof(t), " | %s (%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f)", tag, M[0], M[1], M[2], M[3], M[4], M[5], M[6], M[7], M[8]);
                return std::string(t);
            };
            const int off_c = 1 - controls::gun_hand();
            const int jo = bp_ok ? (bp.ctrl[0] == off_c ? 0 : 1) : -1;
            if (bp_ok && bp.gun_frame_ok && bp.hand_ok[jo])
                hold += in_g(bp.gun_frame_R, bp.wrist_drawn[jo], "in hand: wrist in gun") + in_g(bp.gun_frame_R, bp.target_rot[jo], "target in gun");
            const dual::State ds = dual::state();
            if (bp_ok && ds.carry && ds.john >= 0 && ds.john <= 1 && bp.hand_ok[1 - ds.john] && carry_frame(Rc, oc))
                hold += in_g(Rc, bp.wrist_drawn[1 - ds.john], "carried: wrist in gun") + in_g(Rc, bp.target_rot[1 - ds.john], "target in gun");
        }
        char b[900];
        std::lock_guard lock(g_draw_mutex);
        static const char* const kSrc[5] = {"none", "the hand's pinch", "the other hand's pinch mirrored", "the foregrip's", "the foregrip's mirrored"};
        std::snprintf(b, sizeof(b),
                      "part snap: weight %.2f, the handle (%.3f %.3f %.3f) in the gun, turned %.1f deg, snapped draws %llu, the drawn wrist %.1f mm from it (worst %.1f)"
                      " | the spot (the hand's turn) on it %.1f mm (worst %.1f, draws %llu) | carried: weight %.2f turned %.1f deg, spot %.1f mm (worst %.1f, draws %llu)"
                      " | fingers closed: hand %d from %s (%llu draws) | grasp %d: index and middle %.1f deg, the drawn fingers %.1f deg from the pose, "
                      "the pose's own %.1f deg, put %d",
                      g_frame.part_blend, g_frame.part_p[0], g_frame.part_p[1], g_frame.part_p[2], g_frame.part_turn, static_cast<unsigned long long>(g_part_snaps),
                      g_part_miss * 1000.0f, g_part_miss_max * 1000.0f, g_part_spot_miss[0] * 1000.0f, g_part_spot_max[0] * 1000.0f,
                      static_cast<unsigned long long>(g_part_spot_n[0]), g_frame.cpart_blend, g_frame.cpart_turn, g_part_spot_miss[1] * 1000.0f,
                      g_part_spot_max[1] * 1000.0f, static_cast<unsigned long long>(g_part_spot_n[1]), g_grasp_hand, kSrc[g_grasp_src >= 0 && g_grasp_src < 5 ? g_grasp_src : 0],
                      static_cast<unsigned long long>(g_grasp_frames), g_grasp_cfg.load() ? 1 : 0, g_grasp_curl, g_grasp_err, g_grasp_src_curl,
                      g_grasp_force.load());
        if (arg == "reset") {
            g_part_snaps = 0, g_part_miss = 0.0f, g_part_miss_max = 0.0f;
            for (int k = 0; k < 2; ++k) g_part_spot_miss[k] = g_part_spot_max[k] = 0.0f, g_part_spot_n[k] = 0;
            g_cpart_snaps = g_grasp_frames = 0;
        }
        return std::string(b) + hold;
    }
    if (sub == "grasp") {  // skel grasp on|off|auto|open|pinch (2026-10-28, a test's controls, the session): the fingers closed on
        std::string v;     // a gripped bolt, and the pose put (auto: the chosen one; open, pinch: another, measured against it)
        in >> v;
        if (v == "on" || v == "off") g_grasp_cfg = v == "on";
        if (v == "auto" || v == "open" || v == "pinch") g_grasp_force = v == "open" ? 1 : v == "pinch" ? 2 : 0;
        if (v == "curl") {  // (2026-10-29) skel grasp curl <k>: the pose's fingers closed k times further (0.5 - 3)
            float k = 0.0f;
            in >> k;
            if (!in.fail() && k >= 0.5f && k <= 3.0f) g_grasp_curl_k = k;
        }
        static const char* const kF[3] = {"auto", "open", "pinch"};
        char kb[32];
        std::snprintf(kb, sizeof(kb), ", curl %.2f", g_grasp_curl_k.load());
        return std::string("grasp ") + (g_grasp_cfg.load() ? "on" : "off") + ", put " + kF[g_grasp_force.load() % 3] + kb;
    }
    if (sub == "thumb") {  // skel thumb [spur <weapon> <x> <y> <z> | tip <m> | roll <deg> | hold on|off | arm | trace]: [Reload] ThumbCock
        // (2026-10-28) the session's spur offsets (from the hammer's pivot, the hammer down; the gun's x right, y up, z back),
        // the tip's length past thumb_03, the thumb held on the spur (tuning by eye); arm: the frames recorded from now;
        // trace: written to the log ("[body] thumb trace i: ms w hammer dist tipw (x y z)"). The report: the last frame's
        std::string a;
        in >> a;
        // (the review) the trace copied out under the draw mutex, written to the log after it is let go
        std::vector<ThumbRec> rows;
        uint64_t first = 0;
        char b[900];
        {
            std::lock_guard lock(g_draw_mutex);
            if (a == "spur") {
                int w = -1;
                float x = 0, y = 0, z = 0;
                in >> w >> x >> y >> z;
                const int k = spur_slot(w);
                if (k >= 0 && !in.fail() && std::fabs(x) < 0.1f && std::fabs(y) < 0.1f && std::fabs(z) < 0.1f) g_spur[k][0] = x, g_spur[k][1] = y, g_spur[k][2] = z;
            } else if (a == "tip") {
                float t = 0;
                in >> t;
                if (!in.fail() && t >= 0.0f && t < 0.05f) g_thumb_tip = t;
            } else if (a == "hold") {
                std::string v;
                in >> v;
                g_thumb_hold = v == "on";
            } else if (a == "roll") {
                float d = 0;
                in >> d;
                if (!in.fail() && d >= 0.0f && d <= 30.0f) g_thumb_roll = d;
            } else if (a == "arm") {
                g_thumb_rec_n = 0;
                g_thumb_rec_on = true;
            } else if (a == "trace") {
                const uint64_t have = g_thumb_rec_n < static_cast<uint64_t>(kThumbRecN) ? g_thumb_rec_n : kThumbRecN;
                first = g_thumb_rec_n - have;
                rows.reserve(static_cast<size_t>(have));
                for (uint64_t i = first; i < g_thumb_rec_n; ++i) rows.push_back(g_thumb_rec[i % kThumbRecN]);
                g_thumb_rec_on = false;
            }
            const ThumbLast& L = g_thumb_last;
            float R[2][9], o[2][3];
            const bool dg = drawn_gun(R, o, nullptr, nullptr, nullptr);  // ("aim barrelstep on": the gun's root as drawn)
            const float dgo = dg && L.ok ? std::sqrt((o[1][0] - L.gun_o[0]) * (o[1][0] - L.gun_o[0]) + (o[1][1] - L.gun_o[1]) * (o[1][1] - L.gun_o[1]) +
                                                     (o[1][2] - L.gun_o[2]) * (o[1][2] - L.gun_o[2]))
                                         : -1.0f;
            std::snprintf(b, sizeof(b),
                          "thumb: %s weapon %d weight %.2f hammer %.2f | the tip %.1f mm from the spur | spur (%.4f %.4f %.4f) tip (%.4f %.4f %.4f) tipw (%.4f %.4f %.4f) | "
                          "the gun (%.4f %.4f %.4f), the drawn root %.1f mm from it | spurs (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) tip %.3f roll %.1f | hold %d, recorded %llu | joints (%.1f %.1f %.1f) side %.1f straightening held %.1f deg | rest kept %llu",
                          L.ok ? "on" : "none", L.weapon, L.wt, L.hammer, L.dist * 1000.0f, L.spur[0], L.spur[1], L.spur[2], L.tip[0], L.tip[1], L.tip[2], L.tipw[0],
                          L.tipw[1], L.tipw[2], L.gun_o[0], L.gun_o[1], L.gun_o[2], dgo * 1000.0f, g_spur[0][0], g_spur[0][1], g_spur[0][2], g_spur[1][0],
                          g_spur[1][1], g_spur[1][2], g_spur[2][0], g_spur[2][1], g_spur[2][2], g_thumb_tip, g_thumb_roll.load(), g_thumb_hold.load() ? 1 : 0,
                          static_cast<unsigned long long>(g_thumb_rec_n), L.used[0], L.used[1], L.used[2], L.side, L.held,
                          static_cast<unsigned long long>(g_thumb_rest_frames.load()));
        }
        if (a == "spur") log::info("[body] thumb: %s (the session)", b);
        if (a == "trace") {
            log::info("[body] thumb trace: %llu frames", static_cast<unsigned long long>(rows.size()));
            for (size_t i = 0; i < rows.size(); ++i) {
                const ThumbRec& r = rows[i];
                log::info("[body] thumb trace %llu: %.1f ms w %.3f hammer %.3f dist %.4f tipw (%.4f %.4f %.4f) spurw (%.4f %.4f %.4f) used (%.1f %.1f %.1f) side %.1f",
                          static_cast<unsigned long long>(first + i), r.ms, r.wt, r.hammer, r.dist, r.tipw[0], r.tipw[1], r.tipw[2], r.spurw[0], r.spurw[1],
                          r.spurw[2], r.used[0], r.used[1], r.used[2], r.side);
            }
        }
        // (C3) the second gun's or the copy's thumb: the gun in hand's spur poses kept, the frames drawn from them
        return std::string(b) + " | second thumb: snaps " + std::to_string(g_thumb_snaps.load()) + " frames " + std::to_string(g_sthumb_frames.load());
    }
    if (sub == "shake") {  // (GOAL-older-items item 21) skel shake on|off | skel shake: the report
        std::string a;
        in >> a;
        if (a == "on" || a == "off") {
            {
                std::lock_guard lock(g_draw_mutex);
                g_shake_n = 0;
                g_shake_frame = ~0ull;
            }
            g_shake_on = a == "on";
            if (a == "on") g_dg_on = true;  // (the drawn gun's capture: "aim barrelstep" turns it off again)
            return std::string("shake ") + a;
        }
        if (a == "steady") {  // skel shake steady on|off: [Hands] GunSteady (the session)
            std::string v2;
            in >> v2;
            if (v2 == "on" || v2 == "off") g_gun_steady = v2 == "on";
            return std::string("gun steady ") + (g_gun_steady.load() ? "on" : "off") + ", held " + std::to_string(g_gs_held.load()) + " frames, free " +
                   std::to_string(g_gs_free.load());
        }
        std::vector<ShakeRec> v;
        {
            std::lock_guard lock(g_draw_mutex);
            const uint64_t n = g_shake_n;
            for (uint64_t i = n > static_cast<uint64_t>(kShakeN) ? n - kShakeN : 0; i < n; ++i) v.push_back(g_shake[i % kShakeN]);
        }
        if (v.size() < 3) return "shake: " + std::to_string(v.size()) + " frames (skel shake on first)";
        double sg = 0, sc = 0, sr = 0, mg = 0, mc = 0, mr = 0, sd = 0, md = 0, st = 0, mt = 0, sa = 0, ma = 0, sw = 0, mw = 0;
        size_t nr = 0, steps = 0, nw = 0;
        for (size_t i = 1; i < v.size(); ++i) {
            double dg = 0, dc = 0, dr = 0, dd = 0, dt = 0, da = 0;
            for (int k = 0; k < 3; ++k) {
                const double g = static_cast<double>(v[i].gun[k]) - v[i - 1].gun[k], c = static_cast<double>(v[i].cam[k]) - v[i - 1].cam[k];
                const float r1 = v[i].gun[k] - v[i].cam[k], r0 = v[i - 1].gun[k] - v[i - 1].cam[k];  // (as a float shader would)
                const double e = (v[i].gund[k] - v[i].cam[k]) - (v[i - 1].gund[k] - v[i - 1].cam[k]);  // (in double)
                const double tt = static_cast<double>(v[i].tgt[k]) - v[i - 1].tgt[k], aa = static_cast<double>(v[i].actor[k]) - v[i - 1].actor[k];
                dg += g * g, dc += c * c, dr += static_cast<double>(r1 - r0) * (r1 - r0), dd += e * e, dt += tt * tt, da += aa * aa;
            }
            dg = std::sqrt(dg), dc = std::sqrt(dc), dr = std::sqrt(dr), dd = std::sqrt(dd), dt = std::sqrt(dt), da = std::sqrt(da);
            sg += dg, sc += dc, sr += dr, sd += dd, st += dt, sa += da, ++nr;
            mg = std::fmax(mg, dg), mc = std::fmax(mc, dc), mr = std::fmax(mr, dr), md = std::fmax(md, dd), mt = std::fmax(mt, dt), ma = std::fmax(ma, da);
            if (dr > 1e-6) ++steps;
            if (v[i].wr_ok && v[i - 1].wr_ok) {  // John's drawn wrist against the camera
                double dw = 0;
                for (int k = 0; k < 3; ++k) {
                    const double e = (v[i].wr[k] - v[i].cam[k]) - (v[i - 1].wr[k] - v[i - 1].cam[k]);
                    dw += e * e;
                }
                dw = std::sqrt(dw);
                sw += dw, mw = std::fmax(mw, dw), ++nw;
            }
        }
        const ShakeRec& L = v.back();
        const float mag = std::fmax(std::fabs(L.cam[0]), std::fmax(std::fabs(L.cam[1]), std::fabs(L.cam[2])));
        char b[640];
        std::snprintf(b, sizeof(b),
                      "shake: %zu frames at (%.2f %.2f %.2f) (a float's step there %.3f mm) | the gun against the camera: mean %.4f mm max %.4f mm, "
                      "moved in %zu of %zu frames | the gun: mean %.4f max %.4f mm | the camera: mean %.4f max %.4f mm | in double: mean %.4f max %.4f mm | "
                      "the wrist target: mean %.4f max %.4f mm | John: mean %.4f max %.4f mm | the drawn wrist: mean %.4f max %.4f mm (%zu)",
                      v.size(), L.cam[0], L.cam[1], L.cam[2], static_cast<double>(std::nextafter(mag, 1e9f) - mag) * 1000.0, sr / nr * 1000.0, mr * 1000.0,
                      steps, nr, sg / nr * 1000.0, mg * 1000.0, sc / nr * 1000.0, mc * 1000.0, sd / nr * 1000.0, md * 1000.0, st / nr * 1000.0, mt * 1000.0,
                      sa / nr * 1000.0, ma * 1000.0, nw ? sw / nw * 1000.0 : -1.0, mw * 1000.0, nw);
        char e[160];  // (r104) the last frame's drawn item and the gun hand's wrist target (world)
        std::snprintf(e, sizeof(e), " | last drawn (%.3f %.3f %.3f) target (%.3f %.3f %.3f)", L.gun[0], L.gun[1], L.gun[2], L.tgt[0], L.tgt[1], L.tgt[2]);
        return std::string(b) + e;
    }
    if (sub == "fingers") {  // skel fingers [grip on|off]: each hand's fingers' mean turn from its wrist, as drawn; [Hands] CopyGrip
        std::string arg, v;
        in >> arg >> v;
        if (arg == "grip" && (v == "on" || v == "off")) g_copy_grip_cfg = v == "on";
        char b[200];
        std::snprintf(b, sizeof(b), "fingers: left %.1f deg, right %.1f deg from the wrist (mean of the finger bones, as drawn) | copy grip %s, the copy %s",
                      g_curl[0].load(), g_curl[1].load(), g_copy_grip_cfg.load() ? "on" : "off", dual::copy_W() ? "out" : "not out");
        return b;
    }
    if (sub == "tips") {  // skel tips (2026-10-28, read only; r44's check of the held magazine): each hand's drawn thumb and
        // index tips, their midpoint (the pinch) and the wrist target, world, as the body's points publish them
        body::BodyPoints bp;
        if (!body::body_points(&bp)) return "tips: no body points";
        std::string o = "tips:";
        for (int h = 0; h < 2; ++h) {
            char b[400];
            float pm[3];
            for (int k = 0; k < 3; ++k) pm[k] = 0.5f * (bp.thumb_tip[h][k] + bp.index_tip[h][k]);
            std::snprintf(b, sizeof(b), " | %s (controller %d) ok %d tips %d thumb (%.3f %.3f %.3f) index (%.3f %.3f %.3f) pinch (%.3f %.3f %.3f) wrist (%.3f %.3f %.3f)",
                          h ? "right" : "left", bp.ctrl[h], bp.hand_ok[h] ? 1 : 0, bp.tips_ok[h] ? 1 : 0, bp.thumb_tip[h][0], bp.thumb_tip[h][1], bp.thumb_tip[h][2],
                          bp.index_tip[h][0], bp.index_tip[h][1], bp.index_tip[h][2], pm[0], pm[1], pm[2], bp.hand[h][0], bp.hand[h][1], bp.hand[h][2]);
            o += b;
        }
        return o;
    }
    if (sub == "passcmp") {  // (item 8e, test only) skel passcmp on|off | (the report)
        std::string arg;
        in >> arg;
        if (arg == "on" || arg == "off") {
            if (arg == "on")
                for (int k = 0; k < 2; ++k) {
                    g_pc_eq[k] = 0;
                    g_pc_ne[k] = 0;
                    g_pc_max[k] = 0.0f;
                }
            g_pc_unpaired = 0;
            g_pc_on = arg == "on";
        }
        char b[300];
        std::snprintf(b, sizeof(b), "passcmp %s: raw equal %llu differ %llu (largest %.6g); drawn equal %llu differ %llu (largest %.6g); unpaired %llu",
                      g_pc_on.load() ? "on" : "off", static_cast<unsigned long long>(g_pc_eq[0].load()), static_cast<unsigned long long>(g_pc_ne[0].load()),
                      g_pc_max[0].load(), static_cast<unsigned long long>(g_pc_eq[1].load()), static_cast<unsigned long long>(g_pc_ne[1].load()),
                      g_pc_max[1].load(), static_cast<unsigned long long>(g_pc_unpaired.load()));
        return b;
    }
    if (sub == "follow") {  // skel follow arm | on|off ([Hands] OwnModelFollow, the session) | (the report)
        std::string arg;
        in >> arg;
        if (arg == "on" || arg == "off") {
            g_own_follow = arg == "on";
            return std::string("follow: OwnModelFollow ") + (g_own_follow.load() ? "on" : "off");
        }
        if (arg == "auto") {  // skel follow auto on|off: [Debug] FollowLog for the session
            std::string v;
            in >> v;
            g_follow_auto = v != "off";
            if (!g_follow_auto.load()) g_follow_rec = false;
            return std::string("follow: auto (the 10 s report) ") + (g_follow_auto.load() ? "on" : "off");
        }
        std::lock_guard lock(g_draw_mutex);
        if (arg == "arm") {
            int pass = 1;
            in >> pass;
            g_follow_pass = pass & 7;
            g_follow_n = 0;
            g_follow_rec = true;
            g_follow_manual = true;
            return "follow: recording " + std::to_string(kFollowMax) + " frames of pass " + std::to_string(g_follow_pass.load());
        }
        g_follow_manual = false;  // read: FollowLog's again
        // each object in its wrist's frame (X = W^-1 O) against the first such frame: the move (mm) and turn (deg); the
        // wrist's own speed (mm a frame), to tell a gun a frame late (its error grows with the speed) from an offset
        std::string o;
        char b[400];
        std::snprintf(b, sizeof(b), "follow: %d frames of pass %d (%s) | own model follow %s: draws %llu, no copy sample %llu, held_prop's pose newer %llu", g_follow_n,
                      g_follow_pass.load(), g_follow_rec.load() ? "recording" : "done", g_own_follow.load() ? "on" : "off",
                      static_cast<unsigned long long>(g_own_follow_draws.load()), static_cast<unsigned long long>(g_own_follow_nocp.load()),
                      static_cast<unsigned long long>(g_own_follow_newer.load()));
        o = b;
        return o + follow_report("");
    }
    if (sub == "parts") {  // skel parts arm: record the held gun's set; skel parts [ref]: each bone against bone ref (default 0)
        std::string arg;
        in >> arg;
        std::lock_guard lock(g_draw_mutex);
        if (arg == "arm") {
            g_parts_auto = false;  // a test's recording: not TwoHandShotLog's
            g_parts_manual = true;
            g_parts_n = 0;
            g_parts_frame = ~0ull;
            g_parts_rec = true;
            return "parts: recording the held gun's set (" + std::to_string(kPartsMax) + " frames)";
        }
        if (arg == "dump") {  // skel parts dump <bone> [ref]: each frame's D of that bone against ref, to the log
            g_parts_manual = false;
            int bo = 2, rf = 0;
            in >> bo >> rf;
            for (int i = 0; i < g_parts_n; ++i) {
                if (bo < 0 || bo >= g_parts[i].count || rf < 0 || rf >= g_parts[i].count) break;
                float D[12];
                rel_set(g_parts[i].m[rf], g_parts[i].m[bo], D);
                log::info("[parts] %4.0f ms bone %d vs %d: (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f)", g_parts[i].ms - g_parts[0].ms,
                          bo, rf, D[0], D[1], D[2], D[3], D[4], D[5], D[6], D[7], D[8], D[9], D[10], D[11]);
            }
            return "parts: " + std::to_string(g_parts_n) + " frames logged";
        }
        if (arg == "post") {  // skel parts post: the drawn gun against its first recorded frame (deg/mm over time)
            g_parts_manual = false;
            return parts_post();
        }
        if (arg == "lever") {
            char b[280];
            std::snprintf(b, sizeof(b), "lever draws %llu, steady draws %llu, misses %llu, clips stopped %llu (fire clip %.2f; rest %d %d, steady %d %d) | "
                          "steadied by hand %llu %llu, transplanted %llu",
                          static_cast<unsigned long long>(g_lever_draws.load()), static_cast<unsigned long long>(g_steady_draws.load()),
                          static_cast<unsigned long long>(g_steady_misses.load()), static_cast<unsigned long long>(g_clip_stops.load()),
                          holster::fire_clip_phase(), g_steady[0].rest_ok ? 1 : 0,
                          g_steady[1].rest_ok ? 1 : 0, g_steady[0].on ? 1 : 0, g_steady[1].on ? 1 : 0,
                          static_cast<unsigned long long>(g_steady_hand[0].load()), static_cast<unsigned long long>(g_steady_hand[1].load()),
                          static_cast<unsigned long long>(g_steady_xf.load()));
            return b;
        }
        if (arg == "max") {  // skel parts max <bone> [ref]: that bone's largest turn and move against ref over the frames (short)
            int bo = 2, rf = 0;
            in >> bo >> rf;
            float mdeg = 0, mmm = 0;
            int n = 0;
            for (int i = 0; i < g_parts_n; ++i) {
                const int cnt = g_parts[i].count;
                if (bo < 0 || bo >= cnt || rf < 0 || rf >= cnt) continue;
                float D[12], a3[3], an = 0;
                rel_set(g_parts[i].m[rf], g_parts[i].m[bo], D);
                const float R[9] = {D[0], D[1], D[2], D[4], D[5], D[6], D[8], D[9], D[10]};
                to_axis_angle(R, a3, &an);
                mdeg = std::fmax(mdeg, an * 57.2958f);
                mmm = std::fmax(mmm, std::sqrt(D[3] * D[3] + D[7] * D[7] + D[11] * D[11]) * 1000.0f);
                ++n;
            }
            char b[160];
            std::snprintf(b, sizeof(b), "parts max: bone %d vs %d: %.1f deg, %.1f mm over %d frames", bo, rf, mdeg, mmm, n);
            return b;
        }
        const int ref = arg.empty() ? 0 : std::atoi(arg.c_str());
        char b[600];
        std::snprintf(b, sizeof(b), "parts: %d frames (%s), %.0f ms, %d matrices", g_parts_n, g_parts_rec.load() ? "recording" : "done",
                      g_parts_n ? g_parts[g_parts_n - 1].ms - g_parts[0].ms : 0.0, g_parts_n ? g_parts[0].count : 0);
        std::string o = b;
        if (!g_parts_n) return o;
        const int cnt = g_parts[0].count;
        if (ref < 0 || ref >= cnt) return o + " | no such reference bone";  // (the review: an unchecked index)
        for (int bo = 0; bo < cnt; ++bo) {
            if (bo == ref) continue;
            int at = 0;
            float best = -1, bt = 0, D[12], ax[3] = {}, ang = 0;
            for (int i = 0; i < g_parts_n; ++i) {
                if (g_parts[i].count != cnt) continue;
                rel_set(g_parts[i].m[ref], g_parts[i].m[bo], D);
                const float R[9] = {D[0], D[1], D[2], D[4], D[5], D[6], D[8], D[9], D[10]};
                float a3[3], an = 0;
                to_axis_angle(R, a3, &an);
                const float tl = std::sqrt(D[3] * D[3] + D[7] * D[7] + D[11] * D[11]);
                if (an * 57.2958f + tl * 1000.0f > best) best = an * 57.2958f + tl * 1000.0f, at = i, bt = tl;
            }
            rel_set(g_parts[at].m[ref], g_parts[at].m[bo], D);
            const float R[9] = {D[0], D[1], D[2], D[4], D[5], D[6], D[8], D[9], D[10]};
            to_axis_angle(R, ax, &ang);
            std::snprintf(b, sizeof(b), "\n  bone %d vs %d: max %.1f deg about (%.3f %.3f %.3f), %.1f mm, at %.0f ms | D (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f) | deg over time:",
                          bo, ref, ang * 57.2958f, ax[0], ax[1], ax[2], bt * 1000.0f, g_parts[at].ms - g_parts[0].ms, D[0], D[1], D[2], D[3], D[4], D[5],
                          D[6], D[7], D[8], D[9], D[10], D[11]);
            o += b;
            for (int i = 0; i < g_parts_n; i += 4) {
                rel_set(g_parts[i].m[ref], g_parts[i].m[bo], D);
                const float Ri[9] = {D[0], D[1], D[2], D[4], D[5], D[6], D[8], D[9], D[10]};
                float a3[3], an = 0;
                to_axis_angle(Ri, a3, &an);
                std::snprintf(b, sizeof(b), " %.0f/%.0f", an * 57.2958f, std::sqrt(D[3] * D[3] + D[7] * D[7] + D[11] * D[11]) * 1000.0f);
                o += b;
            }
        }
        return o;
    }
    if (sub == "open") {  // skel open: [Reload] OpenCylinder's draws and the last turned bone (its rows: x y z, translation)
        char b[300];
        std::lock_guard lock(g_draw_mutex);
        const float* m = g_open_last;
        int ow = -1;
        const float amt = actions::open_amount(&ow);
        std::snprintf(b, sizeof(b), "open draws %llu (amount %.2f, weapon %d; the held set: %d matrices, record matrix %d; rigid held draws %llu) | bone rows (%.3f %.3f %.3f | %.3f) (%.3f %.3f %.3f | %.3f) (%.3f %.3f %.3f | %.3f) | the second gun's open draws %llu",
                      static_cast<unsigned long long>(g_open_draws.load()), amt, ow, g_held_set_count.load(), g_held_set_rm.load(),
                      static_cast<unsigned long long>(g_rigid_props.load()), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                      static_cast<unsigned long long>(g_sec_open_draws.load()));
        return b;
    }
    if (sub == "hprop") {  // skel hprop: the held props' draws (held_prop): the paths and the last one's positions
        char b[600];
        std::lock_guard lock(g_draw_mutex);
        const HpDiag& d = g_hp_diag;
        std::snprintf(b, sizeof(b), "hprop draws %llu (skinned %llu, rigid %llu, the pool's spares hidden %llu; the round's pinch learned %llu, put %llu frames) | last: path %d count %d flags %#x before (%.3f %.3f %.3f) target (%.3f %.3f %.3f) after (%.3f %.3f %.3f) x before (%.3f %.3f %.3f) after (%.3f %.3f %.3f)",
                      static_cast<unsigned long long>(g_hprop_draws.load()), static_cast<unsigned long long>(g_hprop_skinned.load()),
                      static_cast<unsigned long long>(g_hprop_rigid.load()), static_cast<unsigned long long>(g_hprop_hidden.load()),
                      static_cast<unsigned long long>(g_round_pinch_learned.load()), static_cast<unsigned long long>(g_round_pinch_frames.load()), d.path, d.count, d.flags, d.before[0], d.before[1], d.before[2],
                      d.target[0], d.target[1], d.target[2], d.after[0], d.after[1], d.after[2], d.xb[0], d.xb[1], d.xb[2], d.xa[0], d.xa[1], d.xa[2]);
        return b;
    }
    if (sub == "grip") {  // skel grip [on|off|reset]: [Hands] FixedGunGrip (the session); the long gun in the animated wrist's frame
        if (line.find(" grip on") != std::string::npos) g_fixed_grip = true;
        if (line.find(" grip off") != std::string::npos) g_fixed_grip = false;
        if (line.find(" learn on") != std::string::npos) g_fg_learn = true;  // skel grip learn on|off (a test, the session)
        if (line.find(" learn off") != std::string::npos) g_fg_learn = false;
        if (line.find(" side on") != std::string::npos) g_fixed_sidearm = true;  // skel grip side on|off: FixedSidearmGrip
        if (line.find(" side off") != std::string::npos) g_fixed_sidearm = false;
        FixedGripDiag d;
        GunRel g;
        {
            std::lock_guard lock(g_fg_mutex);
            if (line.find(" grip reset") != std::string::npos) {
                g_fg_diag.aim_max_mm = g_fg_diag.aim_max_deg = g_fg_diag.low_max_mm = g_fg_diag.low_max_deg = 0;
                g_fg_diag.placed_max_mm = g_fg_diag.placed_max_deg = 0;
            }
            d = g_fg_diag;
            if (d.weapon >= 0 && d.weapon < kGripW) g = g_gun_rel[d.weapon];
        }
        float mm = -1, deg = -1;
        if (g.valid) rel_diff(d.now, g.m, &mm, &deg);
        char b[1000];
        std::snprintf(b, sizeof(b),
                      "fixed grip %s | weapon %d side %d aiming %d src %d | now (%.4f %.4f %.4f) x (%.3f %.3f %.3f) | learned %d (%.4f %.4f %.4f) "
                      "now off it %.1f mm %.2f deg | max off it: aiming %.1f mm %.2f deg, lowered %.1f mm %.2f deg | placed (%.4f %.4f %.4f) max "
                      "off it %.1f mm %.2f deg | learns %llu fixed %llu frames %llu saved %llu | by src %llu %llu %llu | tick differs %llu | why %d | wrist at (%.2f %.2f %.2f) gun (%.2f %.2f %.2f) "
                      "attachment (%.2f %.2f %.2f)",
                      g_fixed_grip.load() ? "on" : "off", d.weapon, d.side, d.aiming, d.src, d.now[12], d.now[13], d.now[14], d.now[0], d.now[1],
                      d.now[2], g.valid ? 1 : 0, g.m[12], g.m[13], g.m[14], mm, deg, d.aim_max_mm, d.aim_max_deg, d.low_max_mm, d.low_max_deg,
                      d.placed[12], d.placed[13], d.placed[14], d.placed_max_mm, d.placed_max_deg, static_cast<unsigned long long>(d.learns),
                      static_cast<unsigned long long>(d.fixed), static_cast<unsigned long long>(d.frames), static_cast<unsigned long long>(d.saved),
                      static_cast<unsigned long long>(d.by_src[0]), static_cast<unsigned long long>(d.by_src[1]),
                      static_cast<unsigned long long>(d.by_src[2]), static_cast<unsigned long long>(g_fg_tick_differs.load()), d.why,
                      d.wrist_pos[0], d.wrist_pos[1], d.wrist_pos[2], d.gun_pos[0], d.gun_pos[1], d.gun_pos[2], d.att_pos[0], d.att_pos[1], d.att_pos[2]);
        return b;
    }
    if (sub == "copy" && line.find("where") != std::string::npos) {  // skel copy where: the last copy draw's place (run 7 item 1)
        CopyWhere w;
        {
            std::lock_guard lock(g_draw_mutex);
            w = g_copy_where;
        }
        BodyPoints bp;
        const bool hb = body_points(&bp);
        auto d = [](const float* a, const float* b) {
            const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
            return std::sqrt(x * x + y * y + z * z);
        };
        char b[520];
        std::snprintf(b, sizeof(b),
                      "copy where: draws %llu, the last kind %d pass %llu: the gun in hand's draw at (%.3f %.3f %.3f), the copy's at (%.3f %.3f %.3f), "
                      "%.3f m apart | hands %d: left (%.3f %.3f %.3f) right (%.3f %.3f %.3f); the copy %.3f m from the left, %.3f from the right",
                      static_cast<unsigned long long>(w.n), w.kind, static_cast<unsigned long long>(w.pass), w.pre[0], w.pre[1], w.pre[2], w.post[0],
                      w.post[1], w.post[2], d(w.pre, w.post), hb ? 1 : 0, bp.hand[0][0], bp.hand[0][1], bp.hand[0][2], bp.hand[1][0], bp.hand[1][1],
                      bp.hand[1][2], hb ? d(w.post, bp.hand[0]) : -1.0f, hb ? d(w.post, bp.hand[1]) : -1.0f);
        return b;
    }
    if (sub == "copy") {  // skel copy: [Hands] DualWieldCopy's placements and draws
        char b[200];
        std::snprintf(b, sizeof(b), "copy W %p | placements %llu, refused %llu (why %d), draws %llu, unmatched %llu | own model %d (weapon %d): copy draws skipped %llu, shots moved to its muzzle %llu",
                      reinterpret_cast<void*>(dual::copy_W()), static_cast<unsigned long long>(g_copy_placements.load()),
                      static_cast<unsigned long long>(g_copy_refused.load()), g_copy_why.load(), static_cast<unsigned long long>(g_copy_draws.load()),
                      static_cast<unsigned long long>(g_copy_nomatch.load()), dual::own_model() ? 1 : 0, dual::copy_model(),
                      static_cast<unsigned long long>(g_copy_own_skips.load()), static_cast<unsigned long long>(g_copy_muzzle_moved.load()));
        return b;
    }
    if (sub == "boltbone") {  // skel boltbone: the bolt guns' drawn bolt (bone 2) in the world, in hand and carried ([Reload] CarriedBolt's check)
        std::lock_guard lock(g_draw_mutex);
        std::string o = "boltbone";
        for (int i = 0; i < 2; ++i) {
            const BoltBone& bb = g_bolt_bone[i];
            char b[360];
            std::snprintf(b, sizeof(b), " | %s: weapon %d age %.0f ms o (%.4f %.4f %.4f) x (%.4f %.4f %.4f) y (%.4f %.4f %.4f) z (%.4f %.4f %.4f)",
                          i ? "carried" : "in hand", bb.weapon, bb.ms > 0 ? log::now_ms() - bb.ms : -1.0, bb.o[0], bb.o[1], bb.o[2], bb.R[0], bb.R[3],
                          bb.R[6], bb.R[1], bb.R[4], bb.R[7], bb.R[2], bb.R[5], bb.R[8]);
            o += b;
        }
        return o;
    }
    if (sub == "carry") {  // skel carry [old on|off] [trace <ms>]: [Hands] CarryByForegrip's placements and the two-handed captures
        {
            std::string x, y;
            in >> x >> y;
            if (x == "old" && (y == "on" || y == "off")) {  // the test aid (carry_old_test), the session
                g_carry_old.store(y == "on");
                log::info("[body] carry: %s (skel carry old)", y == "on" ? "the old placement, a test's control" : "as made");
            }
            if (x == "olddelta" && (y == "on" || y == "off")) {  // the put-away's draws by the wrist's correction (a test's control)
                g_carry_olddelta.store(y == "on");
                log::info("[body] carry: the put-away's draws by %s (skel carry olddelta)", y == "on" ? "the wrist's correction, a test's control" : "the target");
            }
            if (x == "trace") {
                const double ms = std::atof(y.c_str());
                g_carry_trace_until.store(ms > 0.0 && ms <= 60000.0 ? log::now_ms() + ms : 0.0);
            }
        }
        TwoHandT c{};
        for (int tries = 0; tries < 8; ++tries) {
            const uint32_t s0 = g_two_T_seq.load(std::memory_order_acquire);
            if (s0 & 1) continue;
            c = g_two_T;
            if (g_two_T_seq.load(std::memory_order_acquire) == s0) break;
        }
        double cb[2][3] = {};  // each hand's corrected attachment bone (the carry's frame), now
        for (int hb = 0; hb < 2; ++hb) {
            float R_[9], A_[9], a_[3];
            double ad_[3] = {};
            corrected_bone(hb, R_, cb[hb], A_, a_, ad_);
        }
        float cT[16] = {};  // T as the hand-over would make it now, and the bone it took
        double ct[3] = {};
        const bool cT_ok = carry_T_from(c.P, c.fj, cT, ct);  // (the review: a fail flag, never a half-made T printed as one)
        if (!cT_ok) std::memset(cT, 0, sizeof(cT));
        char b[640];
        std::snprintf(b, sizeof(b), "carry placements %llu, refused %llu (why %d) | two-handed T: takes %llu, W %p, age %.0f ms, at (%.3f %.3f %.3f), "
                      "the gun placed at (%.3f %.3f %.3f) | the hand's hold: learns %llu, holds %llu, clears %llu, set %d %d, offset (%.3f %.3f %.3f), finger frames %llu, T settled after %d | "
                      "corrected bones: left (%.3f %.3f %.3f), right (%.3f %.3f %.3f), bones %d %d; T's now %s(%.3f %.3f %.3f)",
                      static_cast<unsigned long long>(g_carry_placements.load()), static_cast<unsigned long long>(g_carry_refused.load()), g_sec_why.load(),
                      static_cast<unsigned long long>(g_two_T_takes.load()), reinterpret_cast<void*>(c.W), c.ms > 0 ? log::now_ms() - c.ms : -1.0,
                      cT[12], cT[13], cT[14], c.P[12], c.P[13], c.P[14], static_cast<unsigned long long>(g_carry_learns),
                      static_cast<unsigned long long>(g_carry_holds), static_cast<unsigned long long>(g_carry_clears), g_carry_rel_ok[0] ? 1 : 0,
                      g_carry_rel_ok[1] ? 1 : 0, g_carry_prel[0][0], g_carry_prel[0][1], g_carry_prel[0][2],
                      static_cast<unsigned long long>(g_carry_finger_frames.load()), g_carry_settle_n.load(), cb[0][0], cb[0][1], cb[0][2], cb[1][0],
                      cb[1][1], cb[1][2], g_att_bone[0].load(), g_att_bone[1].load(), cT_ok ? "" : "FAILED ", ct[0], ct[1], ct[2]);
        char b2[520];
        std::snprintf(b2, sizeof(b2), " | carried parts drawn %llu | two-handed not captured: left-held %llu, not finite %llu; no front hand %llu, fourth column not finite %llu"
                      " | put-away at the front hand %llu, old %d (olddelta %d), trace %d | settled passed over moving %llu, still captures used %llu"
                      " | eased %llu (%.0f ms, the last start %.3f m off)",
                      static_cast<unsigned long long>(g_carry_part_draws.load()), static_cast<unsigned long long>(g_two_T_left.load()),
                      static_cast<unsigned long long>(g_two_T_nonfinite.load()), static_cast<unsigned long long>(g_two_T_nohand.load()),
                      static_cast<unsigned long long>(g_two_T_col4.load()), static_cast<unsigned long long>(g_carry_puts.load()), g_carry_old.load() ? 1 : 0, g_carry_olddelta.load() ? 1 : 0,
                      log::now_ms() < g_carry_trace_until.load() ? 1 : 0, static_cast<unsigned long long>(g_carry_settle_moving.load()),
                      static_cast<unsigned long long>(g_two_T_still_used.load()), static_cast<unsigned long long>(g_carry_eased.load()),
                      g_carry_ease_ms.load(), g_carry_ease_d.load());
        char b3[200];  // the put-away's updates out of the game's hand by the item's state (1 stowed, 2 holstered, 3 drawn)
        std::snprintf(b3, sizeof(b3), " | put-away states 0:%llu 1:%llu 2:%llu 3 in hand:%llu 3 not:%llu 3 unread:%llu other:%llu",
                      static_cast<unsigned long long>(g_carry_put_states[0].load()), static_cast<unsigned long long>(g_carry_put_states[1].load()),
                      static_cast<unsigned long long>(g_carry_put_states[2].load()), static_cast<unsigned long long>(g_carry_put_states[3].load()),
                      static_cast<unsigned long long>(g_carry_put_states[4].load()), static_cast<unsigned long long>(g_carry_put_states[5].load()),
                      static_cast<unsigned long long>(g_carry_put_states[6].load()));
        char b4[200];  // (C37b) the carried gun as drawn, what carry_frame gives while it is
        {
            std::lock_guard lk(g_cmodel_mx);
            std::snprintf(b4, sizeof(b4), " | carried model %d at (%.3f %.3f %.3f) %.0f ms ago, used %llu", g_cmodel.ok ? 1 : 0, g_cmodel.o[0], g_cmodel.o[1],
                          g_cmodel.o[2], log::now_ms() - g_cmodel.ms, static_cast<unsigned long long>(g_cmodel_uses.load()));
        }
        return std::string(b) + b2 + b3 + b4;
    }
    if (sub == "dual") {  // skel dual: the second gun's placements; the placement rule checked on the gun in hand (reads only)
        float in_err = -1.0f, rule_err = -1.0f, m[16];
        int side = -1, lrig = -1, rrig = -1;
        bool left = false;
        uintptr_t W = 0, wmgr = 0, mtx = 0, L = 0;
        const uintptr_t skel = g_skel_game.load();
        auto maxdiff = [](const float* a, const float* b) {  // the rows' xyz
            float d = 0.0f;
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 3; ++k) d = std::fmax(d, std::fabs(a[r * 4 + k] - b[r * 4 + k]));
            return d;
        };
        alignas(16) float B[16], I[16], G[16], P[16], W80[16], Gr[16], Gl[16];
        if (aim::held_item_matrix(m, &left, &W, &wmgr) && W) {
            side = left ? 0 : 1;
            const int ab = g_att_bone[side].load();
            if (ab >= 0 && skel && raw(skel + 0x28, &mtx, sizeof(mtx)) && mtx && raw(mtx + static_cast<uintptr_t>(ab) * 0x40, B, sizeof(B)) &&
                raw(W + 0x40, I, sizeof(I)))
                in_err = maxdiff(I, B);
            if (raw(W + 0x338, &L, sizeof(L)) && L && raw(L + (side ? 0xc0 : 0x100), G, sizeof(G)) && raw(W + 0x40, I, sizeof(I)) &&
                raw(W + 0x80, W80, sizeof(W80))) {
                grip_times(G, I, P);
                rule_err = maxdiff(P, W80);
            }
            if (L && raw(L + 0xc0, Gr, sizeof(Gr))) rrig = rigid_rows(Gr) ? 1 : 0;
            if (L && raw(L + 0x100, Gl, sizeof(Gl))) lrig = rigid_rows(Gl) ? 1 : 0;
        }
        int sj = -1;
        const uintptr_t sw = dual::secondary_W(&sj);
        float sp[3] = {};
        uint32_t wfl = 0, pfl = 0;
        uint16_t ph = 0, pg = 0, sg = 0;
        uintptr_t pool = 0, pobj = 0, pp = 0;
        if (sw) {
            raw(sw + 0xb0, sp, sizeof(sp));
            raw(sw + 0x120, &wfl, sizeof(wfl));  // bit 5 active, 6 the prop hidden, 12 its render disabled
            // its prop (W +0x2bc handle, +0x2be generation in the objects pool), +0x110 -> +0x2c bit 8 hidden
            if (raw(sw + 0x2bc, &ph, sizeof(ph)) && raw(sw + 0x2be, &pg, sizeof(pg)) && raw(anchors::addr(anchors::Id::ObjectsPool), &pool, sizeof(pool)) &&
                pool && raw(pool + static_cast<uintptr_t>(ph) * 0x10 + 8, &sg, sizeof(sg)) && sg == pg &&
                raw(pool + static_cast<uintptr_t>(ph) * 0x10, &pobj, sizeof(pobj)) && pobj && raw(pobj + 0x110, &pp, sizeof(pp)) && pp)
                raw(pp + 0x2c, &pfl, sizeof(pfl));
        }
        float hp[3] = {};  // the gun in hand's W +0xb0 (the carry's test: where the carried gun was before the hand-over)
        if (W) raw(W + 0xb0, hp, sizeof(hp));
        char b[560];
        std::snprintf(b, sizeof(b),
                      "second gun: W %p (John's hand %d), placements %llu, refused %llu (last why %d), draws %llu (loose %llu), near misses %llu (%.3f m, row %.3f), "
                      "at (%.3f %.3f %.3f), W flags 0x%x, prop %s flags 0x%x | the rule on the gun in hand (%s): its input %.4f from the attachment bone, "
                      "W +0x80 %.4f from grip x input; grips right %d left %d | the gun in hand at (%.3f %.3f %.3f)",
                      reinterpret_cast<void*>(sw), sj, static_cast<unsigned long long>(g_sec_placements.load()),
                      static_cast<unsigned long long>(g_sec_refused.load()), g_sec_why.load(), static_cast<unsigned long long>(g_sec_draws.load()),
                      static_cast<unsigned long long>(g_sec_loose.load()), static_cast<unsigned long long>(g_sec_near.load()), g_sec_near_d, g_sec_near_x, sp[0], sp[1], sp[2], wfl, pobj ? "found" : "none", pfl,
                      side == 0 ? "left" : side == 1 ? "right" : "none", in_err, rule_err, rrig, lrig, hp[0], hp[1], hp[2]);
        return b;
    }
    if (sub == "cull") {  // skel cull [draw|skip|filter] [list <from>]: the hidden-triangle filter's mode, counters, entries
        std::string x, y;
        in >> x >> y;
        if (x == "draw" || x == "skip" || x == "filter") g_geom_mode = x == "skip" ? 1 : x == "filter" ? 2 : 0;
        if (x == "passes") g_cull_pass_mask = static_cast<uint32_t>(std::strtoul(y.c_str(), nullptr, 0));  // skel cull passes <mask> (the session)
        char b[480];
        if (x == "list") {
            std::lock_guard lock(g_cull_note_mutex);
            const uint32_t from = static_cast<uint32_t>(std::atoi(y.c_str()));
            std::string o = "entries " + std::to_string(g_cull_note_n) + ":";
            for (uint32_t i = from; i < g_cull_note_n && i < from + 6 && i + 64 >= g_cull_note_n; ++i) {
                const CullNote& n = g_cull_notes[i % 64];
                std::snprintf(b, sizeof(b), " | %u: show %u kind %u why %u read %u runs %u tris %u k%u d%u a%u v%u w255 %u", i, n.show, n.kind, n.why,
                              n.reading, n.nruns, n.tris, n.keep, n.drop, n.any, n.verts, n.w255);
                o += b;
            }
            return o;
        }
        std::snprintf(b, sizeof(b),
                      "hidden geometry %s | entries %llu (skip %llu, draw %llu, filter %llu, rejected %llu) | draws skipped %llu, filtered %llu "
                      "(runs %llu), count mismatches %llu, armed without a recorded draw %llu | filtered by pass%s (mask 0x%x)",
                      g_geom_mode.load() == 2 ? "filter" : g_geom_mode.load() == 1 ? "skip" : "draw",
                      static_cast<unsigned long long>(g_cull_built.load()), static_cast<unsigned long long>(g_cull_kind[1].load()),
                      static_cast<unsigned long long>(g_cull_kind[2].load()), static_cast<unsigned long long>(g_cull_kind[3].load()),
                      static_cast<unsigned long long>(g_cull_kind[4].load()), static_cast<unsigned long long>(g_cull_skipped.load()),
                      static_cast<unsigned long long>(g_cull_filtered.load()), static_cast<unsigned long long>(g_cull_runs.load()),
                      static_cast<unsigned long long>(g_cull_mismatch.load()), static_cast<unsigned long long>(g_cull_nodraw.load()), [] {
                          static thread_local char bp[96];
                          int o = 0;
                          for (int i = 0; i < 8; ++i)
                              o += std::snprintf(bp + o, sizeof(bp) - o, " %llu", static_cast<unsigned long long>(g_cull_by_pass[i].load()));
                          return bp;
                      }(), g_cull_pass_mask.load());
        return b;
    }
    if (sub == "belt") {  // skel belt scan [file]: each LOD 0 geometry of the player's drawable, its bones by weight (run 10 item 9b)
        std::string x, file;
        in >> x >> file;
        std::string o = "belt scan:";
        for (const auto& slot_d : g_player_drawables) {
        const uintptr_t d = slot_d.load();
        if (!d) continue;
        uintptr_t list = 0, models = 0;
        uint16_t nm = 0;
        if (!rd(d + 0x20 + 0x30, &list) || !list || !rd(list, &models) || !rd(list + 8, &nm)) continue;
        {
            char hb[80];
            std::snprintf(hb, sizeof(hb), " ### drawable %llx: %u models", static_cast<unsigned long long>(d), nm);
            o += hb;
        }
        // the rig's parts it reads, copied under the lock (the render thread rebuilds the rig under it: an outfit change)
        uintptr_t rig_data = 0;
        std::vector<uint8_t> calf(256, 0);
        {
            std::lock_guard lock(g_draw_mutex);
            rig_data = g_rig.data;
            for (int h = 0; h < 2; ++h)
                for (int bi : g_rig.leg_set[h][1])
                    if (bi >= 0 && bi < 256) calf[bi] = 1;
        }
        char b[600];
        for (int mi = 0; mi < nm && mi < 40; ++mi) {
            uintptr_t m = 0, geoms = 0;
            uint8_t bone = 0, skinned = 0;
            uint16_t ng = 0;
            if (!rd(models + mi * 8, &m) || !m) continue;
            rd(m + 0x2b, &bone);
            rd(m + 0x2d, &skinned);
            rd(m + 0x2e, &ng);
            rd(m + 8, &geoms);
            std::snprintf(b, sizeof(b), " || m%d bone %u (%s) skinned %u, %u geometries", mi, bone, bone_name(rig_data, bone).c_str(), skinned, ng);
            o += b;
            for (int gi = 0; geoms && gi < ng && gi < 48; ++gi) {
                uintptr_t g = 0;
                if (!rd(geoms + gi * 8, &g) || !g) continue;
                GeomScan* gs = new GeomScan;
                const bool ok = scan_geom_checked(g, gs);
                if (!ok) {
                    std::snprintf(b, sizeof(b), " | g%d unread (%d)", gi, gs->why);
                    o += b;
                    delete gs;
                    continue;
                }
                // the five heaviest bones, and the legs' share (the calves and feet: trousers or boots, not a belt)
                int top[5] = {-1, -1, -1, -1, -1};
                float total = 0.0f, legs = 0.0f;
                for (int bi = 0; bi < 256; ++bi) {
                    total += gs->w[bi];
                    if (calf[bi]) legs += gs->w[bi];
                    for (int t = 0; t < 5; ++t)
                        if (top[t] < 0 || gs->w[bi] > gs->w[top[t]]) {
                            for (int u = 4; u > t; --u) top[u] = top[u - 1];
                            top[t] = bi;
                            break;
                        }
                }
                std::snprintf(b, sizeof(b), " | g%d v%u i%u box (%.2f %.2f %.2f)..(%.2f %.2f %.2f) c (%.2f %.2f %.2f) legs %.0f%%:", gi, gs->vcount, gs->icount,
                              gs->lo[0], gs->lo[1], gs->lo[2], gs->hi[0], gs->hi[1], gs->hi[2], gs->c[0], gs->c[1], gs->c[2],
                              total > 0 ? 100.0f * legs / total : 0.0f);
                o += b;
                for (int t = 0; t < 5; ++t)
                    if (top[t] >= 0 && gs->w[top[t]] > 0.0f) {
                        std::snprintf(b, sizeof(b), " %s %.0f%%", bone_name(rig_data, top[t]).c_str(), total > 0 ? 100.0f * gs->w[top[t]] / total : 0.0f);
                        o += b;
                    }
                o += " || main:";  // run 12 item 1: each bone's own vertices (its heaviest), with their box
                for (int bi = 0; bi < 256; ++bi)
                    if (gs->nmain[bi]) {
                        std::snprintf(b, sizeof(b), " %s(%d)%u [%.2f %.2f %.2f..%.2f %.2f %.2f]", bone_name(rig_data, bi).c_str(), bi, gs->nmain[bi],
                                      gs->mlo[bi][0], gs->mlo[bi][1], gs->mlo[bi][2], gs->mhi[bi][0], gs->mhi[bi][1], gs->mhi[bi][2]);
                        o += b;
                    }
                delete gs;
            }
        }
        }  // the drawables
        if (!file.empty()) {  // the whole of it (a reply is cut at about 2 KB)
            if (FILE* fp = std::fopen(file.c_str(), "w")) {
                std::fputs(o.c_str(), fp);
                std::fclose(fp);
                return "belt scan written to " + file + " (" + std::to_string(o.size()) + " bytes)";
            }
        }
        return o;
    }
    if (sub == "geoms") {  // skel geoms [on|off]: the hidden-geometry skip; its counts; the player's drawable (reads only)
        std::string x;
        in >> x;
        if (x == "on" || x == "off") g_geom_mode = x == "on" ? 1 : 0;
        const uintptr_t d = g_player_drawable.load();
        std::string o;
        char b[400];
        std::snprintf(b, sizeof(b), "hidden geometry skip %s: draws drawn %llu, skipped %llu, not judged %llu, drawn with hidden torso bones %llu | "
                      "drawable %llx", g_geom_mode.load() ? "on" : "off", static_cast<unsigned long long>(g_geoms_seen[1].load()),
                      static_cast<unsigned long long>(g_geoms_seen[2].load()), static_cast<unsigned long long>(g_geoms_seen[3].load()),
                      static_cast<unsigned long long>(g_geoms_seen[4].load()), static_cast<unsigned long long>(d));
        o += b;
        for (int lod = 0; d && lod < 4; ++lod) {
            uintptr_t list = 0, models = 0;
            uint16_t nm = 0;
            if (!rd(d + 0x20 + 0x30 + lod * 8, &list) || !list || !rd(list, &models) || !rd(list + 8, &nm)) continue;
            std::snprintf(b, sizeof(b), " | lod %d: %u models", lod, nm);
            o += b;
            for (int mi = 0; mi < nm && mi < 12; ++mi) {
                uintptr_t m = 0, geoms = 0;
                uint8_t bone = 0, skinned = 0;
                uint16_t ng = 0;
                if (!rd(models + mi * 8, &m) || !m) continue;
                rd(m + 0x2b, &bone);
                rd(m + 0x2d, &skinned);
                rd(m + 0x2e, &ng);
                rd(m + 8, &geoms);
                std::snprintf(b, sizeof(b), " ; m%d bone %u skinned %u geoms %u:", mi, bone, skinned, ng);
                o += b;
                for (int gi = 0; geoms && gi < ng && gi < 8; ++gi) {
                    uintptr_t g = 0, vb = 0, ib = 0, vdata = 0, idata = 0, pal = 0;
                    uint16_t vcount = 0, mtxc = 0;
                    uint32_t stride = 0, icount = 0, gic = 0;
                    if (!rd(geoms + gi * 8, &g) || !g) continue;
                    rd(g + 0x18, &vb);
                    rd(g + 0x38, &ib);
                    rd(g + 0x58, &gic);
                    rd(g + 0x68, &pal);
                    rd(g + 0x72, &mtxc);
                    if (vb) {
                        rd(vb + 8, &vcount);
                        rd(vb + 0x18, &stride);
                        rd(vb + 0x20, &vdata);
                    }
                    if (ib) {
                        rd(ib + 8, &icount);
                        rd(ib + 0x10, &idata);
                    }
                    uint8_t probe[4] = {};
                    const bool vok = vdata && raw(vdata, probe, 4);
                    const bool iok = idata && raw(idata, probe, 4);
                    std::snprintf(b, sizeof(b), " g%d v%u x%u (data %s) i%u/%u (data %s) pal %u", gi, vcount, stride, vok ? "ok" : "none",
                                  icount, gic, iok ? "ok" : "none", pal ? mtxc : 0);
                    o += b;
                    uint8_t cls;
                    {
                        std::lock_guard lock(g_draw_mutex);  // the rig is rebuilt under it on the render thread
                        cls = classify_geom(pal, mtxc, g_frame.show ? g_frame.show : 1);
                    }
                    uint16_t pb[32] = {};
                    if (pal && mtxc && mtxc <= 32 && raw(pal, pb, mtxc * 2u)) {
                        o += cls == 2 ? " hidden[" : cls == 4 ? " MIXED-TORSO[" : cls == 1 ? " shown[" : " ?[";
                        for (int i = 0; i < mtxc; ++i) o += std::to_string(pb[i]) + (i + 1 < mtxc ? "," : "]");
                    }
                }
            }
        }
        for (size_t i = 0; i < o.size(); i += 400) log::info("[body] geoms %zu: %s", i / 400, o.substr(i, 400).c_str());
        return o;
    }
    if (sub == "still") {  // skel still [reset]: what of the drawn body moved against the camera (mm) since the last reset
        std::string v;
        in >> v;
        const int st = stance();
        std::lock_guard lock(g_draw_mutex);
        if (!g_still_init || v == "reset") {
            still_reset();
            if (v == "reset") return "still diagnostic reset";
        }
        static const char* const kName[kStillDiag] = {"pelvis", "spine00", "spine03", "clavicle_l", "clavicle_r", "arm_l", "arm_r", "head", "foot_l", "foot_r", "holster"};
        char b[200];
        std::snprintf(b, sizeof(b), "still: stance %d, %llu frames, sets pinned %llu, refused %llu, frames not under the camera %llu |", st,
                      static_cast<unsigned long long>(g_still_n), static_cast<unsigned long long>(g_still_sets),
                      static_cast<unsigned long long>(g_still_refused), static_cast<unsigned long long>(g_still_off_cam.load()));
        std::string r = b;
        std::snprintf(b, sizeof(b), " the offset re-learned %llu (riding %llu), kept riding %llu (BodyWithCamera %d) |",
                      static_cast<unsigned long long>(g_still_resets.load()), static_cast<unsigned long long>(g_still_resets_riding.load()),
                      static_cast<unsigned long long>(g_still_kept_riding.load()), g_body_with_cam.load() ? 1 : 0);
        r += b;
        std::snprintf(b, sizeof(b), " knees L %.1f (%.1f..%.1f) R %.1f (%.1f..%.1f) deg, hips over the feet L %.3f R %.3f m, feet off their planted place L %.4f R %.4f m |",
                      g_knee_deg[0], g_knee_rng[0][0], g_knee_rng[0][1], g_knee_deg[1], g_knee_rng[1][0], g_knee_rng[1][1], g_hip_h[0], g_hip_h[1],
                      g_feet_err[0], g_feet_err[1]);
        r += b;
        for (int i = 0; i < kStillDiag; ++i) {
            const float* q = g_still_rng[i];
            if (q[0] > q[3]) continue;
            std::snprintf(b, sizeof(b), " %s range %.1f %.1f %.1f mm at (%.3f %.3f %.3f) |", kName[i], (q[3] - q[0]) * 1000.0f, (q[4] - q[1]) * 1000.0f,
                          (q[5] - q[2]) * 1000.0f, (q[0] + q[3]) * 0.5f, (q[1] + q[4]) * 0.5f, (q[2] + q[5]) * 0.5f);
            r += b;
        }
        return r;
    }
    if (sub == "stretch") {  // skel stretch: each arm's last stretch, its reach and how far the drawn wrist misses
        const HandCfg hc = hand_cfg();
        std::lock_guard lock(g_draw_mutex);
        char b[400];
        int o = std::snprintf(b, sizeof(b), "arm stretch %s, at most %.2f, stretched draws %llu", hc.stretch ? "on" : "off", hc.stretch_max,
                              static_cast<unsigned long long>(g_stretches.load()));
        for (int h = 0; h < 2 && o > 0 && o < static_cast<int>(sizeof(b)); ++h) {
            const float* d = g_ik_diag[h];
            const float mx = d[16] - d[9], my = d[17] - d[10], mz = d[18] - d[11];
            o += std::snprintf(b + o, sizeof(b) - o, " | %s k %.3f dl %.3f reach %.3f miss %.3f", h ? "R" : "L", d[19], d[14], d[12] + d[13],
                               std::sqrt(mx * mx + my * my + mz * mz));
        }
        return b;
    }
    if (sub == "piece" || sub == "pieces") {  // skel pieces [on|off] | skel piece <i|key> <dx dy dz> [tilt turn lean]: run 12 item 2 (the session)
        std::string x;
        in >> x;
        if (sub == "pieces" && (x == "on" || x == "off")) body::set_own_pieces(x == "on", false);
        if (sub == "pieces" && x == "rigid") {  // skel pieces rigid on|off: [Holsters] OwnBeltRigid (the session)
            std::string y;
            in >> y;
            if (y == "on" || y == "off") g_rigid_pieces = y == "on";
        }
        if (sub == "pieces" && x == "scan") body::pieces_geom_scan();
        if (sub == "pieces" && x == "outside") {  // skel pieces outside on|off: the sun shadow's draws of the moved pieces (test)
            std::string y;
            in >> y;
            if (y == "body") {  // skel pieces outside body on|off: every geometry with the eyes' correction, or the moved pieces only
                in >> y;
                if (y == "on" || y == "off") g_shadow_body = y == "on";
            } else if (y == "on" || y == "off") {
                g_pieces_outside = y == "on";
            }
        }
        if (sub == "piece" && !x.empty()) {
            int i = std::isdigit(static_cast<unsigned char>(x[0])) ? std::atoi(x.c_str()) : -1;
            for (int k = 0; i < 0 && k < body::kPieces; ++k)
                if (x == body::piece_key(k)) i = k;
            if (i < 0 || i >= body::kPieces) return "piece: <0-4|Holster|Sheath|Satchel|Lasso|Sling> <dx dy dz> [tilt turn lean]";
            body::Piece p;
            in >> p.d[0] >> p.d[1] >> p.d[2] >> p.turn[0] >> p.turn[1] >> p.turn[2];
            body::set_piece(i, p, false);
        }
        std::lock_guard lock(g_draw_mutex);
        std::string o = std::string("pieces ") + (g_own_pieces.load() ? "on" : "off") + " (sets " + std::to_string(g_piece_sets.load()) + "; rigid " +
                        (g_rigid_pieces.load() ? "on" : "off") + ", drawn " + std::to_string(g_rigid_draws.load()) + ", geometries";
        for (int k = 0; k < g_pgeom_n.load() && k < 8; ++k)
            o += " " + std::string(body::piece_key(g_pgeom[k].piece)) + ":g" + std::to_string(g_pgeom[k].gi.load()) + "/" + std::to_string(g_pgeom[k].nb);
        o += ", scans " + std::to_string(g_pgeom_scans.load()) + "; by pass rigid/untransformed/show/outside";
        for (int ps = 0; ps < 8; ++ps)
            if (g_pgeom_pass[ps][0].load() || g_pgeom_pass[ps][1].load() || g_pgeom_pass[ps][2].load() || g_pgeom_pass[ps][3].load())
                o += " " + std::to_string(ps) + ":" + std::to_string(g_pgeom_pass[ps][0].load()) + "/" + std::to_string(g_pgeom_pass[ps][1].load()) + "/" +
                     std::to_string(g_pgeom_pass[ps][2].load()) + "/" + std::to_string(g_pgeom_pass[ps][3].load());
        o += "; untransformed: in an entity call " + std::to_string(g_pu_in[1].load()) + " outside " + std::to_string(g_pu_in[0].load()) +
             ", the last player set " + std::to_string(g_pu_last[1].load()) + " another " + std::to_string(g_pu_last[0].load()) + ", no set " +
             std::to_string(g_pu_nopc.load()) + ", this set an older frame's " + std::to_string(g_pu_stale.load()) + ", outside draws on an entity call's thread " +
             std::to_string(g_out_thread[1].load()) + " other " + std::to_string(g_out_thread[0].load()) + ", while the set was edited " +
             std::to_string(g_out_busy.load()) + ", callers";
        for (int i = 0; i < 4; ++i)
            if (const uintptr_t r = g_pu_ra[i].load()) {
                char h[48];
                std::snprintf(h, sizeof(h), " +0x%llx x%llu", static_cast<unsigned long long>(r), static_cast<unsigned long long>(g_pu_ra_n[i].load()));
                o += h;
            }
        o += "); shadow body " + std::string(g_shadow_body.load() ? "on" : "off") + " draws " + std::to_string(g_shadow_diag.draws) + " stale " +
             std::to_string(g_shadow_diag.stale) + " hands " + std::to_string(g_shadow_hands.load());
        if (g_shadow_diag.frame && g_eye_pelvis.frame) {  // the outside draw's pelvis against the eyes' (world), and its turn from the game's
            const ShadowDiag& sd = g_shadow_diag;
            const EyePelvis& ep = g_eye_pelvis;
            const float d[3] = {sd.pelvis[0] - ep.p[0], sd.pelvis[1] - ep.p[1], sd.pelvis[2] - ep.p[2]};
            const float la = std::sqrt(sd.ax[0] * sd.ax[0] + sd.ax[1] * sd.ax[1] + sd.ax[2] * sd.ax[2]),
                        le = std::sqrt(ep.ax[0] * ep.ax[0] + ep.ax[1] * ep.ax[1] + ep.ax[2] * ep.ax[2]);
            const float c = la > 1e-6f && le > 1e-6f ? (sd.ax[0] * ep.ax[0] + sd.ax[1] * ep.ax[1] + sd.ax[2] * ep.ax[2]) / (la * le) : 1.0f;
            char t[420];
            const float* gp = g_shadow_game_pel;
            const float dg[3] = {gp[0] - ep.g[0], gp[1] - ep.g[1], gp[2] - ep.g[2]}, de[3] = {ep.p[0] - ep.g[0], ep.p[1] - ep.g[1], ep.p[2] - ep.g[2]};
            std::snprintf(t, sizeof(t),
                          " (frame %llu, the eyes' %llu): pelvis off the eyes' %.1f mm %.1f deg, from the game's %.1f mm %.1f deg | the game's pelvis: the "
                          "shadow's off the eyes' %.1f mm; the eyes' drawn off it %.1f mm; the shadow's (%.3f %.3f %.3f) the eyes' (%.3f %.3f %.3f)",
                          static_cast<unsigned long long>(sd.frame), static_cast<unsigned long long>(ep.frame),
                          1000.0f * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), std::acos(std::fmax(-1.0f, std::fmin(1.0f, c))) * 57.29578f,
                          sd.move_mm, sd.turn_deg, 1000.0f * std::sqrt(dg[0] * dg[0] + dg[1] * dg[1] + dg[2] * dg[2]),
                          1000.0f * std::sqrt(de[0] * de[0] + de[1] * de[1] + de[2] * de[2]), gp[0], gp[1], gp[2], ep.g[0], ep.g[1], ep.g[2]);
            o += t;
        }
        o += ":";
        char b[240];
        for (int i = 0; i < body::kPieces; ++i) {
            const body::Piece& p = g_piece[i];
            const PieceDiag& d = g_piece_diag[i];
            const float e = d.ok ? std::sqrt((d.got[0] - d.want[0]) * (d.got[0] - d.want[0]) + (d.got[1] - d.want[1]) * (d.got[1] - d.want[1]) +
                                             (d.got[2] - d.want[2]) * (d.got[2] - d.want[2]))
                                 : -1.0f;
            std::snprintf(b, sizeof(b), " | %d %s bone %d: d (%.3f %.3f %.3f) turn (%.1f %.1f %.1f) want (%.3f %.3f %.3f) got (%.3f %.3f %.3f) off %.4f m moved (%.4f %.4f %.4f)",
                          i, body::piece_key(i), g_rig.piece[i], p.d[0], p.d[1], p.d[2], p.turn[0], p.turn[1], p.turn[2], d.want[0], d.want[1],
                          d.want[2], d.got[0], d.got[1], d.got[2], e, d.want[0] - d.piv[0], d.want[1] - d.piv[1], d.want[2] - d.piv[2]);
            o += b;
            if (const OutDiag& od = g_out_diag[i]; od.frame) {  // the sun shadow's draw (outside) against the eyes'
                float dl = -1.0f, da = -1.0f;
                if (od.pose_ok && d.pose_ok) {
                    dl = 1000.0f * std::sqrt((od.loc[0] - d.loc[0]) * (od.loc[0] - d.loc[0]) + (od.loc[1] - d.loc[1]) * (od.loc[1] - d.loc[1]) +
                                             (od.loc[2] - d.loc[2]) * (od.loc[2] - d.loc[2]));
                    float tr = 0.0f;  // trace(rel_eyes^T rel_outside)
                    for (int k = 0; k < 9; ++k) tr += d.rel[k] * od.rel[k];
                    da = std::acos(std::fmax(-1.0f, std::fmin(1.0f, (tr - 1.0f) * 0.5f))) * 57.29578f;
                }
                const uint64_t sf = g_out_sent_frame[i].load(std::memory_order_relaxed);
                std::snprintf(b, sizeof(b), " outside joint err %.2f mm shift %.1f mm pose vs eyes %.1f mm %.2f deg age %lld sent %.2f mm age %lld", od.err_mm,
                              od.shift_mm, dl, da, static_cast<long long>(g_body_frame - od.frame),
                              sf ? g_out_sent_mm[i].load(std::memory_order_relaxed) : -1.0f, sf ? static_cast<long long>(g_body_frame - sf) : -1ll);
                o += b;
            }
        }
        return o;
    }
    if (sub == "lefthol") {  // skel lefthol [on|off] [move dx dy dz tilt turn lean]: run 12 item 5 (the session)
        std::string x;
        in >> x;
        if (x == "on" || x == "off") body::set_left_holster(x == "on", false);
        if (x == "mode") {
            int m = 0;
            in >> m;
            g_lh_mode.store(m < 0 || m > 2 ? 0 : m);
        }
        if (x == "move") {
            body::Piece p;
            in >> p.d[0] >> p.d[1] >> p.d[2] >> p.turn[0] >> p.turn[1] >> p.turn[2];
            body::set_left_holster_piece(p, false);
        }
        if (x == "on" || x == "scan") left_holster_scan();
        return body::left_holster_status();
    }
    if (sub == "setdump") {  // skel setdump: the last player set's header (0xd4 bytes) as qwords, and where the matrices are
        const uintptr_t m = g_last_player_set.load();
        uint64_t q[27] = {};
        if (!m || !raw(m, q, 0xd0)) return "no set";
        char b[1400];
        int k = std::snprintf(b, sizeof(b), "set %p (matrices at %p):", reinterpret_cast<void*>(m), reinterpret_cast<void*>(m + 0xd4));
        for (int i = 0; i < 26 && k < static_cast<int>(sizeof(b)) - 40; ++i) k += std::snprintf(b + k, sizeof(b) - k, " +%02x %016llx", i * 8, static_cast<unsigned long long>(q[i]));
        return b;
    }
    if (sub == "boneshift") {  // skel boneshift <bone name|index> <dx> <dy> <dz> [more bones...] | off (run 12 item 1, test)
        std::string x;
        in >> x;
        g_bshift_n.store(0, std::memory_order_release);
        if (x == "off" || x.empty()) return "boneshift off (" + std::to_string(g_bshift_draws.load()) + " draws)";
        uintptr_t rig_data = 0;
        {
            std::lock_guard lock(g_draw_mutex);
            rig_data = g_rig.data;
        }
        uint16_t count = 0;
        if (!rig_data || !rd(rig_data + 0x30, &count)) return "boneshift: no rig";
        int n = 0;
        std::string o = "boneshift:";
        while (!x.empty() && n < 8) {
            float d[3] = {};
            in >> d[0] >> d[1] >> d[2];
            int bi = -1;
            if (std::isdigit(static_cast<unsigned char>(x[0]))) bi = std::atoi(x.c_str());
            for (int i = 0; bi < 0 && i < count && i < 256; ++i)
                if (bone_name(rig_data, i) == x) bi = i;
            if (bi < 0) return "boneshift: no bone " + x;
            g_bshift_bone[n] = bi;
            std::memcpy(g_bshift_d[n], d, sizeof(d));
            o += " " + x + "(" + std::to_string(bi) + ")";
            ++n;
            x.clear();
            in >> x;
        }
        g_bshift_n.store(n, std::memory_order_release);
        return o;
    }
    if (sub == "geomshift") {  // skel geomshift <slot|d<hex>> <model> <geom> <dx> <dy> <dz> [noreset] | off: run 12's upload test
        std::string x;
        in >> x;
        if (x == "off" || x.empty()) {
            g_gshift.model.store(0, std::memory_order_release);
            return "geomshift off (" + std::to_string(g_gshift_draws.load()) + " draws moved)";
        }
        int si = std::atoi(x.c_str()), mi = -1, gi = -1;
        if (!x.empty() && x[0] == 'd') {  // a drawable as "skel ownbelt list" prints it (its address's low 24 bits)
            const unsigned long long want = std::strtoull(x.c_str() + 1, nullptr, 16);
            si = -1;
            for (int k = 0; k < 8; ++k)
                if (const uintptr_t dk = g_player_drawables[k].load(); dk && (dk & 0xffffff) == want) si = k;
        }
        float d[3] = {};
        std::string r;
        in >> mi >> gi >> d[0] >> d[1] >> d[2] >> r;
        if (si < 0 || si >= 8 || mi < 0 || gi < 0) return "geomshift: <slot> <model> <geom> <dx> <dy> <dz> [noreset]";
        const uintptr_t dr = g_player_drawables[si].load();
        uintptr_t list = 0, models = 0, m = 0, geoms = 0, g = 0;
        uint16_t nm = 0, ng = 0;
        if (!dr || !rd(dr + 0x20 + 0x30, &list) || !list || !rd(list, &models) || !rd(list + 8, &nm) || mi >= nm || !rd(models + mi * 8, &m) || !m ||
            !rd(m + 0x2e, &ng) || gi >= ng || !rd(m + 8, &geoms) || !geoms || !rd(geoms + gi * 8, &g) || !g)
            return "geomshift: no such geometry";
        GeomScan* gs = new GeomScan;
        const bool ok = scan_geom_checked(g, gs);
        uint8_t bones[16] = {};
        int nb = 0;
        std::string o = "geomshift: d" + std::to_string(si) + " m" + std::to_string(mi) + " g" + std::to_string(gi) + " bones";
        for (int b = 0; ok && b < 256 && nb < 16; ++b)
            if (gs->w[b] > 0.0f) {
                bones[nb++] = static_cast<uint8_t>(b);
                o += " " + std::to_string(b);
            }
        delete gs;
        if (!ok || !nb) return "geomshift: the geometry unread";
        g_gshift.model.store(0, std::memory_order_release);
        std::memcpy(g_gshift.bones, bones, sizeof(bones));
        std::memcpy(g_gshift.d, d, sizeof(d));
        g_gshift.nb.store(nb, std::memory_order_relaxed);
        g_gshift.reset.store(r != "noreset", std::memory_order_relaxed);
        g_gshift.gi.store(static_cast<uint32_t>(gi), std::memory_order_relaxed);
        g_gshift.model.store(m, std::memory_order_release);
        return o + (r == "noreset" ? " (no reset)" : " (stamps reset)");
    }
    if (sub == "props") {
        // skel props [clear]: the first props matched since the last clear: bone, count, record flags, the set's
        // first matrix translation and x axis, the record's
        std::string v;
        in >> v;
        std::lock_guard lock(g_draw_mutex);
        if (v == "clear") {
            g_nprop_diag = 0;
            g_skinned_bone = -1;
        }
        std::string r = "props (the belt's twins hidden " + std::to_string(g_belt_twins.load()) + "):";
        char b[420];
        for (int i = 0; i < g_nprop_diag; ++i) {
            const PropDiag& d = g_prop_diag[i];
            std::snprintf(b, sizeof(b), " | bone %d%s count %u flags %#x set t (%.2f %.2f %.2f) x (%.2f %.2f %.2f) rec t (%.2f %.2f %.2f) x (%.2f %.2f %.2f) "
                          "z (%.2f %.2f %.2f) corrected x (%.2f %.2f %.2f) z (%.2f %.2f %.2f)",
                          d.bone, d.pinned ? " (pinned)" : "", d.count, d.flags, d.set_t[0], d.set_t[1], d.set_t[2], d.set_x[0], d.set_x[1], d.set_x[2],
                          d.rec_t[0], d.rec_t[1], d.rec_t[2], d.rec_x[0], d.rec_x[1], d.rec_x[2], d.rec_z[0], d.rec_z[1], d.rec_z[2],
                          d.cor ? d.cor_x[0] : 0.0f, d.cor ? d.cor_x[1] : 0.0f, d.cor ? d.cor_x[2] : 0.0f, d.cor ? d.cor_z[0] : 0.0f,
                          d.cor ? d.cor_z[1] : 0.0f, d.cor ? d.cor_z[2] : 0.0f);
            r += b;
        }
        if (g_skinned_bone >= 0) {
            std::snprintf(b, sizeof(b), " || skinned prop on bone %d, %d matrices (t; x):", g_skinned_bone, g_skinned_n);
            r += b;
            for (int i = 0; i < g_skinned_n; ++i) {
                const float* m = g_skinned_diag[i];
                std::snprintf(b, sizeof(b), " [%d] (%.3f %.3f %.3f; %.2f %.2f %.2f)", i, m[3], m[7], m[11], m[0], m[4], m[8]);
                r += b;
            }
            r += " | set header floats:";
            for (int i = 0; i < 52; ++i) {
                std::snprintf(b, sizeof(b), " %d:%.3f", i * 4, g_skinned_hdr[i]);
                r += b;
            }
            r += " | record floats:";
            for (int i = 0; i < 48; ++i) {
                std::snprintf(b, sizeof(b), " %d:%.3f", i * 4, g_skinned_rec[i]);
                r += b;
            }
            const float* m = g_player_bone_diag;
            std::snprintf(b, sizeof(b), " | player set bone %d (%.3f %.3f %.3f; %.2f %.2f %.2f)", g_skinned_bone, m[3], m[7], m[11], m[0], m[4], m[8]);
            r += b;
        }
        if (v.size() > 2 && v != "clear") {  // skel props <file>: the whole dump to a file (replies are cut at 512)
            FILE* f = nullptr;
            if (fopen_s(&f, v.c_str(), "w") == 0 && f) {
                std::fputs(r.c_str(), f);
                std::fclose(f);
                return "written to " + v;
            }
        }
        return r;
    }
    if (sub == "body" || sub == "hide") {
        // skel body on|off|auto: the first-person body forced on or off, or with the camera anchor (the default)
        std::string v;
        in >> v;
        if (v == "on" || v == "off" || v == "auto") g_override = v == "on" ? 1 : v == "off" ? 2 : 0;
        if (v == "stillbelt") {  // skel body stillbelt on|off: [Body] StillBelt for the session (run 10 item 9b)
            std::string s;
            in >> s;
            std::lock_guard lock(g_cfg_mutex);
            g_cfg_still_belt = s == "on";
            log::info("[body] the belt and holster still with the still body: %s", g_cfg_still_belt ? "on" : "off");
        }
        if (v == "autoshow") {  // skel body autoshow on|off: [Body] AutoShow for the session (run 12 item 6's riding check)
            std::string s;
            in >> s;
            set_auto_shows(s == "on");
        }
        if (v == "stillfeet") {  // skel body stillfeet on|off: [Body] StillFeet for the session (run 10 item 9 stage 2)
            std::string s;
            in >> s;
            std::lock_guard lock(g_cfg_mutex);
            g_cfg_still_feet = s == "on";
            log::info("[body] the still body's feet where the game planted them: %s", g_cfg_still_feet ? "on" : "off");
        }
        if (v == "stance") {
            std::string s;
            in >> s;
            set_stance(s == "game" ? 0 : s == "lean" ? 2 : s == "still" ? 3 : 1);
        } else if (v == "back") {
            float m = 0;
            in >> m;
            set_body_back(m);
        } else if (v == "follow") {
            std::string s;
            in >> s;
            set_follows_head(s == "on");
        } else if (v == "ik") {
            std::string x;
            in >> x;
            HandCfg hc = hand_cfg();
            hc.ik = x == "on";
            set_hand_cfg(hc);
        } else if (v == "torso") {
            std::string x;
            in >> x;
            set_locks_torso(x == "on");
        } else if (v == "show") {
            std::string x;
            in >> x;
            HandCfg hc = hand_cfg();
            hc.show = x == "arms" ? 1 : x == "hands" ? 2 : 0;
            set_hand_cfg(hc);
        } else if (v == "iktest") {
            std::string x;
            in >> x;
            g_ik_test = x == "on";
        } else if (v == "collapse") {  // skel body collapse <scale> | collapse root on|off (the session only)
            std::string x;
            in >> x;
            if (x == "root") {
                std::string y;
                in >> y;
                g_collapse_root_cfg = y == "on";
            } else if (!x.empty()) {
                const float cs = std::strtof(x.c_str(), nullptr);
                g_collapse_cfg = !(cs >= 1e-9f) ? 1e-9f : cs > 1e-2f ? 1e-2f : cs;  // up to 1e-2 for a test
            }
            char o[200];
            std::snprintf(o, sizeof(o), "collapse %.0e, root %s", g_collapse_cfg.load(), g_collapse_root_cfg.load() ? "on" : "off");
            return o;
        } else if (v == "wind") {  // skel body wind | wind still on|off | wind test <y>|off (the session only)
            std::string x, y;
            in >> x >> y;
            if (x == "still") g_wind_still_cfg = y == "on";
            if (x == "test") g_wind_test = y == "off" || y.empty() ? -1.0f : std::strtof(y.c_str(), nullptr);
            int id = 0;
            uintptr_t at = 0;
            float w[4] = {};
            const bool ok = wind_param(&id, &at) && raw(at, w, 16);
            char o[300];
            std::snprintf(o, sizeof(o), "wind: id %d, gWindParams %s(%.3f %.4f %.2f %.2f), holds %llu, still %s, test %.2f", id, ok ? "" : "UNREAD ",
                          w[0], w[1], w[2], w[3], static_cast<unsigned long long>(g_wind_holds.load()), g_wind_still_cfg.load() ? "on" : "off",
                          g_wind_test.load());
            return o;
        } else if (v == "stretch") {  // skel body stretch on|off|<max>
            std::string x;
            in >> x;
            HandCfg hc = hand_cfg();
            if (x == "on" || x == "off")
                hc.stretch = x == "on";
            else if (!x.empty())
                hc.stretch_max = std::strtof(x.c_str(), nullptr);
            set_hand_cfg(hc);
        } else if (v == "lock") {
            std::string x;
            in >> x;
            set_locks_facing(x == "on");
        } else if (v == "pitch") {
            float d = 0;
            in >> d;
            set_torso_pitch(d);
        } else if (v == "hidehead") {
            std::string s;
            in >> s;
            set_hide_enabled(s == "on");
        }
        char out[600];
        std::snprintf(out, sizeof(out),
                      "body %s (override %d), posture flags %u (auto show %s), head hide %s, stance %d, back %.2f m, follows head %s, "
                      "upright bone %d, skeleton %p, sets %llu, hat hides %llu, props moved %llu, rigid props %llu | props owned by the "
                      "player's skeleton %llu, by another %llu (on the head %llu / %llu), foreign hats left %llu, his hat not kept %llu",
                      g_active.load() ? "on" : "off", g_override.load(), g_actor_flags.load(), auto_shows() ? "on" : "off",
                      hide_enabled() ? "on" : "off", stance(), body_back(),
                      follows_head() ? "on" : "off", g_rig.up, reinterpret_cast<void*>(player_skeleton()),
                      static_cast<unsigned long long>(g_sets.load()), static_cast<unsigned long long>(g_hat_hides.load()),
                      static_cast<unsigned long long>(g_props.load()), static_cast<unsigned long long>(g_rigid_props.load()),
                      static_cast<unsigned long long>(g_prop_own_skel.load()), static_cast<unsigned long long>(g_prop_other_owner.load()),
                      static_cast<unsigned long long>(g_prop_head_own.load()), static_cast<unsigned long long>(g_prop_head_other.load()),
                      static_cast<unsigned long long>(g_head_foreign.load()), static_cast<unsigned long long>(g_head_lost.load()));
        std::string o = out;
        {  // the remembered props: each one's bone, head, and instance (whose they are)
            std::lock_guard lock(g_draw_mutex);
            o += " | remembered props:";
            char b[96];
            for (size_t i = 0; i < g_prop_bones.size() && i < 12; ++i) {
                std::snprintf(b, sizeof(b), " %llx/b%d%s/i%llx", static_cast<unsigned long long>(g_prop_bones[i].drawable & 0xffffff), g_prop_bones[i].bone,
                              g_prop_bones[i].head ? "h" : "", static_cast<unsigned long long>(g_prop_bones[i].inst));
                o += b;
            }
        }
        return o;
    }
    return "ERROR usage: skel q|f|s <addr> [n] | skel bones <file> | skel ik | skel stretch | skel body [on|off|auto|stance upright|lean|still|game|back <m>|follow on|off|hidehead on|off|stretch on|off|<max>]";
}

}  // namespace rdrvr::body
