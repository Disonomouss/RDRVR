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
#include <sstream>
#include <vector>

#include "core/actions.h"
#include "core/aim.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/dual.h"
#include "core/hands.h"
#include "core/held_prop.h"
#include "core/holster.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/pose.h"
#include "core/reload.h"

namespace rdrvr::body {
namespace {

bool readable(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    return a && VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
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
int g_cfg_stance = 1;  // 0 the game's animation, 1 upper body upright, 2 the forward/back lean removed
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
    int holster[body::kHolsterBones] = {-1, -1, -1, -1, -1, -1};  // pistol (right hip), rifle (back), thrower, melee, the chest, the lower back
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
    float yaw = 0;        // radians about the world's up through the root: the facing lock
    bool lock = false;
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
    float barrel_open = 0.0f;  // the barrels' drawn opening (0 shut - 1 open), for the hand's turn with them
    float pump = 0.0f;         // [Reload] PumpParts: the fore-end's drawn travel (0 - 1 of kPumpSlide): the front hand's grip with it
    bool snap = false;         // [Reload] TwoHandedSnap
    float fore_off[3] = {};    // [Reload] ForegripOffset: right, up, forward in the gun's axes
};
Frame g_frame;

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
    actions::kDrvBreech, actions::kDrvCount;  // the drivers
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
    {5, 6, 0, 0, 3, {{3, 0, kDrvOpen, kOwn, -70.0f, {0.0157f, 0.0272f, -0.0585f}, {}, {}}, {4, 0, kDrvOpen, kHinged, -70.0f, {0.0157f, 0.0272f, -0.0585f}, {}, {}},
                     {4, 2, kDrvIndex, kOwn, -60.0f, {0.0f, 0.04092f, -0.01445f}, {}, {}}}},
    {7, 6, 0, 0, 3, {{3, 0, kDrvOpen, kOwn, -70.0f, {0.0063f, 0.0192f, -0.0554f}, {}, {}}, {4, 0, kDrvOpen, kHinged, -70.0f, {0.0063f, 0.0192f, -0.0554f}, {}, {}},
                     {4, 2, kDrvIndex, kOwn, -60.0f, {0.0f, 0.04120f, -0.02906f}, {}, {}}}},
    // the Double-action: its cylinder (bone 2) swung out to the left on a crane beside it (the mod's own, approved by the
    // user), turned a chamber a round about its own centre: the Cattleman's (its frame is the Cattleman's: the flap's and
    // the trigger's pivots within 0.1 mm). Not bone 2's origin (-6.4, 22.1) mm: that is the crane, about which the game's
    // own reload swings the cylinder out 60 degrees (round 13: turned about it, the cylinder went round the gun)
    {6, 6, 0, 0, 2, {{2, 2, kDrvOpen, kOwn, 80.0f, {0.0056f, 0.0101f, -0.0478f}, {}, {}}, {2, 2, kDrvIndex, kOwn, -60.0f, {0.0f, 0.04826f, 0.0f}, {}, {}}}},
    // the Cattleman: its loading gate (bone 4) 50 degrees about the barrel's axis at (15.7, 35.3) mm (a hinge within
    // 0.04 mm in its own reload); its cylinder (bone 2, the Double-action's place in the same skeleton; it never turns in
    // the game's clips) a chamber a round
    {4, 6, 0, 0, 2, {{4, 2, kDrvOpen, kOwn, -50.0f, {0.0157f, 0.0353f, 0.0f}, {}, {}}, {2, 2, kDrvIndex, kOwn, -60.0f, {0.0f, 0.04826f, 0.0f}, {}, {}}}},
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
            const float sl[3] = {P.slide[0] * a + P.t0[0], P.slide[1] * a + P.t0[1], P.slide[2] * a + P.t0[2]};
            float* m = base + P.bone * 12;
            if (P.base == kHinged) {
                float out[12];
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) out[i * 4 + j] = G[i * 4] * m[j] + G[i * 4 + 1] * m[4 + j] + G[i * 4 + 2] * m[8 + j];
                    out[i * 4 + 3] = G[i * 4] * m[3] + G[i * 4 + 1] * m[7] + G[i * 4 + 2] * m[11] + G[i * 4 + 3];
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
// run 6 item 9: the gun in hand whose action the player works (actions::parts_weapon: the lever guns with LeverParts):
// its model's flags (its own fire clip stopped, the gun steadied through John's fire clip); 0 for none
uint8_t worked_flags() {
    const int w = actions::parts_weapon();
    return w >= 0 ? gun_flags(w) : 0;
}
// John's fire clip, for the steadying: the gun controller's fire phase (G +0x24), or within 2 s of the shot (cycle L2: the
// Bolt Action's bolt cycle swung the drawn gun 97 degrees past that phase; the steadying only puts the drawn gun at the
// controller with its rest offset, so a longer hold is harmless)
constexpr double kShotHoldMs = 2000.0;
bool in_fire_clip() { return holster::fire_clip_phase() >= 0.0f || actions::since_shot_ms() < kShotHoldMs; }
std::atomic<uint64_t> g_lever_draws{0}, g_steady_draws{0}, g_steady_misses{0}, g_clip_stops{0};
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
std::atomic<uintptr_t> g_held_wmgr{0};  // the player's weapon manager (actor +0x70), from the last visibility build
struct Placed {
    uintptr_t W = 0;
    uint64_t build = 0;  // the visibility builds done before it (it belongs to the next one)
    float m[16] = {};
    float A[9] = {}, a[3] = {};
    double ad[3] = {};
    int john = -1;       // the second gun's: John's hand it was placed at
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
std::atomic<uint32_t> g_front_seen[kGripW] = {};  // bit 0/1: the lowered/aiming hold learned; bit 2/3: its fingers
std::atomic<uint64_t> g_front_finger_frames{0};
// a hand's fingers relative to its wrist in a bone set (12 floats a bone: [R | t] row-major), none for a collapsed hand
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
uint64_t g_pub_frame = ~0ull;  // the frame g_hand_seq's data was last published for (the render thread's)
std::vector<Corr> g_corr_set;    // the latest current set's, in the set's own space (the world less its offset)
float g_ik_diag[2][20];          // per hand: S, E, W, T (set space), l1, l2, d, the world target's y, the wrist as drawn, the stretch
float g_cam_diag[12];            // the frame's camera position, the head bone, the root bone, the right wrist target (world)
float g_torso_diag[2] = {0, 0};  // the torso lock's last horizontal correction (m) and the largest since the last read
// The hands' world corrections for the aim (aim.cpp reads them on the game thread): the wrist bones' T_world of the
// latest current player set, written on the render thread behind a sequence counter (odd while writing).
struct HandCorr {
    float A[9], a[3];
    double ad[3];
    double ms;
    bool ik;
};
std::atomic<uint32_t> g_hand_seq{0};
std::atomic<uint64_t> g_seq_waits{0}, g_seq_misses{0};
std::atomic<uintptr_t> g_player_drawable{0};  // rec[0x10] of the player's set draw (the "skel geoms" probe)
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
using RecorderDraw_t = void (*)(uintptr_t rec, uint32_t topo, uint32_t count);
using RecorderDrawAt_t = void (*)(uintptr_t rec, uint32_t topo, uint32_t start, uint32_t count, uint32_t base);
RecorderDraw_t o_rec_draw = nullptr;
void hk_rec_draw(uintptr_t rec, uint32_t topo, uint32_t count) {
    const CullDraw* cd = t_cull_draw;
    if (!cd) return o_rec_draw(rec, topo, count);
    t_cull_recorded = true;
    const auto at = reinterpret_cast<RecorderDrawAt_t>(anchors::addr(anchors::Id::RecorderDrawAt));
    uintptr_t vt = 0;
    if (count != cd->icount || !cd->nruns || topo != 4 || !at || !raw(rec, &vt, sizeof(vt)) || vt != anchors::addr(anchors::Id::RecorderVtbl)) {
        g_cull_mismatch.fetch_add(1, std::memory_order_relaxed);  // another count or topology, or not the recorder: as the game draws it
        return o_rec_draw(rec, topo, count);
    }
    for (int i = 0; i < cd->nruns; ++i) at(rec, topo, cd->runs[i].start, cd->runs[i].count, 0);
    g_cull_filtered.fetch_add(1, std::memory_order_relaxed);
    g_cull_runs.fetch_add(cd->nruns, std::memory_order_relaxed);
    g_cull_by_pass[t_draw_pass & 7].fetch_add(1, std::memory_order_relaxed);
}

void hk_draw_geom(uintptr_t shader, uintptr_t model, uint64_t gi, uintptr_t mset, uint64_t a5, uint64_t a6) {
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
float g_two_diag = 0.0f;         // the two-handed turn's full angle (degrees), the latest
float g_player_offset[3] = {};   // that set's header offset (+0x10: a skinned set's matrices are relative to it)
// the first unflagged (skinned) prop's whole set, and the player set's matrix for the same bone, as drawn
float g_skinned_diag[8][12];
int g_skinned_n = 0;
float g_player_bone_diag[12];
int g_skinned_bone = -1;
float g_skinned_hdr[52];  // the set's bytes 0x00..0xd0 as floats
float g_skinned_rec[48];  // the record's first 0xc0 bytes as floats
bool g_corr_world_valid = false;

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
    if (f.stance && g_rig.chain[0] >= 0 && g_rig.chain[0] < n) {
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
    if (f.stance && g_rig.pelvis >= 0 && g_rig.pelvis < n) {
        float rc[9], piv[3];
        stance_rot(s + g_rig.pelvis * 12, rc);
        jdrawn(g_rig.pelvis, piv);
        for (int b : g_rig.pelvis_set)
            if (b < n) corr_apply(g_corr[b], rc, piv);
    }
    // 1b. the torso position: running pushes spine00 forward of the root; the whole body (legs too, so nothing stretches)
    //     moves horizontally so spine00's joint sits over the root as at rest (round 6: with the pivots right, the
    //     torso came forward again when running)
    if (f.lock_torso && f.actor_valid && g_rig.chain[0] >= 0) {
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
        for (int b = 1; b < n; ++b) corr_apply(g_corr[b], ry, rootj);
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
    bool pub_fore = false;  // the foregrip point for the body points (world), set below
    float pub_fore_p[3] = {};
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
            if (h == fh && f.long_gun && hs && ((!f.xfer && f.gun_j == 1) || (mir && xt_ok)) && g_rig.wrist[0] >= 0 && g_rig.wrist[0] < n &&
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
                        if (!mir && f.ik[1] && (worked_flags() & kSteady)) {
                            float T[12], F[12], ti[12], X[12];
                            for (int i = 0; i < 3; ++i) {
                                for (int j = 0; j < 3; ++j) {
                                    T[i * 4 + j] = f.ik_r[1][i * 3 + j];
                                    F[i * 4 + j] = Gd[i * 3 + j];
                                }
                                T[i * 4 + 3] = f.ik_t[1][i] - off[i];
                                F[i * 4 + 3] = gd_o[i];
                            }
                            const bool clip = in_fire_clip();
                            const float fade = clip ? 0.0f : static_cast<float>((log::now_ms() - g_steady[1].clip_end_ms) / 150.0);
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
            to_set(f.ik_pole[h], pole, false);
            if (h == fh && fore_ok && slot == 0) {
                pub_fore = true;
                for (int k = 0; k < 3; ++k) pub_fore_p[k] = fore_p[k] + off[k];
            }
            if (h == fh && barrel_ok)  // [Reload] BarrelHandSnap: the off hand onto the open barrels, by its weight
                for (int k = 0; k < 3; ++k) T[k] += (barrel_p[k] - T[k]) * f.barrel_blend;
            if (h == fh && part_ok) {  // [Reload] PartHandSnap: the off hand's interaction spot onto the part it grips, by its weight
                float go[3], w[3];
                holster::interact_offset(go);
                const float o[3] = {(h == 0 ? -1.0f : 1.0f) * go[0], go[1], -go[2]};  // as holster::grab_point
                const float* R = f.ik_r[h];
                for (int k = 0; k < 3; ++k) w[k] = part_pt[k] - (R[k * 3] * o[0] + R[k * 3 + 1] * o[1] + R[k * 3 + 2] * o[2]);
                for (int k = 0; k < 3; ++k) T[k] += (w[k] - T[k]) * f.part_blend;
            }
            if (h == fh && snap_ok)  // the front hand onto the gun's grip, by the two-handed weight
                for (int k = 0; k < 3; ++k) T[k] += (fore_p[k] - T[k]) * f.two_blend;
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
            if (h == fh && snap_ok) {  // the hand turned to the game's grip (the drawn wrist is then exactly Md)
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
            compose(w[d], mrel, o);
            put(fd, o);
            if (!xf_fingers || f.copy_grip) continue;  // the copy: the gripping hand keeps its own grip
            if (f.fingers == 2) {
                put(fg, w[g]);  // open: the bind pose's fingers
            } else {
                mirrored(rel[d][i], mrel);  // the game's empty hand's pose onto the other hand
                compose(w[g], mrel, o);
                put(fg, o);
            }
        }
    }
    {  // [Weapon.<Gun>] FrontHandPose: a fixed hold's own fingers on the front hand (John's left; not with the gun in his
       // left, the transplant), blended from the game's by the two-handed hold
        const bool mir_now = f.mirror && f.xfer && f.gun_j == 0 && f.item_side == 1;
        const int fp = f.weapon >= 0 && f.weapon < kGripW ? g_front_pose[f.weapon].load(std::memory_order_relaxed) : 0;
        const FingerRel* fr = fp > 0 ? &g_front_fingers[f.weapon][fp - 1] : nullptr;
        const float b = f.two_blend > 1.0f ? 1.0f : f.two_blend;
        const int wb = g_rig.wrist[0];
        if (fr && fr->valid && f.long_gun && !mir_now && !f.xfer && f.gun_j == 1 && b > 0.0f && wb >= 0 && wb < n) {
            const FingerRel live = finger_rel(s, n, 0);
            float* w = s + wb * 12;
            float WR[9], wt[3];
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) WR[i * 3 + k] = w[i * 4 + k];
                wt[i] = w[i * 4 + 3];
            }
            for (int i = 0; live.valid && i < 15; ++i) {
                const int fb = g_rig.finger[0][i];
                if (fb < 0 || fb >= n || !fr->has[i] || !live.has[i]) continue;
                float M[9], t[3];
                if (b >= 1.0f) {
                    std::memcpy(M, fr->M[i], sizeof(M));
                    std::memcpy(t, fr->t[i], sizeof(t));
                } else {  // from the live finger toward the hold's: its turn and its place by the blend
                    float lt[9], d[9], ax[3], ang = 0.0f, rp[9];
                    for (int a = 0; a < 3; ++a)
                        for (int c = 0; c < 3; ++c) lt[a * 3 + c] = live.M[i][c * 3 + a];
                    mul3(fr->M[i], lt, d);
                    to_axis_angle(d, ax, &ang);
                    axis_angle(ax, ang * b, rp);
                    mul3(rp, live.M[i], M);
                    for (int k = 0; k < 3; ++k) t[k] = live.t[i][k] + (fr->t[i][k] - live.t[i][k]) * b;
                }
                float* m = s + fb * 12;  // F = W rel
                for (int a = 0; a < 3; ++a) {
                    for (int c = 0; c < 3; ++c) m[a * 4 + c] = WR[a * 3 + 0] * M[0 * 3 + c] + WR[a * 3 + 1] * M[1 * 3 + c] + WR[a * 3 + 2] * M[2 * 3 + c];
                    m[a * 4 + 3] = WR[a * 3 + 0] * t[0] + WR[a * 3 + 1] * t[1] + WR[a * 3 + 2] * t[2] + wt[a];
                }
            }
            if (slot == 0 && live.valid) g_front_finger_frames.fetch_add(1, std::memory_order_relaxed);
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
        if (hb && bside >= 0 && bj >= 0 && bj < 2 && f.ik[bj]) {
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
        std::memcpy(g_points.fore, pub_fore_p, sizeof(g_points.fore));
        g_points.gun = f.gun_j;
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
            g_points.tips_ok[h] = tb >= 0 && tb < n && ib >= 0 && ib < n;
            if (!g_points.tips_ok[h]) continue;
            jdrawn(tb, g_points.thumb_tip[h]);
            jdrawn(ib, g_points.index_tip[h]);
            for (int k = 0; k < 3; ++k) {
                g_points.thumb_tip[h][k] += off[k];
                g_points.index_tip[h][k] += off[k];
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
void held_delta(const HeldSample& h, int bone, Corr* w, Corr* s) {
    static const Corr kId = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
    const int sw = h.sec && h.john >= 0 && h.john < 2 ? g_rig.wrist[h.john] : -1;  // the second gun: its own hand's (never the transplant)
    const Corr& now = h.sec ? (sw >= 0 && sw < static_cast<int>(g_corr_set.size()) ? g_corr_set[sw] : kId)
                    : g_item_xfer ? g_item_set[h.left ? 0 : 1]
                    : bone >= 0 && bone < static_cast<int>(g_corr_set.size()) ? g_corr_set[bone] : kId;
    const float* off = g_player_offset;
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

bool hand_correction_d(int h, float A[9], float a[3], bool* ik, double* ad) {
    if (h < 0 || h > 1) return false;
    HandCorr c;
    if (!seq_copy(g_hand_seq, &c, &g_hand_corr[h], sizeof(c))) return false;
    if (!c.ms || log::now_ms() - c.ms > 250) return false;  // no player set drawn lately: not in first person
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
    g_sec_seq.fetch_add(1, std::memory_order_release);
    g_sec_placements.fetch_add(1, std::memory_order_relaxed);
    return true;
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
            g_copy_muzzle_moved.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_copy_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
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
    float wrist_pos[3] = {}, gun_pos[3] = {}, att_pos[3] = {};  // the raw positions read (the matrices' spaces)
    int why = 0;  // the last early return: 1 no wrist bone, 2 the wrist not read, 3 not rigid, 4 not a long gun
};
FixedGripDiag g_fg_diag;
std::atomic<uint64_t> g_fg_aim_frames{0};
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
// the built-in aiming relations, measured in the simulator (none yet: filled from item 1c's measurement)
bool builtin_gun_rel(int w, float* out) {
    (void)w;
    (void)out;
    return false;
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
void fixed_grip(int side, float* src) {
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
    RdrvrActorState ws{};
    const int w = api::actor_state(&ws) && ws.valid ? ws.weapon : -1;
    const bool sidearm = w >= 0 && w <= 7;
    if (!is_long_gun_w(w) && !sidearm) return fail(4);
    const bool fix_on = sidearm ? g_fixed_sidearm.load(std::memory_order_relaxed) : g_fixed_grip.load(std::memory_order_relaxed);
    GunRel& tpl = sidearm ? g_gun_rel_tpl_side : g_gun_rel_tpl;
    mul44r(S, Wi, rel);
    const bool aiming = holster::aim_pose() || aim::aiming();  // the gun controller's aim pose (G +0x5d6 & 0x40), or the zoom
    const uint64_t af = aiming ? g_fg_aim_frames.fetch_add(1, std::memory_order_relaxed) + 1 : (g_fg_aim_frames.store(0), 0);
    std::lock_guard lock(g_fg_mutex);
    FixedGripDiag& d = g_fg_diag;
    if (d.weapon != w) {
        d = FixedGripDiag{};
        d.weapon = w;
    }
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
    if (aiming && af > 20 && rigid_axes(rel) && rel[12] * rel[12] + rel[13] * rel[13] + rel[14] * rel[14] < 1.0f &&
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

// The rope's points draw (render thread): inside the lasso's draw, a corrected copy of its points
void hk_rope_points(uintptr_t lod, uint64_t cam, uintptr_t model, uintptr_t pts, uintptr_t ld, uint32_t p6, uint32_t p7) {
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

uint64_t hk_obj_set_matrix(uintptr_t obj, const float* m) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_gun_place_ret || !m || !g_held_at_hand_cfg.load(std::memory_order_relaxed) ||
        !g_held_fix_cfg.load(std::memory_order_relaxed))
        return o_obj_set_matrix(obj, m);
    const uintptr_t W = reinterpret_cast<uintptr_t>(m) - 0x80;
    {
        int sj = -1;
        uintptr_t sitem = 0;
        if (const uintptr_t sw = dual::secondary_W(&sj, &sitem); sw && sw == W) {
            // re-checked here (the weapon tick's view is the last update's): the slot item still has this W, holstered,
            // and is not the item in hand
            const uintptr_t wmgr = g_held_wmgr.load(std::memory_order_relaxed);
            uintptr_t iw = 0, hand = 0;
            int32_t state = 0;
            if (sitem && raw(sitem + 0xa0, &iw, sizeof(iw)) && iw == W && raw(sitem + 0x24, &state, sizeof(state)) && state == 2 &&
                (!wmgr || (raw(wmgr + 0x80, &hand, sizeof(hand)) && hand != sitem)) && place_secondary(W, const_cast<float*>(m), sj))
                dual::note_placed(W);  // m is W +0x80: written when placed
            return o_obj_set_matrix(obj, m);
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
    float A[9], a[3], src[16];
    bool ik = false;
    double ad[3] = {};
    if (!item_correction(lf ? 0 : 1, A, a, &ik, ad) || !ik || !raw(reinterpret_cast<uintptr_t>(m), src, sizeof(src)))
        return o_obj_set_matrix(obj, m);
    fixed_grip(lf ? 0 : 1, src);  // run 7 item 1c: measured always; with [Hands] FixedGunGrip the long gun held by its aiming hold
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
    g_placed_seq.fetch_add(1, std::memory_order_acq_rel);  // odd while writing
    g_placed.W = W;
    g_placed.build = g_vis_builds.load(std::memory_order_relaxed);
    std::memcpy(g_placed.m, g_place_buf, sizeof(g_placed.m));
    std::memcpy(g_placed.A, A, sizeof(A));
    std::memcpy(g_placed.a, a, sizeof(a));
    std::memcpy(g_placed.ad, ad, sizeof(ad));
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
    float set_t[3], rec_t[3], set_x[3], rec_x[3];
};
PropDiag g_prop_diag[8];
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
};
struct FollowSample {
    uint64_t frame;
    int nw, nobj[3];          // the pass's draws seen this frame: the player's sets, then the gun in hand, the own model, the copy
    bool w_ok[2][2];          // [eye][hand]
    float w[2][2][12];
    FollowObj obj[3][2];      // [object][eye]
};
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
}
// each object in its wrist's frame (X = W^-1 O) against the first such frame of its gun: the move (mm) and turn (deg);
// the wrist's own speed (mm a frame), to tell a gun a frame late (its error grows with the speed) from an offset. Under
// g_draw_mutex; each line logged with `tag`
std::string follow_report(const char* tag) {
    std::string o;
    char b[400];
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
        std::snprintf(b, sizeof(b), "\n  %s (weapon %d), eye %d: %d frames (stale %d), worst %.1f mm %.2f deg, rms %.1f mm | the wrist up to %.0f mm a frame; worst at over 20 mm a frame (%d frames) %.1f mm, under 5 mm a frame %.1f mm",
                      name, wl, e, n, nstale, worst_t, worst_a, std::sqrt(sum2 / n), wsp_max, nfast, fast_t, slow_t);
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

void hk_DrawVisEntity(void* ctx, void** rec, uint64_t pass, uint64_t bucket, uint64_t a5, uint64_t a6, uint64_t a7) {
    t_draw_pass = pass;
    CopyJob cj;  // [Hands] DualWieldCopy: the held item drawn again at the free hand after its own draw
    thread_local std::vector<float> save;
    float wind_y = -1.0f;  // >= 0: this draw's cloth flutter wind (gWindParams.y), set around it
    float* set_at = nullptr;
    size_t set_n = 0;
    float* rec_at = nullptr;
    float rec_save[32];  // the record's two matrices (+0x00 drawn, +0x40 last frame's), restored after the draw
    int cull_show = 0;   // the player's set in a show mode: its all-hidden geometries are skipped (HiddenGeometry)
    if (g_frame.valid && rec && rec[0x12]) {
        uintptr_t mset = reinterpret_cast<uintptr_t>(rec[0x12]);
        uint8_t count = 0;
        if (raw(mset, &count, 1)) {
            float* sm = reinterpret_cast<float*>(mset + 0xd4);
            const size_t n = static_cast<size_t>(count) * 24;  // the current and the previous set, 12 floats a matrix
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
            if (count > g_rig.max_index && count <= g_rig.count && rec[0x13] &&
                raw(reinterpret_cast<uintptr_t>(rec[0x13]) + 0x18, &owner, 8) && owner == g_frame.skel) {
                g_player_drawable.store(reinterpret_cast<uintptr_t>(rec[0x10]), std::memory_order_relaxed);
                if (g_frame.show && o_draw_geom && g_geom_mode.load(std::memory_order_relaxed) != 0) cull_show = g_frame.show;
                std::lock_guard lock(g_draw_mutex);
                save.assign(sm, sm + n);
                set_at = sm;
                set_n = n;
                // the sets' header offsets: +0x10 the current, +0x20 the previous (the matrices are the world less it)
                const float* off_cur = reinterpret_cast<const float*>(mset + 0x10);
                const float* off_prev = reinterpret_cast<const float*>(mset + 0x20);
                transform_set(sm, count, 0, g_frame, off_cur);
                transform_set(sm + count * 12, count, 1, g_frame, off_prev);  // the previous frame's (motion vectors)
                for (int h = 0; h < 2; ++h) {  // [Reload] LeverParts: each wrist as drawn (the held gun kept in it)
                    Steady& sd = g_steady[h];
                    const int wb = g_rig.wrist[h];
                    sd.wrist_ok = wb >= 0 && wb < count;
                    if (sd.wrist_ok) std::memcpy(sd.Sw, sm + wb * 12, sizeof(sd.Sw));
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
                if (g_frame.show && g_frame.still_cloth)
                    wind_y = 0.0f;
                else if (const float wt = g_wind_test.load(std::memory_order_relaxed); wt >= 0.0f)
                    wind_y = wt;
                std::memcpy(g_player_offset, reinterpret_cast<const void*>(mset + 0x10), sizeof(g_player_offset));
                g_sets.fetch_add(1, std::memory_order_relaxed);
            } else if (count >= 1 && count <= 8) {
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
                const bool hp = hi < 0 && si < 0 && held_prop::match(reinterpret_cast<uintptr_t>(rec[0x10]), rc + 12, hp_pose, &hp_slot);
                if (hp && hp_slot == 1) {  // [Hands] OwnModelFollow: the other sidearm at the copy's hand of this frame
                    std::lock_guard lock(g_draw_mutex);
                    hp_john = own_follow_pose(hp_pose);
                }
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
                    float dx = rc[12] - ap[i * 3], dy = rc[13] - ap[i * 3 + 1], dz = rc[14] - ap[i * 3 + 2];
                    float d = dx * dx + dy * dy + dz * dz;
                    if (d < best_d) {
                        best_d = d;
                        best = g_rig.attach[i];
                    }
                }
                const uintptr_t drawable = reinterpret_cast<uintptr_t>(rec[0x10]);
                {
                    std::lock_guard lock(g_draw_mutex);
                    const bool head_5cm = g_frame.held_fix;  // [Body] HeldPropFix: also this rule
                    if (head_5cm && best >= 0 && g_rig.in_head[best]) {
                        // a head prop is one first matched within 5 cm of a head bone (the hat: 0-1 cm); anything else
                        // near the head (a thrown stick or a knife in the game's overhand throw, a projectile) is not
                        // hidden with it
                        bool known_head = false;
                        for (const PropBone& pb : g_prop_bones)
                            if (pb.drawable == drawable && pb.head) known_head = true;
                        if (!known_head && best_d > 0.05f * 0.05f) {
                            best = -1;
                            g_head_refused.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (best >= 0) {
                        bool found = false;
                        for (PropBone& pb : g_prop_bones)
                            if (pb.drawable == drawable) {
                                // a prop once on the head stays a head prop: the hat bobs between head_Attachment and
                                // the nearer neck_Attachment while sprinting (round 5c: it flickered)
                                if ((head_5cm ? pb.head : g_rig.in_head[pb.bone] != 0) && !g_rig.in_head[best]) best = pb.bone;
                                else if (g_rig.in_head[best] && best_d <= 0.05f * 0.05f) pb.head = true;
                                pb.bone = best;
                                pb.frame = g_body_frame;
                                found = true;
                            }
                        if (!found && drawable)
                            g_prop_bones.push_back({drawable, best, g_body_frame, g_rig.in_head[best] && (!head_5cm || best_d <= 0.05f * 0.05f)});
                    } else if (drawable) {
                        for (PropBone& pb : g_prop_bones) {
                            if (pb.drawable != drawable) continue;
                            for (size_t i = 0; i * 3 + 2 < ap.size(); ++i) {
                                if (g_rig.attach[i] != pb.bone) continue;
                                float dx = rc[12] - ap[i * 3], dy = rc[13] - ap[i * 3 + 1], dz = rc[14] - ap[i * 3 + 2];
                                if (dx * dx + dy * dy + dz * dz < 0.5f * 0.5f) {
                                    best = pb.bone;
                                    pb.frame = g_body_frame;
                                }
                            }
                        }
                    }
                }
                }  // not the held item
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
                        }
                    }
                    save.assign(sm, sm + n);
                    set_at = sm;
                    set_n = n;
                    std::memcpy(rec_save, rc, sizeof(rec_save));
                    rec_at = rc;
                    const bool part_hidden = g_frame.show && !(g_frame.show == 1 ? g_rig.in_elbow[best] : g_rig.in_wrist[best]);
                    if ((g_frame.hide && g_rig.in_head[best]) || part_hidden) {
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
                            if (xf) mask &= ~(1u << kDrvLever | 1u << kDrvHammer);  // [Reload] LeverParts: not on a transplanted gun
                            const unsigned done = mask ? gun_parts(sm, count, gw, drv, mask) : 0u;
                            if (done & 1u << kDrvOpen) g_open_draws.fetch_add(1, std::memory_order_relaxed);
                            if (done & (1u << kDrvLever | 1u << kDrvHammer)) g_lever_draws.fetch_add(1, std::memory_order_relaxed);
                            // any steadied model's parts (the lever guns, the pump, the bolts, the single shots): not only the
                            // lever's drivers (cycles L2 and M1: the Bolt Action swung 102 degrees through John's fire clip)
                            if (done && (gun_flags(gw) & kSteady)) steady_on = true;
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
                        if (steady_on && !rec_matrix) {  // the drawn gun kept in the drawn hand through John's fire clip
                            int sh = -1;
                            for (int h = 0; h < 2; ++h)
                                if (g_rig.att_wrist[h] == best) sh = h;
                            Steady* sd = sh >= 0 && g_steady[sh].wrist_ok ? &g_steady[sh] : nullptr;
                            const float* po = reinterpret_cast<const float*>(mset + 0x10);
                            const float d[3] = {po[0] - g_player_offset[0], po[1] - g_player_offset[1], po[2] - g_player_offset[2]};
                            float root[12], T[12], ti[12];
                            std::memcpy(root, sm, sizeof(root));  // bone 0, the root, as drawn (the player set's space)
                            for (int k = 0; k < 3; ++k) root[k * 4 + 3] += d[k];
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
                                        std::memcpy(md, m, sizeof(md));
                                        for (int k = 0; k < 3; ++k) md[k * 4 + 3] += d[k];
                                        mul34(X, md, m);
                                        for (int k = 0; k < 3; ++k) m[k * 4 + 3] -= d[k];
                                    }
                                    sd->on = true;
                                    g_steady_draws.fetch_add(1, std::memory_order_relaxed);
                                }
                            } else if (clip) {
                                g_steady_misses.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        if (hi >= 0 && !rec_matrix) {  // run 6 item 1, "skel follow": the gun in hand's root as drawn (world)
                            const float* po = reinterpret_cast<const float*>(mset + 0x10);
                            float m[12];
                            std::memcpy(m, sm, sizeof(m));
                            for (int k = 0; k < 3; ++k) m[k * 4 + 3] += po[k];
                            follow_obj(0, xf ? 1 - held : held, m);
                        }
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
                            std::memcpy(ps.post, sm, sizeof(ps.post));
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
    if (rigid && held_prop::diag_on())
        held_prop::diag_draw(reinterpret_cast<uintptr_t>(rec[0x10]), reinterpret_cast<const float*>(rec) + 12, t_draw_pass, reinterpret_cast<const float*>(rec));
    const bool rigid_hp = rigid && held_prop::match(reinterpret_cast<uintptr_t>(rec[0x10]), reinterpret_cast<const float*>(rec) + 12, hp_pose2, &hp_slot2);
    if (rigid_hp) {
        float* rc = reinterpret_cast<float*>(rec);
        std::lock_guard lock(g_draw_mutex);
        if (hp_slot2 == 1) {  // [Hands] OwnModelFollow, and "skel follow"
            const int john = own_follow_pose(hp_pose2);
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
        const int hi = rigid_held ? held_match(rc) : -1;
        int si = hi < 0 && rigid_sec ? sec_match(rc) : -1;  // [Hands] DualWield: the second gun
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
                for (int half = 0; half < 2; ++half) {
                    float* rm = rc + half * 16;
                    for (int ax = 0; ax < 3; ++ax) {
                        float v[3] = {rm[ax * 4], rm[ax * 4 + 1], rm[ax * 4 + 2]};
                        for (int k = 0; k < 3; ++k) rm[ax * 4 + k] = c.A[k * 3] * v[0] + c.A[k * 3 + 1] * v[1] + c.A[k * 3 + 2] * v[2];
                    }
                    float t[3] = {rm[12], rm[13], rm[14]};
                    for (int k = 0; k < 3; ++k) rm[12 + k] = c.A[k * 3] * t[0] + c.A[k * 3 + 1] * t[1] + c.A[k * 3 + 2] * t[2] + c.a[k];
                }
                if (hi >= 0 && copy_maps(*hsm, c, &cj.Tw, &cj.Ts)) {
                    cj.kind = 1;
                    cj.rc = rc;
                    cj.rw = rc + 16;
                }
            }
            g_rigid_props.fetch_add(1, std::memory_order_relaxed);
        }
    }
    WindHold wind;
    if (wind_y >= 0.0f) wind_hold(&wind, wind_y);
    t_cull_show = cull_show;
    o_DrawVisEntity(ctx, rec, pass, bucket, a5, a6, a7);
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
            g_copy_where.kind = cj.kind;
            g_copy_where.pass = t_draw_pass;
            std::memcpy(g_copy_where.pre, pre, sizeof(pre));
            copy_root(cj, g_copy_where.post);
            ++g_copy_where.n;
            if (cj.kind == 3 && g_follow_rec.load(std::memory_order_relaxed)) {  // run 6 item 1, "skel follow": the copy
                const float* po = reinterpret_cast<const float*>(cj.mset + 0x10);
                float m[12];
                std::memcpy(m, cj.sm, sizeof(m));
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
        o_DrawVisEntity(ctx, rec, pass, bucket, a5, a6, a7);
    }
    t_cull_show = 0;
    wind_release(wind);
    if (set_at) std::memcpy(set_at, save.data(), set_n * sizeof(float));
    if (rec_at) std::memcpy(rec_at, rec_save, sizeof(rec_save));
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
    g_cfg_stance = s < 0 || s > 2 ? 1 : s;
    log::info("[body] stance %s", g_cfg_stance == 0 ? "the game's" : g_cfg_stance == 1 ? "upper body upright" : "lean removed");
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

bool gun_in_gun_hand() { return g_gun_hand_cfg.load(); }
void set_gun_in_gun_hand(bool on) {
    if (g_gun_hand_cfg.exchange(on) != on) log::info("[body] left-handed: the gun in John's left hand, each arm on its own controller %d", on ? 1 : 0);
    config::set("Hands", "GunInGunHand", on ? "1" : "0");
}
bool fixed_gun_grip() { return g_fixed_grip.load(std::memory_order_relaxed); }
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
            for (int w = 8; w <= 20; ++w) {
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
        g_fixed_grip = config::get_bool("Hands", "FixedGunGrip", false);
        g_fixed_sidearm = config::get_bool("Hands", "FixedSidearmGrip", false);
        g_follow_auto = config::get_bool("Debug", "FollowLog", true);
        load_gun_rels();
        g_copy_grip_cfg = config::get_bool("Hands", "CopyGrip", true);
        g_same_frame_cfg = config::get_bool("Reload", "TwoHandedSteady", true);
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
        g_cfg_stance = st == "game" ? 0 : st == "lean" ? 2 : 1;
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
    // the user's choice (2026-10-04): crouching, in cover, riding or driving show the forearms and hands (or less, if the
    // global setting already shows less), the body getting in the way of the view there
    f.show = hc.show;
    if (auto_show && (flags & (RDRVR_ACTOR_CROUCHING | RDRVR_ACTOR_IN_COVER | RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)) && f.show < 1)
        f.show = 1;
    f.stretch_max = hc.stretch ? hc.stretch_max : 1.0f;
    {
        RdrvrActorState ws{};
        f.long_gun = api::actor_state(&ws) && reload::is_long_gun(ws.weapon) && reload::two_handed();
        f.weapon = ws.valid ? ws.weapon : -1;
        float fr = 0;
        holster::gun_adjust(f.weapon, holster::kAdjForegrip, f.fore_off, &fr);  // round 13 item 8: the gun's own, else every gun's
        f.snap = holster::foregrip_snap();
        f.two_blend = holster::two_hand_blend();
        f.barrel_blend = actions::barrel_snap();
        f.part_blend = actions::part_snap(f.part_p);
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
    {
        static bool stretch_logged = false;
        if (!stretch_logged && g_stretches.load(std::memory_order_relaxed)) {
            stretch_logged = true;
            log::info("[body] arm stretch: an arm lengthened past John's reach (the first this session)");
        }
    }
    float grip_wp[2][3] = {};  // each hand's controller grip (world), for the two-handed aim below
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
        f.ik[hh] = true;
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
        if (tb > 0.0f && f.ik[0] && f.ik[1] && bt_ok) {
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
    }
    f.pitch = pitch_deg * 0.0174532925f;
    // the facing lock: the angle about the world's up from the actor's forward (the root's -z) to the camera's
    // (heading h faces (-sin h, -cos h))
    float h = pose::torso_heading_deg() * 0.0174532925f;  // run 9: the horse's while the ride's view is turned
    float cfx = -std::sin(h), cfz = -std::cos(h);
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
    if (seq_copy(g_held_seq, f.held, g_held_ring, sizeof(f.held))) {  // the held item's samples
        for (const HeldSample& hs : f.held) f.held_any |= hs.valid;
    } else {
        for (HeldSample& hs : f.held) hs.valid = false;
    }
    for (int i = 0; i < kHeldRing; ++i)
        if (f.held[i].build && (f.held_new < 0 || f.held[i].build > f.held[f.held_new].build)) f.held_new = i;
    if (seq_copy(g_sec_ring_seq, f.sec, g_sec_ring, sizeof(f.sec))) {  // [Hands] DualWield: the second gun's samples
        for (const HeldSample& hs : f.sec) f.sec_any |= hs.valid;
    } else {
        for (HeldSample& hs : f.sec) hs.valid = false;
    }
    for (int i = 0; i < kHeldRing; ++i)
        if (f.sec[i].valid && (f.sec_new < 0 || f.sec[i].build > f.sec[f.sec_new].build)) f.sec_new = i;
    for (const HeldSample& hs : f.sec) f.sec_build = hs.build > f.sec_build ? hs.build : f.sec_build;  // valid or not
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
        f.xfer = f.gun_j == 0 && f.item_side >= 0 && f.item_side != f.gun_j;
        f.fingers = f.xfer && g_rig.mirror_ok ? g_fingers_cfg.load(std::memory_order_relaxed) : 0;
        // round 13: any second gun's hand too (DualWield's long guns and other sidearms), not only the copy's
        f.copy_grip = g_copy_grip_cfg.load(std::memory_order_relaxed) && g_rig.mirror_ok && (dual::copy_W() != 0 || dual::secondary_W(nullptr) != 0) &&
                      f.item_side >= 0;
    }
    {  // the last frame: the item was in hand, and did any of its draws come?
        static bool was_held = false;
        const uint64_t n = g_held_frame_draws.exchange(0, std::memory_order_relaxed);
        if (was_held) {
            g_held_frames.fetch_add(1, std::memory_order_relaxed);
            if (!n) g_held_missed.fetch_add(1, std::memory_order_relaxed);
        }
        was_held = f.held_any;
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
            if (frames != frames0 || ob[1] != o0[1] || cp != cp0 || cr != cr0)
                log::info("[lag] 10 s: held draws by pass 0/1/3 %u/%u/%u, stale %u/%u/%u | the second gun or own model %u/%u/%u, stale %u/%u/%u | "
                          "matched the newest %llu, an older sample %llu | frames in hand %llu, without a draw %llu | the copy: placed %llu, refused "
                          "%llu (why %d), unmatched %llu, draws by pass 0/1/3 %u/%u/%u, stale %u/%u/%u",
                          hb[0] - h0[0], hb[1] - h0[1], hb[3] - h0[3], sb[0] - s0[0], sb[1] - s0[1], sb[3] - s0[3], ob[0] - o0[0], ob[1] - o0[1], ob[3] - o0[3],
                          osb[0] - os0[0], osb[1] - os0[1], osb[3] - os0[3], static_cast<unsigned long long>(newest - newest0),
                          static_cast<unsigned long long>(older - older0), static_cast<unsigned long long>(frames - frames0),
                          static_cast<unsigned long long>(missed - missed0), static_cast<unsigned long long>(cp - cp0),
                          static_cast<unsigned long long>(cr - cr0), g_copy_why.load(), static_cast<unsigned long long>(cn - cn0), cb[0] - c0[0],
                          cb[1] - c0[1], cb[3] - c0[3], csb[0] - cs0[0], csb[1] - cs0[1], csb[3] - cs0[3]);
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
        std::snprintf(b, sizeof(b), "rope at hands %d: lasso draws %llu, points draws %llu, moved %llu; the last: %d points, %d moved | game first (%.3f %.3f %.3f) "
                      "last (%.3f %.3f %.3f) | game wrists L (%.3f %.3f %.3f) R (%.3f %.3f %.3f) | drawn first (%.3f %.3f %.3f) last (%.3f %.3f %.3f)",
                      g_lasso_cfg.load() ? 1 : 0, static_cast<unsigned long long>(g_lasso_draws.load()), static_cast<unsigned long long>(g_lasso_points.load()),
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
        return b;
    }
    if (sub == "stale") {  // skel stale: the held draws and the stale ones (last frame's correction) by pass
        std::string o = "held draws by pass:";
        for (int i = 0; i < 8; ++i) o += " " + std::to_string(g_held_by_pass[i].load());
        o += " | stale by pass:";
        for (int i = 0; i < 8; ++i) o += " " + std::to_string(g_stale_by_pass[i].load());
        o += " | the second gun or own model by pass:";
        for (int i = 0; i < 8; ++i) o += " " + std::to_string(g_other_by_pass[i].load());
        o += " | stale:";
        for (int i = 0; i < 8; ++i) o += " " + std::to_string(g_other_stale_by_pass[i].load());
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
        char b[200];
        std::lock_guard lock(g_draw_mutex);
        std::snprintf(b, sizeof(b), "part snap: weight %.2f, the handle (%.3f %.3f %.3f) in the gun, snapped draws %llu, the drawn wrist %.1f mm from it (worst %.1f)",
                      g_frame.part_blend, g_frame.part_p[0], g_frame.part_p[1], g_frame.part_p[2], static_cast<unsigned long long>(g_part_snaps),
                      g_part_miss * 1000.0f, g_part_miss_max * 1000.0f);
        if (arg == "reset") g_part_snaps = 0, g_part_miss = 0.0f, g_part_miss_max = 0.0f;
        return b;
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
            char b[200];
            std::snprintf(b, sizeof(b), "lever draws %llu, steady draws %llu, misses %llu, clips stopped %llu (fire clip %.2f; rest %d %d, steady %d %d)",
                          static_cast<unsigned long long>(g_lever_draws.load()), static_cast<unsigned long long>(g_steady_draws.load()),
                          static_cast<unsigned long long>(g_steady_misses.load()), static_cast<unsigned long long>(g_clip_stops.load()),
                          holster::fire_clip_phase(), g_steady[0].rest_ok ? 1 : 0,
                          g_steady[1].rest_ok ? 1 : 0, g_steady[0].on ? 1 : 0, g_steady[1].on ? 1 : 0);
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
        std::snprintf(b, sizeof(b), "open draws %llu (amount %.2f, weapon %d; the held set: %d matrices, record matrix %d; rigid held draws %llu) | bone rows (%.3f %.3f %.3f | %.3f) (%.3f %.3f %.3f | %.3f) (%.3f %.3f %.3f | %.3f)",
                      static_cast<unsigned long long>(g_open_draws.load()), amt, ow, g_held_set_count.load(), g_held_set_rm.load(),
                      static_cast<unsigned long long>(g_rigid_props.load()), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
        return b;
    }
    if (sub == "hprop") {  // skel hprop: the held props' draws (held_prop): the paths and the last one's positions
        char b[400];
        std::lock_guard lock(g_draw_mutex);
        const HpDiag& d = g_hp_diag;
        std::snprintf(b, sizeof(b), "hprop draws %llu (skinned %llu, rigid %llu) | last: path %d count %d flags %#x before (%.3f %.3f %.3f) target (%.3f %.3f %.3f) after (%.3f %.3f %.3f) x before (%.3f %.3f %.3f) after (%.3f %.3f %.3f)",
                      static_cast<unsigned long long>(g_hprop_draws.load()), static_cast<unsigned long long>(g_hprop_skinned.load()),
                      static_cast<unsigned long long>(g_hprop_rigid.load()), d.path, d.count, d.flags, d.before[0], d.before[1], d.before[2],
                      d.target[0], d.target[1], d.target[2], d.after[0], d.after[1], d.after[2], d.xb[0], d.xb[1], d.xb[2], d.xa[0], d.xa[1], d.xa[2]);
        return b;
    }
    if (sub == "grip") {  // skel grip [on|off|reset]: [Hands] FixedGunGrip (the session); the long gun in the animated wrist's frame
        if (line.find(" grip on") != std::string::npos) g_fixed_grip = true;
        if (line.find(" grip off") != std::string::npos) g_fixed_grip = false;
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
                      "off it %.1f mm %.2f deg | learns %llu fixed %llu frames %llu saved %llu | why %d | wrist at (%.2f %.2f %.2f) gun (%.2f %.2f %.2f) "
                      "attachment (%.2f %.2f %.2f)",
                      g_fixed_grip.load() ? "on" : "off", d.weapon, d.side, d.aiming, d.src, d.now[12], d.now[13], d.now[14], d.now[0], d.now[1],
                      d.now[2], g.valid ? 1 : 0, g.m[12], g.m[13], g.m[14], mm, deg, d.aim_max_mm, d.aim_max_deg, d.low_max_mm, d.low_max_deg,
                      d.placed[12], d.placed[13], d.placed[14], d.placed_max_mm, d.placed_max_deg, static_cast<unsigned long long>(d.learns),
                      static_cast<unsigned long long>(d.fixed), static_cast<unsigned long long>(d.frames), static_cast<unsigned long long>(d.saved), d.why,
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
        char b[480];
        std::snprintf(b, sizeof(b),
                      "second gun: W %p (John's hand %d), placements %llu, refused %llu (last why %d), draws %llu (loose %llu), near misses %llu (%.3f m, row %.3f), "
                      "at (%.2f %.2f %.2f), W flags 0x%x, prop %s flags 0x%x | the rule on the gun in hand (%s): its input %.4f from the attachment bone, "
                      "W +0x80 %.4f from grip x input; grips right %d left %d",
                      reinterpret_cast<void*>(sw), sj, static_cast<unsigned long long>(g_sec_placements.load()),
                      static_cast<unsigned long long>(g_sec_refused.load()), g_sec_why.load(), static_cast<unsigned long long>(g_sec_draws.load()),
                      static_cast<unsigned long long>(g_sec_loose.load()), static_cast<unsigned long long>(g_sec_near.load()), g_sec_near_d, g_sec_near_x, sp[0], sp[1], sp[2], wfl, pobj ? "found" : "none", pfl,
                      side == 0 ? "left" : side == 1 ? "right" : "none", in_err, rule_err, rrig, lrig);
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
        std::string r = "props:";
        char b[300];
        for (int i = 0; i < g_nprop_diag; ++i) {
            const PropDiag& d = g_prop_diag[i];
            std::snprintf(b, sizeof(b), " | bone %d count %u flags %#x set t (%.2f %.2f %.2f) x (%.2f %.2f %.2f) rec t (%.2f %.2f %.2f) x (%.2f %.2f %.2f)",
                          d.bone, d.count, d.flags, d.set_t[0], d.set_t[1], d.set_t[2], d.set_x[0], d.set_x[1], d.set_x[2], d.rec_t[0],
                          d.rec_t[1], d.rec_t[2], d.rec_x[0], d.rec_x[1], d.rec_x[2]);
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
        if (v == "stance") {
            std::string s;
            in >> s;
            set_stance(s == "game" ? 0 : s == "lean" ? 2 : 1);
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
        char out[400];
        std::snprintf(out, sizeof(out),
                      "body %s (override %d), posture flags %u (auto show %s), head hide %s, stance %d, back %.2f m, follows head %s, "
                      "upright bone %d, skeleton %p, sets %llu, hat hides %llu, props moved %llu, rigid props %llu",
                      g_active.load() ? "on" : "off", g_override.load(), g_actor_flags.load(), auto_shows() ? "on" : "off",
                      hide_enabled() ? "on" : "off", stance(), body_back(),
                      follows_head() ? "on" : "off", g_rig.up, reinterpret_cast<void*>(player_skeleton()),
                      static_cast<unsigned long long>(g_sets.load()), static_cast<unsigned long long>(g_hat_hides.load()),
                      static_cast<unsigned long long>(g_props.load()), static_cast<unsigned long long>(g_rigid_props.load()));
        return out;
    }
    return "ERROR usage: skel q|f|s <addr> [n] | skel bones <file> | skel ik | skel stretch | skel body [on|off|auto|stance upright|lean|game|back <m>|follow on|off|hidehead on|off|stretch on|off|<max>]";
}

}  // namespace rdrvr::body
