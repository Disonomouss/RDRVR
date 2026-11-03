#include "core/round_draw.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include "common/rdrvr_api.h"
#include "core/aim.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/held_prop.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/state.h"
#include "core/pso.h"

namespace rdrvr::round_draw {
namespace {

// ---- the old models ([Reload] RoundModels = 0): lathe profiles (radius, height along the axis, mm; the material),
// base at 0, tip up
enum Mat { kBrass = 0, kLead = 1, kHull = 2 };
struct Pt {
    float r, y;
    int mat;  // the band from this point to the next
};
// .44-40 (the Cattleman's and the Schofield's era): rim 13.3 mm, case 33 mm, a round-nosed lead bullet, 40 mm in all
const Pt kHandgun[] = {{0, 0, kBrass},     {6.67f, 0, kBrass},    {6.67f, 1.65f, kBrass}, {5.98f, 1.65f, kBrass},
                       {5.80f, 26, kBrass}, {5.63f, 28, kBrass},   {5.63f, 33.1f, kBrass}, {5.42f, 33.1f, kLead},
                       {5.42f, 35, kLead},  {5.10f, 37, kLead},    {4.30f, 38.8f, kLead},  {3.00f, 39.8f, kLead},
                       {1.50f, 40.3f, kLead}, {0, 40.4f, kLead}};
// .30-30 (the repeaters): rim 12.9 mm, a bottlenecked case 52 mm, a pointed bullet, 65 mm in all
const Pt kRifle[] = {{0, 0, kBrass},        {6.43f, 0, kBrass},     {6.43f, 1.6f, kBrass},  {5.36f, 1.6f, kBrass},
                     {5.10f, 40.6f, kBrass}, {4.19f, 44, kBrass},    {4.19f, 51.8f, kBrass}, {3.91f, 51.8f, kLead},
                     {3.91f, 55, kLead},     {3.40f, 59, kLead},     {2.40f, 62.3f, kLead},  {1.10f, 64.2f, kLead},
                     {0, 64.8f, kLead}};
// a 12 gauge shell: rim 22.2 mm, a brass head 13 mm, a red paper hull to 63.5 mm with a rolled crimp
const Pt kShell[] = {{0, 0, kBrass},       {11.1f, 0, kBrass},    {11.1f, 1.5f, kBrass}, {10.3f, 1.5f, kBrass},
                     {10.3f, 13, kHull},    {10.15f, 13, kHull},   {10.15f, 61.5f, kHull}, {9.6f, 63, kHull},
                     {8.6f, 63.5f, kHull},  {0, 63.5f, kHull}};
constexpr int kSeg = 24;  // around the axis

// One vertex layout for both: the old models leave uv 0 (their shader reads no texture) and carry the material in w
struct Vtx {
    float p[4];  // x y z (m), w the old models' material
    float n[3];
    float uv[2];  // the new models': the atlas
};
struct Model {
    uint32_t first = 0, count = 0;  // vertices (the old, drawn as a list) or indices (the new)
    int32_t base = 0;               // the new: the first vertex (the indices are the model's own)
    bool indexed = false;
    float len = 0.0f, rad = 0.0f;   // m: base to tip, the widest radius
};
Model g_old[3];  // handgun, rifle, shotgun
std::vector<Vtx> g_mesh;
std::vector<uint16_t> g_idx;

void lathe(const Pt* pts, int n, Model* out) {
    out->first = static_cast<uint32_t>(g_mesh.size());
    for (int i = 0; i + 1 < n; ++i) {
        const Pt &a = pts[i], &b = pts[i + 1];
        const float dr = b.r - a.r, dy = b.y - a.y, l = std::sqrt(dr * dr + dy * dy);
        if (l < 1e-6f) continue;
        const float nr = dy / l, ny = -dr / l;  // the band's outward normal in (radial, axial)
        for (int s = 0; s < kSeg; ++s) {
            const float t0 = 6.2831853f * s / kSeg, t1 = 6.2831853f * (s + 1) / kSeg;
            const float c0 = std::cos(t0), s0 = std::sin(t0), c1 = std::cos(t1), s1 = std::sin(t1);
            auto v = [&](float r, float y, float c, float sn) {
                Vtx x{{r * c * 0.001f, y * 0.001f, r * sn * 0.001f, static_cast<float>(a.mat)}, {nr * c, ny, nr * sn}, {0.0f, 0.0f}};
                g_mesh.push_back(x);
            };
            v(a.r, a.y, c0, s0);
            v(b.r, b.y, c0, s0);
            v(b.r, b.y, c1, s1);
            v(a.r, a.y, c0, s0);
            v(b.r, b.y, c1, s1);
            v(a.r, a.y, c1, s1);
        }
        out->len = std::fmax(out->len, b.y * 0.001f);
        out->rad = std::fmax(out->rad, std::fmax(a.r, b.r) * 0.001f);
    }
    out->count = static_cast<uint32_t>(g_mesh.size()) - out->first;
}

// ---- the new models (2026-10-27, research/run27/rounds.md section 4; the user: "Create new models for the bullets. Make
// them match the game style and resolution."): each gun's own round at its real (nominal) dimensions, in mm, base at 0.
// 7-10 bands a profile and 12 sides (the shell 10, as the game's own): 140-216 triangles a round, about the game's 2-3
// a cm2 (the old lathe had 432-624). The poles take one triangle a side. Normals smooth across the case and the ogive,
// split at the hard edges (the rim's faces, the extractor groove, the base, the shell's top): the old per-band banding
// gone. Every point a band's end; the materials are the atlas's (the height where the case ends)
struct KPt {
    float r, y;
};
enum KMat { kmBrass, kmCopper, kmLead, kmJacket, kmHull, kmPaper };
enum KHead { khCentre, khRimfire, khVolcanic, khShell, khPaper };
struct KindDef {
    const char* name;     // the test commands' ("round show 44-40 ...")
    const KPt* pts;
    int n;
    int sides;
    float case_top;       // mm: below it the case (lower), above it the bullet or the hull (upper); 0 no case
    int lower, upper;     // KMat
    int head;             // KHead: what the base shows
    float primer;         // mm: the primer's radius (the Volcanic's disc, the shell's cup)
    float crimp;          // mm: the case's crimp ring (0 none)
    float rim_h;          // mm: the rim's thickness (its edge worn light)
    bool rimless;         // an extractor groove (grime in it)
};
// .44-40 WCF (Winchester 1873, 1873: the Colt's too): rim 13.3, a slight bottleneck 11.96 -> 11.25, case 33.0, a
// 10.85 round-nosed lead bullet, 40.5 in all
const KPt kP4440[] = {{0, 0}, {6.65f, 0}, {6.65f, 1.65f}, {5.98f, 1.65f}, {5.82f, 25.0f}, {5.63f, 27.5f}, {5.55f, 33.3f},
                      {4.9f, 36.6f}, {3.3f, 39.4f}, {0, 40.5f}};
// .44 Henry rimfire (1860): rim 13.3, a straight copper case 22.2, a 11.3 flat-pointed lead bullet, 34.2 in all
const KPt kPHenry[] = {{0, 0}, {6.65f, 0}, {6.65f, 1.4f}, {5.75f, 1.4f}, {5.75f, 22.2f}, {5.62f, 24.5f}, {4.7f, 30.8f},
                       {2.8f, 33.6f}, {0, 34.2f}};
// .44 Evans Long (the Evans' own, 1877): rim 12.7, a straight case 27.9, a 10.6 lead bullet, 39.0 in all
const KPt kPEvans[] = {{0, 0}, {6.35f, 0}, {6.35f, 1.4f}, {5.6f, 1.4f}, {5.55f, 27.9f}, {5.32f, 28.6f}, {4.6f, 33.8f},
                       {2.7f, 37.8f}, {0, 39.0f}};
// .38 ACP (the Colt 1900 class): a semi-rim 10.2 with a groove, case 22.9, a 9.0 copper-jacketed round nose, 32.5
const KPt kP38[] = {{0, 0}, {5.1f, 0}, {5.1f, 1.0f}, {4.35f, 1.6f}, {4.8f, 2.3f}, {4.75f, 22.9f}, {4.5f, 23.6f},
                    {3.7f, 27.9f}, {2.0f, 31.3f}, {0, 32.5f}};
// .45 ACP class: rimless 12.0 with a groove, case 22.8, an 11.5 jacketed round nose, 32.4 in all
const KPt kP45[] = {{0, 0}, {6.0f, 0}, {6.0f, 1.2f}, {5.1f, 1.8f}, {6.0f, 2.5f}, {5.97f, 22.8f}, {5.73f, 23.4f},
                    {4.9f, 27.4f}, {2.9f, 31.0f}, {0, 32.4f}};
// 7.63 x 25 Mauser (the C96): rimless 9.9, a bottleneck to 8.4 at 20, case 25.1, a 7.85 jacketed bullet, 34.9
const KPt kPMauser[] = {{0, 0}, {4.95f, 0}, {4.95f, 1.1f}, {4.2f, 1.6f}, {4.95f, 2.3f}, {4.85f, 18.6f}, {4.2f, 20.4f},
                        {4.05f, 26.6f}, {3.1f, 31.0f}, {1.5f, 34.1f}, {0, 34.9f}};
// .41 Volcanic "Rocket Ball": no case; a 10.4 lead bullet, 15 long, its hollow base the powder and a primer disc
const KPt kPVolcanic[] = {{0, 0}, {4.9f, 0}, {5.2f, 0.5f}, {5.2f, 7.5f}, {4.7f, 10.6f}, {3.4f, 13.1f}, {1.6f, 14.6f},
                          {0, 15.0f}};
// 7 x 57 Mauser class (the Bolt Action; the 6.5 Carcano's 52.5 / 77 near it): rimless 12.0, a shoulder at 47, neck
// 8.25, case 57.0, a 7.24 jacketed spitzer, 78.0 in all
const KPt kPRifle[] = {{0, 0}, {6.0f, 0}, {6.0f, 1.2f}, {5.1f, 1.9f}, {6.0f, 2.8f}, {5.45f, 47.0f}, {4.12f, 50.5f},
                       {4.12f, 57.0f}, {3.3f, 63.5f}, {1.8f, 72.0f}, {0, 78.0f}};
// .45-70 Government (the trapdoor Springfield's; the Rolling Block's and the Sharps' class): rim 15.2, a straight case
// 53.5, an 11.6 heavy lead round nose, 64.8 in all
const KPt kPBig[] = {{0, 0}, {7.6f, 0}, {7.6f, 1.75f}, {6.4f, 1.75f}, {6.2f, 53.5f}, {5.85f, 54.3f}, {5.6f, 58.5f},
                     {4.2f, 62.4f}, {2.0f, 64.4f}, {0, 64.8f}};
// the shotgun shell at the game's own p_gen_shellshotgun01x size (research/run27/rounds.md section 3: 19.6 rim, a hull
// about 18, the brass head 8.3 above the rim, 47.4 in all, flat-topped), 10 sides as the game's: the stand-in for the
// game's own shell, alike beside it
const KPt kPShell[] = {{0, 0}, {9.8f, 0}, {9.8f, 1.0f}, {9.1f, 1.4f}, {9.1f, 9.3f}, {9.0f, 9.6f}, {9.0f, 46.6f},
                       {8.4f, 47.4f}, {0, 47.4f}};
// the Blunderbuss (a muzzle loader): a paper cartridge, the ball's bulge at the base, the tail twisted shut
const KPt kPPaper[] = {{0, 0}, {6.5f, 0.8f}, {9.0f, 4.0f}, {9.0f, 30.0f}, {7.8f, 34.8f}, {3.0f, 39.5f}, {2.6f, 43.0f},
                       {0, 45.0f}};
#define RDRVR_N(a) static_cast<int>(sizeof(a) / sizeof((a)[0]))
const KindDef kDefs[kKinds] = {
    {"44-40", kP4440, RDRVR_N(kP4440), 12, 33.0f, kmBrass, kmLead, khCentre, 2.67f, 32.4f, 1.65f, false},
    {"henry", kPHenry, RDRVR_N(kPHenry), 12, 22.2f, kmCopper, kmLead, khRimfire, 0.0f, 21.6f, 1.4f, false},
    {"evans", kPEvans, RDRVR_N(kPEvans), 12, 27.9f, kmBrass, kmLead, khCentre, 2.5f, 27.3f, 1.4f, false},
    {"38acp", kP38, RDRVR_N(kP38), 12, 22.9f, kmBrass, kmJacket, khCentre, 2.22f, 0.0f, 1.0f, true},
    {"45acp", kP45, RDRVR_N(kP45), 12, 22.8f, kmBrass, kmJacket, khCentre, 2.67f, 0.0f, 1.2f, true},
    {"mauser", kPMauser, RDRVR_N(kPMauser), 12, 25.1f, kmBrass, kmJacket, khCentre, 2.22f, 0.0f, 1.1f, true},
    {"volcanic", kPVolcanic, RDRVR_N(kPVolcanic), 12, 0.0f, kmLead, kmLead, khVolcanic, 1.9f, 0.0f, 0.0f, false},
    {"rifle", kPRifle, RDRVR_N(kPRifle), 12, 57.0f, kmBrass, kmJacket, khCentre, 2.67f, 0.0f, 1.2f, true},
    {"45-70", kPBig, RDRVR_N(kPBig), 12, 53.5f, kmBrass, kmLead, khCentre, 2.67f, 52.9f, 1.75f, false},
    {"shell", kPShell, RDRVR_N(kPShell), 10, 9.3f, kmBrass, kmHull, khShell, 3.1f, 0.0f, 1.0f, false},
    {"paper", kPPaper, RDRVR_N(kPPaper), 12, 0.0f, kmPaper, kmPaper, khPaper, 0.0f, 0.0f, 0.0f, false},
};
#undef RDRVR_N
Model g_kind[kKinds];

// ---- the atlas (2026-10-27, rounds.md "Making the textures our own"): generated here at boot (a fixed seed), nothing
// of the game's read, traced or shipped. Each kind a side strip (round the axis by up the profile's length) and two
// 32 x 32 discs (the base, the top), at about 16 texels a cm (the guns' painted cartridge heads, 14-19), in one
// RGBA8 sRGB texture (alpha: the gloss), packed in shelves with a 4-texel gutter (round the axis: wrapped; up it:
// clamped), three mips (the sampler stops at the third: the gutters cover it)
constexpr int kAtlasW = 512, kGut = 4, kCapT = 32, kMips = 3;
constexpr float kTexPerMm = 1.6f;
int g_atlas_h = 0;
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;  // the content (texels); the gutter is outside it
};
struct SideBand {
    float ar, ay, br, by;  // the band's ends (mm)
    float s0, s1;          // its arc along the side (mm)
};
struct KindTex {
    Rect side, base, top;
    float arc = 0.0f;        // the side's whole arc (mm)
    float base_r = 1.0f, top_r = 0.0f;  // the discs' radii (mm; top 0: none)
    std::vector<SideBand> bands;
};
KindTex g_kt[kKinds];
// 2026-10-28 ([Reload] PistolMagazines; research/run28/magazines.md "The game's own magazines"): each magazine's (and the
// clip's) own steel rect, painted from its gun's own diffuse as decoded (the area-weighted 10/25/50/75/90% texels of
// the gun's low-saturation steel: nothing of the game's read at run time or shipped, only these numbers), mapped at its
// gun's texel density
constexpr int kMagT = 144;
Rect g_mrect[3];  // the High Power's, the Semi-Auto's, the clip's (the Mauser's steel)
struct MagMat {
    uint8_t q[5][3];  // sRGB at the 10, 25, 50, 75 and 90% points
    float tpm;        // texels a mm on the model (the gun's own: research)
    float gloss;
};
const MagMat kMagMat[3] = {
    {{{32, 32, 32}, {46, 53, 46}, {65, 70, 62}, {81, 78, 73}, {90, 97, 90}}, 1.45f, 0.55f},  // pistol_highpower01: 14.3 texels/cm
    {{{16, 16, 16}, {26, 26, 26}, {41, 40, 41}, {51, 53, 51}, {71, 68, 63}}, 0.78f, 0.50f},  // pistol_semiauto01: 7.8
    {{{16, 12, 16}, {24, 24, 24}, {41, 36, 32}, {49, 52, 57}, {82, 68, 57}}, 1.40f, 0.55f},  // pistol_mauser01: 13.8-14.2
};
std::vector<uint8_t> g_atlas[kMips];  // RGBA8 (sRGB), mip m: (kAtlasW >> m) x (g_atlas_h >> m)

// the side's bands (the base's and the shell's top disc excluded: the discs map them) and the discs' radii
void kind_bands(int k) {
    const KindDef& d = kDefs[k];
    KindTex& t = g_kt[k];
    t.bands.clear();
    t.arc = 0.0f;
    for (int i = 0; i + 1 < d.n; ++i) {
        const KPt &a = d.pts[i], &b = d.pts[i + 1];
        const float dr = b.r - a.r, dy = b.y - a.y, l = std::sqrt(dr * dr + dy * dy);
        if (l < 1e-6f) continue;
        if (i == 0 && a.r < 1e-4f) {  // the base disc
            t.base_r = b.r;
            continue;
        }
        if (i + 2 == d.n && b.r < 1e-4f && std::fabs(dy) < 1e-4f) {  // a flat top disc (the shell)
            t.top_r = a.r;
            continue;
        }
        t.bands.push_back({a.r, a.y, b.r, b.y, t.arc, t.arc + l});
        t.arc += l;
    }
}

// the side's point at v (0 the base's edge, 1 the tip): radius and height (mm)
void side_at(int k, float v, float* r, float* y) {
    const KindTex& t = g_kt[k];
    const float s = std::clamp(v, 0.0f, 1.0f) * t.arc;
    for (const SideBand& b : t.bands) {
        if (s <= b.s1 || &b == &t.bands.back()) {
            const float f = b.s1 > b.s0 ? std::clamp((s - b.s0) / (b.s1 - b.s0), 0.0f, 1.0f) : 0.0f;
            *r = b.ar + (b.br - b.ar) * f;
            *y = b.ay + (b.by - b.ay) * f;
            return;
        }
    }
    *r = 0.0f, *y = 0.0f;
}

// value noise (a hash of the lattice; fixed: the same atlas every boot) and its fbm, 0..1
uint32_t hash3(int x, int y, int z) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u ^ static_cast<uint32_t>(z) * 0xcb1ab31fu ^ 0x2026u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
float vnoise(float x, float y, float z) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    float tx = x - fx, ty = y - fy, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty), tz = tz * tz * (3 - 2 * tz);
    auto h = [&](int a, int b, int c) { return static_cast<float>(hash3(ix + a, iy + b, iz + c) & 0xffff) / 65535.0f; };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(h(0, 0, 0), h(1, 0, 0), tx), x10 = lerp(h(0, 1, 0), h(1, 1, 0), tx);
    const float x01 = lerp(h(0, 0, 1), h(1, 0, 1), tx), x11 = lerp(h(0, 1, 1), h(1, 1, 1), tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}
float fbm(float x, float y, float z, int oct) {
    float s = 0.0f, a = 0.5f, w = 0.0f;
    for (int i = 0; i < oct; ++i) {
        s += a * vnoise(x, y, z);
        w += a;
        x = x * 2.03f + 17.1f, y = y * 2.03f + 5.3f, z = z * 2.03f + 11.7f;
        a *= 0.5f;
    }
    return s / w;
}
float smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
struct Col {
    float r, g, b;  // sRGB, 0..1
};
Col mix(Col a, Col b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Col mul(Col a, float k) { return {a.r * k, a.g * k, a.b * k}; }
Col rgb8(int r, int g, int b) { return {r / 255.0f, g / 255.0f, b / 255.0f}; }

// The materials at a surface point p (mm, the round's own axes: y up it). The albedos are the game's colours as its
// own props show them lit (rounds.md 3 and 4, measured from game-lit captures): the brass casing (186-207, 136-155,
// 94-110) a soft low-saturation brass, the lead a dull dark grey (the Cattleman's painted noses, 60-80), a copper
// jacket about (150, 95, 70), the shell's hull the game's dark grey-brown paper (76, 65, 59) with its lighter band
// (113, 101, 83) - not red (2026-10-27: the game's shell has no red texel; the old lathe's red stood out). g: the gloss
// (the alpha): the specular is one broad soft lobe, as the game's props' flat specular maps give
// (cycle r169, 2026-10-27: the brass read salmon beside the game's shell, whose brass head showed (88, 75, 55) in the
// same daylight capture against ours (165, 121, 83): its base made duller and less saturated, about a third less, the
// grime stronger; the copper and the jacket likewise)
Col brass_at(const KindDef& d, int mat, float px, float py, float pz, float r, float* g) {
    const bool copper = mat == kmCopper;
    Col c = copper ? rgb8(150, 106, 80) : rgb8(162, 136, 100);
    const Col tarn = copper ? rgb8(100, 74, 58) : rgb8(112, 98, 72);
    const float t = smooth(0.48f, 0.74f, fbm(px * 0.22f, py * 0.22f, pz * 0.22f, 4));  // tarnish: olive-brown patches
    c = mix(c, tarn, t * 0.45f);  // (offline review 2026-10-27: 0.62 read as green camouflage)
    c = mul(c, 0.955f + 0.09f * vnoise(px * 2.2f, py * 0.07f, pz * 2.2f));  // the drawing's fine lines along the axis
    c = mul(c, 0.80f + 0.20f * fbm(px * 0.05f + 3.0f, py * 0.05f, pz * 0.05f, 2));  // grime, broad
    c = mul(c, 0.92f + 0.08f * vnoise(px * 0.9f + 7.0f, py * 0.9f, pz * 0.9f));  // and in small smudges
    const Col wear = copper ? rgb8(176, 132, 104) : rgb8(190, 164, 124);
    if (py < d.rim_h + 0.15f) c = mix(c, wear, 0.40f);  // the rim's edge, worn bright
    if (d.case_top > 0.0f && std::fabs(py - d.case_top) < 0.7f) c = mix(c, wear, 0.32f);  // the case mouth
    if (d.crimp > 0.0f && std::fabs(py - d.crimp) < 0.35f) c = mul(c, 0.62f);  // the crimp ring
    if (d.rimless && py > d.rim_h - 0.05f && py < 3.0f && r < d.pts[1].r - 0.3f) c = mul(c, 0.55f);  // the extractor groove's grime
    if (d.head == khShell && (std::fabs(py - 6.4f) < 0.3f || std::fabs(py - 7.6f) < 0.3f)) c = mul(c, 0.70f);  // the head's ribs
    *g = (copper ? 0.75f : 0.85f) - 0.40f * t;
    return c;
}
Col lead_at(const KindDef& d, float px, float py, float pz, float top, float* g) {
    Col c = rgb8(76, 74, 74);
    const float ox = smooth(0.55f, 0.78f, fbm(px * 0.35f, py * 0.35f, pz * 0.35f, 4));  // white-grey oxidation
    c = mix(c, rgb8(120, 119, 114), ox * 0.55f);
    if (vnoise(px * 3.1f, py * 3.1f, pz * 3.1f) > 0.86f) c = mul(c, 1.12f);  // specks
    if (d.head == khVolcanic)  // the Rocket Ball's grease grooves
        for (float gy : {1.6f, 3.4f, 5.2f})
            if (std::fabs(py - gy) < 0.28f) c = mul(c, 0.55f);
    if (d.case_top > 0.0f && py > d.case_top && py < d.case_top + 0.6f) c = mul(c, 0.70f);  // the lube line at the mouth
    if (py > top - 0.7f) c = mul(c, 1.10f);  // the nose's flat, rubbed
    *g = 0.30f - 0.14f * ox;
    return c;
}
Col jacket_at(const KindDef& d, float px, float py, float pz, float top, float* g) {
    Col c = rgb8(138, 98, 78);
    const float f = d.case_top < top ? std::clamp((py - d.case_top) / (top - d.case_top), 0.0f, 1.0f) : 0.0f;
    c = mul(c, 0.95f + 0.12f * f);  // toward the nose
    const float a = std::atan2(pz, px);
    if (std::fabs(a - 1.2f) < 0.035f) c = mul(c, 0.92f);  // a faint seam
    c = mul(c, 0.94f + 0.06f * fbm(px * 0.3f, py * 0.3f, pz * 0.3f, 3));
    *g = 0.70f;
    return c;
}
Col hull_at(const KindDef& d, float px, float py, float pz, float top, float* g) {
    Col c = rgb8(80, 66, 58);
    const float h0 = d.case_top, h = top - h0, f = h > 0.0f ? (py - h0) / h : 0.0f;
    const float band = smooth(0.0f, 0.04f, f - 0.49f) * (1.0f - smooth(0.0f, 0.04f, f - 0.75f));  // the lighter band
    c = mix(c, rgb8(113, 101, 83), band * 0.6f);
    c = mul(c, 0.95f + 0.10f * vnoise(px * 2.5f, py * 2.5f, pz * 2.5f));  // paper fibres
    c = mul(c, 0.96f + 0.08f * fbm(px * 0.4f, py * 0.4f, pz * 0.4f, 3));
    const float scuff = smooth(0.66f, 0.80f, fbm(px * 0.3f + 9.0f, py * 0.3f, pz * 0.3f, 3));
    c = mix(c, rgb8(112, 98, 86), scuff * 0.35f);  // worn scuffs
    if (py > top - 1.0f) c = mul(c, 0.85f);  // the crimp's fold
    if (py < h0 + 0.5f) c = mul(c, 0.72f);  // into the brass head
    *g = 0.12f;
    return c;
}
Col paper_at(float px, float py, float pz, float* g) {
    Col c = rgb8(132, 108, 80);
    c = mul(c, 0.93f + 0.12f * vnoise(px * 2.0f, py * 2.0f, pz * 2.0f));
    c = mul(c, 0.92f + 0.10f * fbm(px * 0.3f, py * 0.3f, pz * 0.3f, 3));
    if (py > 33.0f) {  // the twisted tail's creases
        const float a = std::atan2(pz, px), s = std::fabs(std::sin(a * 3.0f + py * 0.9f));
        c = mul(c, 1.0f - 0.32f * std::pow(1.0f - s, 10.0f));
    }
    if (py > 38.6f && py < 40.2f) c = mix(c, rgb8(92, 84, 70), 0.8f);  // the twine round it
    if (py < 5.0f) c = mul(c, 0.86f + 0.028f * py);  // the ball's bulge, handled
    *g = 0.05f;
    return c;
}
uint32_t pack(Col c, float g) {
    auto b = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(g) << 24;
}
float profile_top(const KindDef& d) { return d.pts[d.n - 1].y; }

uint32_t side_texel(int k, float u, float v) {
    const KindDef& d = kDefs[k];
    float r = 0.0f, y = 0.0f;
    side_at(k, v, &r, &y);
    const float th = 6.2831853f * u, px = r * std::cos(th), pz = r * std::sin(th), top = profile_top(d);
    const int mat = d.case_top > 0.0f && y < d.case_top - 0.02f ? d.lower : d.upper;
    float g = 0.5f;
    Col c;
    switch (mat) {
        case kmBrass:
        case kmCopper: c = brass_at(d, mat, px, y, pz, r, &g); break;
        case kmLead: c = lead_at(d, px, y, pz, top, &g); break;
        case kmJacket: c = jacket_at(d, px, y, pz, top, &g); break;
        case kmHull: c = hull_at(d, px, y, pz, top, &g); break;
        default: c = paper_at(px, y, pz, &g); break;
    }
    return pack(c, g);
}

// a disc: the base (top false) or the shell's crimped top, at (x, z) mm from the axis
uint32_t cap_texel(int k, bool top, float x, float z) {
    const KindDef& d = kDefs[k];
    const KindTex& t = g_kt[k];
    const float R = top ? t.top_r : t.base_r;
    float rho = std::sqrt(x * x + z * z);
    if (rho > R) x *= R / rho, z *= R / rho, rho = R;
    const float th = std::atan2(z, x), ht = top ? profile_top(d) : 0.0f;
    float g = 0.5f;
    Col c;
    if (top) {  // the shell's top: the hull's paper, a six-fold star crimp, a dimple at its centre, a worn bevel
        c = hull_at(d, x, 20.0f, z, ht, &g);  // (a height clear of the band, the fold and the head)
        float dl = 1e9f;
        for (int i = 0; i < 6; ++i) dl = std::fmin(dl, std::fabs(rho * std::sin(th - 1.0471976f * i - 0.5235988f)));
        if (rho > 1.0f && rho < 0.86f * R && dl < 0.35f) c = mul(c, 0.55f);
        c = mul(c, 0.96f + 0.05f * std::cos(6.0f * th + 3.1415927f));
        if (rho < 0.9f) c = mul(c, 0.50f);
        if (rho > 0.86f * R) c = mix(c, rgb8(112, 100, 84), 0.5f);
        return pack(c, g);
    }
    switch (d.head) {
        case khPaper: {  // the paper gathered over the ball: radial folds
            c = paper_at(x, 1.0f, z, &g);
            const float s = std::fabs(std::sin(th * 4.5f + 2.0f * vnoise(x * 0.8f, 3.0f, z * 0.8f)));
            if (s < 0.08f && rho > 1.0f) c = mul(c, 0.70f);
            if (rho < 1.2f) c = mul(c, 0.75f);
            return pack(c, g);
        }
        case khVolcanic: {  // lead, with the primer's brass disc in the hollow base
            c = lead_at(d, x, 8.0f, z, 100.0f, &g);
            if (rho < d.primer) c = brass_at(d, kmBrass, x, 20.0f, z, rho, &g);
            else if (rho < d.primer + 0.25f) c = mul(c, 0.45f);
            if (rho > R - 0.4f) c = mul(c, 1.08f);
            return pack(c, g);
        }
        default: break;
    }
    // a case's head: its brass (or the Henry's copper), the rim's edge worn, the primer and its pocket ring, a dimple
    // ring, a headstamp ring of short marks (no maker's name, no letters)
    c = brass_at(d, d.lower, x, 20.0f, z, rho, &g);  // (a height clear of the rim's and the mouth's wear)
    if (rho > R - 0.7f) c = mix(c, d.lower == kmCopper ? rgb8(176, 132, 104) : rgb8(190, 164, 124), 0.35f);
    float pr = d.primer;
    if (d.head == khShell) {  // the battery cup round the primer
        if (rho < pr + 0.7f && rho >= pr) c = mix(c, rgb8(190, 164, 124), 0.25f);
        if (std::fabs(rho - (pr + 0.7f)) < 0.2f) c = mul(c, 0.6f);
    }
    if (d.head != khRimfire && pr > 0.0f) {
        if (rho < pr) {
            c = mul(rgb8(142, 102, 78), 0.94f + 0.08f * vnoise(x * 1.5f, 7.0f, z * 1.5f));
            g = 0.70f;
            if (std::fabs(rho - 0.55f * pr) < 0.18f) c = mul(c, 0.85f);
        } else if (rho < pr + 0.3f) {
            c = mul(c, 0.45f);
        }
    } else {
        pr = 1.2f;
    }
    const float s0 = pr + (d.head == khShell ? 2.0f : 1.1f), s1 = s0 + 1.3f;
    if (s1 < R - 0.9f && rho > s0 && rho < s1) {
        const float a = (th + 3.1415927f) / 6.2831853f * 20.0f;
        const int seg = static_cast<int>(a);
        if (seg % 5 != 4 && a - static_cast<float>(seg) < 0.55f) c = mul(c, 0.72f);
    }
    return pack(c, g);
}

// 2026-10-28: a magazine's steel at (x, y) texels: a noise read through its gun's own texel distribution (its 10-90%
// points: the game's mottled bluing), fine scratches toward the light end along the face, grime toward the dark
uint32_t mag_texel(int m, int x, int y) {
    const MagMat& mt = kMagMat[m];
    const float px = static_cast<float>(x), py = static_cast<float>(y);
    float q = 0.5f + 3.0f * (fbm(px * 0.07f, py * 0.07f, 3.0f + m, 4) - 0.5f);  // (fbm's spread about 0.15: q about 10-90%)
    q += 0.18f * (vnoise(px * 0.45f, py * 0.45f, 11.0f + m) - 0.5f);
    if (vnoise(px * 0.9f, py * 0.05f, 1.0f + m) > 0.86f) q += 0.25f;  // scratches
    q = std::clamp(q, 0.0f, 1.0f);
    static const float kAt[5] = {0.10f, 0.25f, 0.50f, 0.75f, 0.90f};
    Col c = rgb8(mt.q[0][0], mt.q[0][1], mt.q[0][2]);
    for (int i = 0; i < 4; ++i) {
        if (q <= kAt[i]) break;
        const float t = std::clamp((q - kAt[i]) / (kAt[i + 1] - kAt[i]), 0.0f, 1.0f);
        c = mix(rgb8(mt.q[i][0], mt.q[i][1], mt.q[i][2]), rgb8(mt.q[i + 1][0], mt.q[i + 1][1], mt.q[i + 1][2]), t);
    }
    return pack(c, mt.gloss - 0.2f * std::fabs(q - 0.5f));
}

// The rects packed in shelves 512 wide (each with its gutter), the atlas's height the next power of two
void pack_atlas() {
    struct Item {
        Rect* r;
        int w, h;
    };
    std::vector<Item> items;
    for (int k = 0; k < kKinds; ++k) {
        KindTex& t = g_kt[k];
        float rmax = 0.0f;
        for (int i = 0; i < kDefs[k].n; ++i) rmax = std::fmax(rmax, kDefs[k].pts[i].r);
        auto q16 = [](float v, int lo, int hi) { return std::clamp(static_cast<int>(std::lround(v / 16.0f)) * 16, lo, hi); };
        t.side.w = q16(6.2831853f * rmax * kTexPerMm, 32, 128);
        t.side.h = q16(t.arc * kTexPerMm, 32, 160);
        t.base.w = t.base.h = kCapT;
        items.push_back({&t.side, t.side.w, t.side.h});
        items.push_back({&t.base, kCapT, kCapT});
        if (t.top_r > 0.0f) {  // a flat top only (the shell's crimp): the pointed noses map their tips on the side
            t.top.w = t.top.h = kCapT;
            items.push_back({&t.top, kCapT, kCapT});
        }
    }
    for (Rect& r : g_mrect) {
        r.w = r.h = kMagT;
        items.push_back({&r, kMagT, kMagT});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.h > b.h; });
    int x = 0, y = 0, shelf = 0;
    for (Item& it : items) {
        const int w = it.w + 2 * kGut, h = it.h + 2 * kGut;
        if (x + w > kAtlasW) x = 0, y += shelf, shelf = 0;
        it.r->x = x + kGut;
        it.r->y = y + kGut;
        x += w;
        shelf = std::max(shelf, h);
    }
    g_atlas_h = (y + shelf + 15) / 16 * 16;  // (a multiple of 16: the mips divide it; no power of two needed)
}

float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float lin_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

void make_atlas() {
    const int W = kAtlasW, H = g_atlas_h;
    g_atlas[0].assign(static_cast<size_t>(W) * H * 4, 0);
    uint32_t* px = reinterpret_cast<uint32_t*>(g_atlas[0].data());
    for (int k = 0; k < kKinds; ++k) {
        const KindTex& t = g_kt[k];
        const Rect& s = t.side;
        for (int ty = -kGut; ty < s.h + kGut; ++ty)
            for (int tx = -kGut; tx < s.w + kGut; ++tx) {  // round the axis wraps (the noise is sampled on the circle)
                const float u = (tx + 0.5f) / s.w, v = (ty + 0.5f) / s.h;
                px[(s.y + ty) * W + s.x + tx] = side_texel(k, u, v);
            }
        for (int which = 0; which < 2; ++which) {
            if (which && t.top_r <= 0.0f) continue;
            const Rect& c = which ? t.top : t.base;
            const float R = which ? t.top_r : t.base_r;
            for (int ty = -kGut; ty < c.h + kGut; ++ty)
                for (int tx = -kGut; tx < c.w + kGut; ++tx) {
                    const float x = ((tx + 0.5f) / c.w * 2.0f - 1.0f) * R, z = ((ty + 0.5f) / c.h * 2.0f - 1.0f) * R;
                    px[(c.y + ty) * W + c.x + tx] = cap_texel(k, which == 1, x, z);
                }
        }
    }
    for (int m = 0; m < 3; ++m) {  // the magazines' steel (their gutters too: clamped faces)
        const Rect& r = g_mrect[m];
        for (int ty = -kGut; ty < r.h + kGut; ++ty)
            for (int tx = -kGut; tx < r.w + kGut; ++tx) {
                const int cx = std::clamp(tx, 0, r.w - 1), cy = std::clamp(ty, 0, r.h - 1);
                px[(r.y + ty) * W + r.x + tx] = mag_texel(m, cx, cy);
            }
    }
    for (int m = 1; m < kMips; ++m) {  // a box filter in linear light (the colours); the gloss averaged as is
        const int w = W >> m, h = H >> m, pw = W >> (m - 1);
        g_atlas[m].assign(static_cast<size_t>(w) * h * 4, 0);
        const uint8_t* s = g_atlas[m - 1].data();
        uint8_t* o = g_atlas[m].data();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int ch = 0; ch < 4; ++ch) {
                    float acc = 0.0f;
                    for (int j = 0; j < 2; ++j)
                        for (int i = 0; i < 2; ++i) {
                            const float v = s[((y * 2 + j) * pw + x * 2 + i) * 4 + ch] / 255.0f;
                            acc += ch < 3 ? srgb_to_lin(v) : v;
                        }
                    acc *= 0.25f;
                    o[(y * w + x) * 4 + ch] = static_cast<uint8_t>(std::clamp(ch < 3 ? lin_to_srgb(acc) : acc, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
    }
}

// A kind's mesh, indexed: rings of sides + 1 vertices (the seam's twice, for its u) or, at a pole, one a side (each at
// its side's middle u: no pinched texels); a ring shared with the next band where both are on the side and turn by
// under 40 degrees (its normal their mean), else split
void lathe2(int k, Model* out) {
    const KindDef& d = kDefs[k];
    const KindTex& t = g_kt[k];
    const int S = d.sides;
    struct B {
        KPt a, b;
        float nr, ny;
        int map;  // 0 the side, 1 the base disc, 2 the top disc
        float va, vb;
    };
    std::vector<B> bands;
    float s = 0.0f;
    for (int i = 0; i + 1 < d.n; ++i) {
        const KPt &a = d.pts[i], &b = d.pts[i + 1];
        const float dr = b.r - a.r, dy = b.y - a.y, l = std::sqrt(dr * dr + dy * dy);
        if (l < 1e-6f) continue;
        B x{a, b, dy / l, -dr / l, 0, 0.0f, 0.0f};
        if (i == 0 && a.r < 1e-4f) x.map = 1;
        else if (i + 2 == d.n && b.r < 1e-4f && std::fabs(dy) < 1e-4f) x.map = 2;
        if (x.map == 0) {
            x.va = s / t.arc;
            s += l;
            x.vb = s / t.arc;
        }
        bands.push_back(x);
    }
    out->base = static_cast<int32_t>(g_mesh.size());
    out->first = static_cast<uint32_t>(g_idx.size());
    out->indexed = true;
    const float W = static_cast<float>(kAtlasW), H = static_cast<float>(g_atlas_h);
    auto ring = [&](const KPt& p, float nr, float ny, int map, float v) {
        const int at = static_cast<int>(g_mesh.size()) - out->base;
        const bool pole = p.r < 1e-4f;
        const int cnt = pole ? S : S + 1;
        for (int i = 0; i < cnt; ++i) {
            const float u = (static_cast<float>(i) + (pole ? 0.5f : 0.0f)) / static_cast<float>(S);
            const float th = 6.2831853f * u, c = std::cos(th), sn = std::sin(th);
            Vtx x{};
            x.p[0] = p.r * c * 0.001f, x.p[1] = p.y * 0.001f, x.p[2] = p.r * sn * 0.001f, x.p[3] = 0.0f;
            float n[3] = {nr * c, ny, nr * sn};
            if (pole && map == 0) n[0] = 0.0f, n[1] = ny >= 0.0f ? 1.0f : -1.0f, n[2] = 0.0f;  // the tip: along the axis
            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int j = 0; j < 3; ++j) x.n[j] = nl > 1e-6f ? n[j] / nl : 0.0f;
            if (map == 0) {
                x.uv[0] = (t.side.x + u * t.side.w) / W;
                x.uv[1] = (t.side.y + v * t.side.h) / H;
            } else {
                const Rect& r = map == 1 ? t.base : t.top;
                const float R = map == 1 ? t.base_r : t.top_r;
                x.uv[0] = (r.x + (p.r * c / R * 0.5f + 0.5f) * r.w) / W;
                x.uv[1] = (r.y + (p.r * sn / R * 0.5f + 0.5f) * r.h) / H;
            }
            g_mesh.push_back(x);
        }
        return at;
    };
    auto smooth_join = [&](size_t i, size_t j) {
        if (j >= bands.size() || bands[i].map || bands[j].map) return false;
        return bands[i].nr * bands[j].nr + bands[i].ny * bands[j].ny > 0.766f;  // under 40 degrees
    };
    auto tri = [&](int a, int b, int c) {
        g_idx.push_back(static_cast<uint16_t>(a));
        g_idx.push_back(static_cast<uint16_t>(b));
        g_idx.push_back(static_cast<uint16_t>(c));
    };
    int prev_end = -1;
    for (size_t i = 0; i < bands.size(); ++i) {
        const B& b = bands[i];
        const int A = i > 0 && smooth_join(i - 1, i) ? prev_end : ring(b.a, b.nr, b.ny, b.map, b.va);
        float enr = b.nr, eny = b.ny;
        if (smooth_join(i, i + 1)) {
            enr = 0.5f * (b.nr + bands[i + 1].nr), eny = 0.5f * (b.ny + bands[i + 1].ny);
            const float l = std::sqrt(enr * enr + eny * eny);
            if (l > 1e-6f) enr /= l, eny /= l;
        }
        const int Bv = ring(b.b, enr, eny, b.map, b.vb);
        const bool pa = b.a.r < 1e-4f, pb = b.b.r < 1e-4f;
        for (int j = 0; j < S; ++j) {
            if (pa && !pb) tri(A + j, Bv + j, Bv + j + 1);
            else if (pb && !pa) tri(A + j, A + j + 1, Bv + j);
            else if (!pa && !pb) tri(A + j, Bv + j, Bv + j + 1), tri(A + j, Bv + j + 1, A + j + 1);
        }
        prev_end = Bv;
        out->len = std::fmax(out->len, b.b.y * 0.001f);
        out->rad = std::fmax(out->rad, std::fmax(b.a.r, b.b.r) * 0.001f);
    }
    out->count = static_cast<uint32_t>(g_idx.size()) - out->first;
}

// ---- 2026-10-28 ([Reload] PistolMagazines; research/run28/magazines.md): the magazines and the stripper clip, our own,
// at the game's own magazines' measures: each part a profile across (x, z; mm, a rectangle with its corners chamfered)
// carried along y in a few rings, flat-shaded at its faces as the game's low-poly gun parts are, about the game's
// density (1.1-2.3 triangles/cm2, the guns' 1.1-1.4; the magazines' bases 1.2-2.3), each face mapped onto its
// magazine's steel rect at its gun's texel density (the model's frame x right, y up, z back, the foot at 0)
constexpr int kClipN = 10;  // its rounds
constexpr float kClipGap = 9.9f, kClipFoot = 5.5f, kClipHead = 3.4f;  // the rounds in the clip: their spacing, the first's centre, their base (z)
Model g_magm[3];  // the High Power's, the Semi-Auto's, the clip
float g_magb[3][6] = {};  // each one's bounds (m): x z min, x z max... (xmin, zmin, xmax, zmax, ymin, ymax)
int mag_model(int weapon) { return weapon == 2 ? 0 : weapon == 1 ? 1 : weapon == 3 ? 2 : -1; }
// a rectangle centred (cx, cz), half sizes (hx, hz), its corners cut by ch (0: none), counter-clockwise (positive area)
int rect_prof(float cx, float cz, float hx, float hz, float ch, float* o) {
    if (ch <= 0.0f) {
        const float p[8] = {cx - hx, cz - hz, cx + hx, cz - hz, cx + hx, cz + hz, cx - hx, cz + hz};
        std::memcpy(o, p, sizeof(p));
        return 4;
    }
    const float p[16] = {cx - hx + ch, cz - hz, cx + hx - ch, cz - hz, cx + hx, cz - hz + ch, cx + hx, cz + hz - ch,
                         cx + hx - ch, cz + hz, cx - hx + ch, cz + hz, cx - hx, cz + hz - ch, cx - hx, cz - hz + ch};
    std::memcpy(o, p, sizeof(p));
    return 8;
}
// one part: the convex profile pr (n points, x z pairs, counter-clockwise) from y0 to y1 in segs rings, its ends capped
// or not, into the model out (its magazine m's rect at kMagMat[m].tpm)
void extrude(const float* pr, int n, float y0, float y1, int segs, bool cap0, bool cap1, int m, Model* out) {
    const Rect& r = g_mrect[m];
    const float k = kMagMat[m].tpm, W = static_cast<float>(kAtlasW), H = static_cast<float>(g_atlas_h);
    auto uv = [&](float u, float v, float* o) {  // texels within the rect (clamped to it)
        o[0] = (static_cast<float>(r.x) + std::clamp(u, 0.0f, static_cast<float>(r.w))) / W;
        o[1] = (static_cast<float>(r.y) + std::clamp(v, 0.0f, static_cast<float>(r.h))) / H;
    };
    auto vert = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        Vtx t{};
        t.p[0] = x * 0.001f, t.p[1] = y * 0.001f, t.p[2] = z * 0.001f;
        t.n[0] = nx, t.n[1] = ny, t.n[2] = nz;
        uv(u, v, t.uv);
        g_mesh.push_back(t);
        float* b = g_magb[m];
        b[0] = std::fmin(b[0], t.p[0]), b[1] = std::fmin(b[1], t.p[2]), b[2] = std::fmax(b[2], t.p[0]), b[3] = std::fmax(b[3], t.p[2]);
        b[4] = std::fmin(b[4], t.p[1]), b[5] = std::fmax(b[5], t.p[1]);
        out->len = std::fmax(out->len, t.p[1]);
        out->rad = std::fmax(out->rad, std::fmax(std::fabs(t.p[0]), std::fabs(t.p[2])));
        return static_cast<int>(g_mesh.size()) - 1 - out->base;
    };
    auto quad = [&](int a, int b, int c, int e) {
        for (int i : {a, b, c, a, c, e}) g_idx.push_back(static_cast<uint16_t>(i));
    };
    float s = 0.0f;  // the arc along the profile (mm)
    for (int i = 0; i < n; ++i) {
        const float ax = pr[i * 2], az = pr[i * 2 + 1], bx = pr[(i + 1) % n * 2], bz = pr[(i + 1) % n * 2 + 1];
        const float ex = bx - ax, ez = bz - az, el = std::sqrt(ex * ex + ez * ez);
        if (el < 1e-4f) continue;
        const float nx = ez / el, nz = -ex / el;  // outward (counter-clockwise)
        if ((s + el) * k > static_cast<float>(r.w)) s = 0.0f;  // (the review: past the rect it would smear: the face starts over)
        for (int j = 0; j < segs; ++j) {
            const float ya = y0 + (y1 - y0) * j / segs, yb = y0 + (y1 - y0) * (j + 1) / segs;
            const int v0 = vert(ax, ya, az, nx, 0.0f, nz, s * k, ya * k), v1 = vert(bx, ya, bz, nx, 0.0f, nz, (s + el) * k, ya * k);
            const int v2 = vert(bx, yb, bz, nx, 0.0f, nz, (s + el) * k, yb * k), v3 = vert(ax, yb, az, nx, 0.0f, nz, s * k, yb * k);
            quad(v0, v1, v2, v3);
        }
        s += el;
    }
    for (int end = 0; end < 2; ++end) {  // a fan (the profile is convex)
        if (!(end ? cap1 : cap0)) continue;
        const float y = end ? y1 : y0, ny = end ? 1.0f : -1.0f;
        const int c0 = static_cast<int>(g_mesh.size()) - out->base;
        for (int i = 0; i < n; ++i) vert(pr[i * 2], y, pr[i * 2 + 1], 0.0f, ny, 0.0f, (pr[i * 2] + 40.0f) * k, (pr[i * 2 + 1] + 40.0f) * k);
        for (int i = 1; i + 1 < n; ++i)
            for (int t : {c0, c0 + i, c0 + i + 1}) g_idx.push_back(static_cast<uint16_t>(t));
    }
}
void build_mags() {
    float pr[16];
    for (int m = 0; m < 3; ++m) {
        Model& o = g_magm[m];
        o.base = static_cast<int32_t>(g_mesh.size());
        o.first = static_cast<uint32_t>(g_idx.size());
        o.indexed = true;
        g_magb[m][0] = g_magb[m][1] = g_magb[m][4] = 1e9f;
        g_magb[m][2] = g_magb[m][3] = g_magb[m][5] = -1e9f;
        if (m == 0) {  // the High Power's (the FN 1903's; pistol_highpower01x's grip bottom 18 x 45 mm, its well 13 x 34)
            extrude(pr, rect_prof(0.0f, 0.5f, 9.0f, 22.5f, 3.0f, pr), 0.0f, 5.0f, 1, true, true, m, &o);  // the floorplate
            extrude(pr, rect_prof(0.0f, 0.0f, 6.5f, 17.0f, 2.0f, pr), 5.0f, 90.0f, 5, false, true, m, &o);  // the body
            for (float sx : {-4.6f, 4.6f}) extrude(pr, rect_prof(sx, -3.0f, 1.6f, 12.0f, 0.5f, pr), 88.0f, 93.0f, 1, false, true, m, &o);  // the lips
        } else if (m == 1) {  // the Semi-Auto's (the Borchardt's; pistol_semiauto01x's base 22 x 19 x 9 mm below its grip,
            // forward of the well's middle; the game's dark steel, not a walnut)
            extrude(pr, rect_prof(0.0f, -9.0f, 11.0f, 9.5f, 4.0f, pr), 0.0f, 9.0f, 1, true, true, m, &o);   // the base
            extrude(pr, rect_prof(0.0f, 0.0f, 6.5f, 18.0f, 2.0f, pr), 9.0f, 92.0f, 5, false, true, m, &o);  // the body
            for (float sx : {-4.6f, 4.6f}) extrude(pr, rect_prof(sx, -3.0f, 1.6f, 12.0f, 0.5f, pr), 90.0f, 95.0f, 1, false, true, m, &o);
        } else {  // the C96's stripper clip: the back plate behind the case heads, the two lips round the rims, the end tabs
            extrude(pr, rect_prof(0.0f, 4.2f, 5.5f, 0.6f, 0.0f, pr), 0.0f, 101.0f, 2, true, true, m, &o);
            for (float sx : {-5.0f, 5.0f}) extrude(pr, rect_prof(sx, 2.0f, 0.5f, 2.0f, 0.0f, pr), 0.0f, 101.0f, 1, true, true, m, &o);
            extrude(pr, rect_prof(0.0f, 2.0f, 5.5f, 2.6f, 0.0f, pr), 0.0f, 1.0f, 1, false, true, m, &o);
            extrude(pr, rect_prof(0.0f, 2.0f, 5.5f, 2.6f, 0.0f, pr), 100.0f, 101.0f, 1, true, false, m, &o);
        }
        o.count = static_cast<uint32_t>(g_idx.size()) - o.first;
    }
}

// The vertex shader carries the model into the eye's view (L: rows of the model-to-view rotation, w the view-space
// translation) and projects it as the game's Perspective does (row-vector, right-handed, standard Z): x' = (2x + (r+l)z)
// / (r-l), y' likewise, z' = P22 z + P32, w = -z. The pixel shader drops what lies behind the game's own depth there.
// (The old models' shader, [Reload] RoundModels = 0, as it was: a light in the head, its colour constants.)
const char kShader[] = R"(
cbuffer K : register(b0) {
    float4 L0, L1, L2;      // the model-to-view rows (xyz) and translation (w)
    float4 proj;            // 2/(r-l), (r+l)/(r-l), 2/(u-d), (u+d)/(u-d)
    float4 zc;              // P22, P32, near, far
    float4 scale;           // depth texels per image pixel (x, y), brightness, the depth bias (m)
    uint4 dinfo;            // the eye's byte offset, the row pitch (bytes), width, height
    uint4 dfmt;             // 0 D24 in the low bits, 1 float32; the game's depth used (1) or not (0)
};
ByteAddressBuffer depth : register(t0);
struct V { float4 pos : SV_Position; float3 vp : TEXCOORD0; float3 n : TEXCOORD1; nointerpolation float mat : TEXCOORD2; };
V vs(float4 p : POSITION, float3 n : NORMAL) {
    V o;
    float3 v = float3(dot(L0.xyz, p.xyz) + L0.w, dot(L1.xyz, p.xyz) + L1.w, dot(L2.xyz, p.xyz) + L2.w);
    o.pos = float4(proj.x * v.x + proj.y * v.z, proj.z * v.y + proj.w * v.z, zc.x * v.z + zc.y, -v.z);
    if (dfmt.z) o.pos = float4(p.x * 12, p.y * 12 - 0.3, 0.5 + p.z * 4, 1);  // a test: the model enlarged in the image's centre
    o.vp = v;
    o.n = float3(dot(L0.xyz, n), dot(L1.xyz, n), dot(L2.xyz, n));
    o.mat = p.w;
    return o;
}
float4 ps(V i) : SV_Target {
    if (dfmt.y) {
        uint2 q = min(uint2(i.pos.xy * scale.xy), uint2(dinfo.z - 1, dinfo.w - 1));
        uint raw = depth.Load(dinfo.x + q.y * dinfo.y + q.x * 4);
        float d = dfmt.x ? asfloat(raw) : (raw & 0xFFFFFF) / 16777215.0;
        float game = zc.z * zc.w / (zc.w - d * (zc.w - zc.z));  // the game's distance along the view axis there
        if (game < -i.vp.z - scale.w) discard;
    }
    float3 N = normalize(i.n), V = normalize(-i.vp), Ld = normalize(float3(0.35, 0.85, 0.40)), H = normalize(Ld + V);
    float3 alb, spc;
    float sh;
    if (i.mat < 0.5) { alb = float3(0.50, 0.33, 0.10); spc = float3(0.85, 0.68, 0.38); sh = 48; }
    else if (i.mat < 1.5) { alb = float3(0.20, 0.20, 0.22); spc = float3(0.25, 0.25, 0.27); sh = 18; }
    else { alb = float3(0.33, 0.03, 0.02); spc = float3(0.06, 0.05, 0.05); sh = 8; }
    float dif = saturate(dot(N, Ld)), rim = saturate(1 - abs(dot(N, V)));
    float3 c = alb * (0.22 + 0.78 * dif) + spc * pow(saturate(dot(N, H)), sh) * (dif > 0) + alb * rim * rim * 0.25;
    return float4(c * scale.z, 1);
}
)";

// 2026-10-27: the new models' shader. The atlas's colour (sRGB, read linear) and gloss; a key light and a sky and ground
// ambient fixed in the world (key: toward the light, from above and the body's front; upv: the world's up, both in the
// eye's view space: the highlight stays put when the head turns); one broad soft specular lobe, no rim boost; the
// scene's light round the round (the probe texel: its median luminance against lum_ref, clamped 0.15-1.5, and a pull
// toward its colour) so it darkens in the hand's shade, at dusk and indoors as the game's own props do; the silhouette
// faded over its last pixel and the game's nearer depth taken over a 2 x 2 bilinear footprint (the fingers' edge soft;
// drawn after DLSS, with no MSAA, the round's hard edges stepped against the game's smooth image).
const char kShader2[] = R"(
cbuffer K : register(b0) {
    float4 L0, L1, L2;      // the model-to-view rows (xyz) and translation (w)
    float4 proj;            // 2/(r-l), (r+l)/(r-l), 2/(u-d), (u+d)/(u-d)
    float4 zc;              // P22, P32, near, far
    float4 scale;           // depth texels per image pixel (x, y), brightness, the depth bias (m)
    uint4 dinfo;            // the eye's byte offset, the row pitch (bytes), width, height
    uint4 dfmt;             // 0 D24 in the low bits, 1 float32; the game's depth used (1) or not (0); the ndc test
    float4 key;             // toward the key light (view space), its strength
    float4 upv;             // the world's up (view space), the ambient's strength
    float4 probe;           // lum_ref, the pull toward the scene's colour, the probe texel, used (1) or not (0)
};
ByteAddressBuffer depth : register(t0);
Texture2D<float4> atlas : register(t1);
Texture2D<float4> probes : register(t3);
SamplerState lin : register(s0);
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float3 kDay = float3(1.14, 0.985, 0.71);  // the probe's colour in daylight (cycle r169, the ranch at noon)
struct V { float4 pos : SV_Position; float3 vp : TEXCOORD0; float3 n : TEXCOORD1; float2 uv : TEXCOORD2; };
V vs(float4 p : POSITION, float3 n : NORMAL, float2 uv : TEXCOORD0) {
    V o;
    float3 v = float3(dot(L0.xyz, p.xyz) + L0.w, dot(L1.xyz, p.xyz) + L1.w, dot(L2.xyz, p.xyz) + L2.w);
    o.pos = float4(proj.x * v.x + proj.y * v.z, proj.z * v.y + proj.w * v.z, zc.x * v.z + zc.y, -v.z);
    if (dfmt.z) o.pos = float4(p.x * 12, p.y * 12 - 0.3, 0.5 + p.z * 4, 1);  // a test: the model enlarged in the image's centre
    o.vp = v;
    o.n = float3(dot(L0.xyz, n), dot(L1.xyz, n), dot(L2.xyz, n));
    o.uv = uv;
    return o;
}
float game_z(uint2 q) {  // the game's distance along the view axis at a depth texel
    uint raw = depth.Load(dinfo.x + q.y * dinfo.y + q.x * 4);
    float d = dfmt.x ? asfloat(raw) : (raw & 0xFFFFFF) / 16777215.0;
    return zc.z * zc.w / (zc.w - d * (zc.w - zc.z));
}
float hidden(float2 px, float z) {  // the share of the pixel the game's scene is nearer at, over 2 x 2 depth texels
    float2 f = px * scale.xy - 0.5;
    int2 b = int2(floor(f));
    float2 w = f - float2(b);
    int2 hi = int2(dinfo.z, dinfo.w) - 1;
    float o = 0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i) {
            uint2 q = uint2(clamp(b + int2(i, j), int2(0, 0), hi));
            float wt = (i ? w.x : 1 - w.x) * (j ? w.y : 1 - w.y);
            o += game_z(q) < z - scale.w ? wt : 0;
        }
    return o;
}
float4 ps(V i) : SV_Target {
    float4 t = atlas.Sample(lin, i.uv);
    float3 N = normalize(i.n), Vd = normalize(-i.vp), Lk = normalize(key.xyz), U = normalize(upv.xyz);
    float nv = dot(N, Vd);
    float edge = fwidth(nv);
    float occ = dfmt.y ? hidden(i.pos.xy, -i.vp.z) : 0;
    clip(0.995 - occ);
    float dif = saturate(dot(N, Lk));
    float hemi = 0.5 + 0.5 * dot(N, U);
    float3 amb = lerp(float3(0.50, 0.46, 0.42), float3(0.80, 0.86, 0.96), hemi) * upv.w;
    float g = t.a;
    float3 H = normalize(Lk + Vd);
    float sh = 4.0 + 28.0 * g;
    float3 tint = t.rgb / max(max(t.r, max(t.g, t.b)), 0.02);
    float3 spc = lerp(float3(0.9, 0.9, 0.9), tint, saturate(g * 1.15)) * (0.30 * g * g);
    float3 c = t.rgb * (amb + key.w * dif) + spc * pow(saturate(dot(N, H)), sh) * dif;
    if (probe.w > 0.5) {
        float4 pr = probes.Load(int3(int(probe.z), 0, 0));
        float f = clamp(pr.w / max(probe.x, 1e-4), 0.15, 1.5);
        float3 cc = lerp(float3(1, 1, 1), max(pr.rgb, 0) / kDay, probe.y);
        cc /= max(dot(cc, kLuma), 1e-3);
        c *= f * cc;
    }
    c *= scale.z;
    float a = saturate(abs(nv) / max(edge, 1e-4)) * (1.0 - occ);
    return float4(c, a);
}
)";

// 2026-10-27: the scene's light round a round, one texel per (eye, subject) of a small RGBA16F target, blended with
// the texel's last value (the blend factor: about 0.3 s): 12 taps of the eye image the blit just drew (its staging
// texture, linearised as the blit does: pow(gamma), then sRGB to linear) on a ring about 1.8 times the round's screen
// radius round its centre, the sky's taps skipped (the depth at its far plane; all sky: the sky's), their median
// luminance (w) and their mean colour at luminance 1 (rgb).
const char kShaderProbe[] = R"(
cbuffer P : register(b0) {
    float4 c0;   // the ring's centre (image px), its radius (px), the blit's gamma
    float4 c1;   // eye texels per image px (xy), depth texels per image px (zw)
    uint4 d0;    // the eye's depth: byte offset, row pitch, width, height
    uint4 d1;    // the depth's format (1 float32), the depth used (1), the image's width and height
};
ByteAddressBuffer depth : register(t0);
Texture2D<float4> eye : register(t2);
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 to_linear(float3 c) {
    c = pow(saturate(c), c0.w);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float l[12];
    float3 rgb[12];
    bool ok[12];
    int n = 0;
    [unroll] for (int k = 0; k < 12; ++k) {
        float a = 0.5235988 * k + 0.2617994;
        float2 p = clamp(c0.xy + c0.z * float2(cos(a), sin(a)), 0.5, float2(d1.zw) - 0.5);
        rgb[k] = to_linear(eye.Load(int3(int2(p * c1.xy), 0)).rgb);
        l[k] = dot(rgb[k], kLuma);
        bool sky = false;
        if (d1.y) {
            uint2 q = min(uint2(p * c1.zw), uint2(d0.z - 1, d0.w - 1));
            uint raw = depth.Load(d0.x + q.y * d0.y + q.x * 4);
            sky = d1.x ? asfloat(raw) >= 0.99999 : (raw & 0xFFFFFF) >= 0xFFFFF0;
        }
        ok[k] = !sky;
        n += sky ? 0 : 1;
    }
    bool all = n == 0;
    if (all) n = 12;
    float med = 0;
    float3 sum = 0;
    [unroll] for (int i = 0; i < 12; ++i) {
        if (ok[i] || all) {
            sum += rgb[i];
            int below = 0;
            [unroll] for (int j = 0; j < 12; ++j)
                below += ((ok[j] || all) && (l[j] < l[i] || (l[j] == l[i] && j < i))) ? 1 : 0;
            if (below == ((n - 1) >> 1)) med = l[i];
        }
    }
    float3 m = sum / n;
    return float4(m / max(dot(m, kLuma), 1e-4), med);
}
)";

constexpr int kConsts = 44;  // dwords (the old shader reads the first 32)
struct Consts {
    float L[3][4];
    float proj[4];
    float zc[4];
    float scale[4];
    uint32_t dinfo[4];
    uint32_t dfmt[4];
    float key[4];
    float upv[4];
    float probe[4];
};
static_assert(sizeof(Consts) == kConsts * 4, "root constants");
struct ProbeConsts {
    float c0[4];
    float c1[4];
    uint32_t d0[4];
    uint32_t d1[4];
};

constexpr int kSlots = 4;  // >= xr_blit's lists
constexpr int kKeep = 2;   // depth slots (frame parity)
constexpr int kPerSlot = 6;  // descriptors a slot: per eye t0 the depth, t1 the atlas, t2 the eye image
constexpr int kProbeTexels = 8;  // [eye][subject]: 0 the hand's, 1 the row's, 2 the "round show" test's
enum Subject { kSubHand = 0, kSubRow = 1, kSubShow = 2, kSubjects = 3 };

// ---- settings
std::atomic<bool> g_on{false};
std::atomic<int> g_mode{0};  // [Reload] RoundInHand: 0 off, 1 the mod's round, 2 also the game's own shells for shotguns
std::atomic<bool> g_models{true};  // [Reload] RoundModels (2026-10-27): the new rounds (1) or the old lathe (0)
std::mutex g_cfg_mutex;
float g_offset[3] = {0.0f, 0.0f, 0.0f};  // moved from the pinch in the hand's IK target axes (the wrist bone's), m
float g_axis[3] = {1.0f, 0.0f, 0.0f};    // the round's base-to-tip in those axes (x: along the fingers), when not "auto"
bool g_axis_auto = false;                // auto: out of the hand, from the wrist through the pinch
float g_bright = 1.0f, g_bias = 0.004f;
float g_mag_off[3] = {0.0f, 0.0f, 0.0f};  // (2026-10-29, round 38) [Reload] MagazineOffset: the held magazine moved (the hand's axes, m)
float g_mag_turn = 0.0f, g_mag_tilt = 0.0f;  // MagazineTurn, MagazineTilt (degrees)
// (2026-10-31, the off-hand build step 4, R13; 55.5, the user: "let me adjust left hand and right ammop positions
// seperately in the mod menu") the right hand's own: [Reload] RoundOffsetRight, RoundAxisRight, MagazineOffsetRight,
// MagazineTurnRight, MagazineTiltRight. Until set (none in the ini, nothing chosen in the menu), the right hand uses the
// keys above, which stay the left hand's (the hand the user tuned them with: r78 E1, the two not each other's mirror)
float g_r_offset[3] = {0.0f, 0.0f, 0.0f}, g_r_axis[3] = {1.0f, 0.0f, 0.0f};
bool g_r_axis_auto = false, g_r_round_own = false;
float g_r_mag_off[3] = {0.0f, 0.0f, 0.0f}, g_r_mag_turn = 0.0f, g_r_mag_tilt = 0.0f;
bool g_r_mag_own = false;
// (C38) each magazine pistol's own (weapon 1-3; [hand] John's 0 left, 1 right), under g_cfg_mutex
struct WMag {
    bool own = false;
    float off[3] = {0.0f, 0.0f, 0.0f}, turn = 0.0f, tilt = 0.0f;
};
WMag g_wmag[4][2];
bool wmag_ok(int w) { return w >= 1 && w <= 3; }
std::atomic<bool> g_occlude{true}, g_ndc{false};
std::atomic<int> g_axes_from{0};  // the round's axes: 1 the IK target's, 2 the drawn wrist's, 0 neither valid
std::atomic<float> g_out_mm{0.0f};  // (cycle r169) the last pose's move out along the round past the pinch (mm)
// (cycle r170) the placement's path: the last (0 the wrist, no tips; 1 the pinch; 2 the flat hand, along the index
// finger) and the poses by each, and the poses found not finite (put at the hand)
std::atomic<int> g_path{0};
std::atomic<uint64_t> g_path_pinch{0}, g_path_flat{0}, g_path_bad{0};
std::mutex g_last_mutex;
float g_last_k[2][kConsts];  // the last constants per eye (a test aid)
// the scene light probe (2026-10-27; "round probe"): lum_ref the daylight scene's own (cycle r169 measured 0.38-0.51
// round the rounds at the ranch at noon: the first guess, 0.18, clamped every round at 1.5 times, over-bright and
// over-saturated), so the albedos, taken from game-lit captures, are drawn as they are in daylight and darker in shade;
// the pull toward the scene's colour relative to daylight's (the shader's kDay: in daylight no tint at all, the warmth
// being in the albedos already)
std::atomic<bool> g_probe_on{true};
std::atomic<float> g_probe_ref{0.45f}, g_probe_pull{0.25f};
std::mutex g_probe_mutex;
float g_probe_val[kProbeTexels][4] = {};  // the last read back (status)
double g_probe_ms[kSubjects] = {0, 0, 0};  // presenting thread: each subject's last probe

// ---- the holder
std::atomic<int> g_hand{-1}, g_weapon{-1}, g_count{1};
std::atomic<float> g_fan_sign{1.0f};  // "round fan -1": the handful on the pinch's other side (a test aid)
std::atomic<double> g_held_ms{0};
std::mutex g_fwd_mutex;
float g_fwd[3] = {0.0f, 0.0f, -1.0f};  // the body's forward (world, level): the new rounds' key light

bool held_now() {
    return g_hand.load(std::memory_order_relaxed) >= 0 && log::now_ms() - g_held_ms.load(std::memory_order_relaxed) < 250.0;
}

// ---- the ammo row ([Reload] ShowAmmo, run 6 item 6b)
std::atomic<bool> g_row_on{false};
struct Row {
    int weapon = -1, n = 0;
    float at[3] = {}, rgt[3] = {}, up[3] = {}, fwd[3] = {};
    double ms = 0;
};
std::mutex g_row_mutex;
Row g_row;
std::atomic<uint64_t> g_row_px[2] = {0, 0}, g_row_drawn{0};
bool row_now() {  // a row wanted within the last 250 ms
    if (!g_row_on.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_row_mutex);
    return g_row.n > 0 && audio::click_family(g_row.weapon) > 0 && log::now_ms() - g_row.ms < 250.0;
}

// ---- "round show <kind> ..." (2026-10-27, a test aid: one round at a fixed world place, beside the game's own shell)
struct Show {
    bool on = false;
    int kind = 0;
    float pz[12] = {};
};
std::mutex g_show_mutex;
Show g_show;
std::atomic<bool> g_show_on{false};

// ---- 2026-10-28 ([Reload] PistolMagazines): the magazine (the clip) in a hand, those let fall, the Mauser's seated clip
std::mutex g_mag_mutex;
int g_mag_hand = -1, g_mag_weapon = -1, g_mag_rounds = 0;
double g_mag_ms = -1e12;
struct Drop {
    float pz[12];
    double t0 = -1e12;
    int weapon = -1, rounds = 0;
};
Drop g_drop[3];
int g_ndrop = 0;
struct Seat {
    float pz[12] = {};
    int rounds = 0;
    double ms = -1e12;
};
Seat g_seat;
// "round showmag" (a test aid, the pictures'): one magazine (clip) standing at a world place, with its rounds
struct MShow {
    bool on = false;
    int weapon = -1, rounds = 0;
    float pz[12] = {};
};
MShow g_mshow;
// (step 7, R12) the chest's magazine row (under g_mag_mutex)
struct MRow {
    int weapon = -1, n = 0, rounds = 0;
    float at[3] = {}, rgt[3] = {}, up[3] = {}, fwd[3] = {};
    double ms = 0.0;
};
MRow g_mrow;
std::atomic<uint64_t> g_mag_drawn{0}, g_drops_made{0}, g_mag_items{0};
std::atomic<uint64_t> g_mrow_drawn{0};  // (step 7, R12) frames the magazine row was drawn in
std::atomic<float> g_mag_proj[2] = {0.0f, 0.0f};  // the held magazine's projected box in each eye (px), the last drawn
// (the review) the held magazine's own model, each eye: its pixels drawn past the scene's depth (seen) and all of them
// (the same draw, the scene's depth not tested, nothing written to the image): the share of it in view, the hand and the
// world in front of the rest. (Its rounds not counted.) g_q_mag: the slot's queries hold them
std::atomic<uint64_t> g_mag_seen[2] = {0, 0}, g_mag_all[2] = {0, 0};
bool g_q_mag[8] = {};
int g_q_kind[8] = {};  // (the review) the kind the slot's counts are of: 1 the held, 2 the shown
std::atomic<int> g_mag_seen_kind{0};  // the kind of the counts last read
// "round maghold old|new" (a test aid: r44's positive control): the r219 hold, the held end 1.5 cm behind the pinch
std::atomic<bool> g_mag_hold_old{false};
std::atomic<int> g_mag_grip_axis{0};  // the last held pose's grip axis (the gun's up, mirrored): 0 none, 1 the drawn frame's, 2 the gun hand's target's
std::atomic<const char*> g_mag_axis_why{""};  // why none (a literal)
std::atomic<float> g_mag_cover{-1.0f};  // the held one's length from its held end to the farther drawn fingertip along it (m)
std::atomic<uint32_t> g_iw_last{0}, g_ih_last{0};  // the eye image's size the last record drew ("round project" reads it)
// the measured magazine (the held one, else the shown one): its long axis against each eye's ray to its middle (degrees)
std::atomic<float> g_mag_view_deg[2] = {-1.0f, -1.0f};
std::atomic<int> g_mag_measured{0};  // 1 the held, 2 the shown, 0 none
constexpr double kDropMs = 600.0;  // a magazine let fall is gone after this
bool mags_now() {
    const double now = log::now_ms();
    std::lock_guard lock(g_mag_mutex);
    bool any = now - g_mag_ms < 250.0 || now - g_seat.ms < 250.0 || g_mshow.on || (g_mrow.n > 0 && now - g_mrow.ms < 250.0);
    for (const Drop& x : g_drop) any = any || now - x.t0 < kDropMs;
    return any;
}

// ---- the game's shells' cover (mode 2) and the stand-ins drawn for them
std::atomic<uint64_t> g_covered{0}, g_stand_ins{0};
std::atomic<int> g_hand_cover{0}, g_hand_want{0}, g_row_cover{0}, g_row_want{0};  // the last drawn frame's

// ---- the pass cameras, by frame (state::presents when the pass ran)
struct PassCam {
    double a[3], b[3], c[3], d[3];
    float l, r, u, dn;
    uint64_t frame = ~0ull;
};
std::mutex g_cam_mutex;
PassCam g_cam[kKeep][2];
uint64_t g_cam_frame_seen = ~0ull;
int g_first_eye[kKeep] = {0, 0};

// ---- the depth copies: one DEFAULT buffer, [parity][eye] regions of `g_stride` bytes
std::atomic<ID3D12Resource*> g_dbuf{nullptr};
std::atomic<uint64_t> g_dbuf_size{0};
std::atomic<uint64_t> g_need{0};  // the size the taps wanted (the presenting thread makes it)
// the "Depth Resolve" the game copies its scene depth into, taken from the copies themselves (2026-10-11: the game names
// several, 4808x2704 and DLSS's render size among them, and the names of freed ones stay; find_resource's pick was any of
// them, so with the wrong one no copy was tapped and the rounds were never drawn: the user's "invisible ammo")
std::atomic<const void*> g_resolve{nullptr};
std::atomic<uint64_t> g_resolve_frame{0}, g_resolve_other{0};
struct DepthMeta {
    uint32_t w = 0, h = 0, pitch = 0, fmt = 0;
    uint64_t stride = 0;
};
std::mutex g_depth_mutex;
DepthMeta g_dmeta;
uint64_t g_dframe[kKeep][2] = {{~0ull, ~0ull}, {~0ull, ~0ull}};
uint64_t g_tap_frame = ~0ull;
int g_tap_k = 0;
std::atomic<uint64_t> g_taps{0}, g_tap_extra{0}, g_tap_frames{0};
std::atomic<int> g_tap_max{0};
std::atomic<uint32_t> g_tap_tid{0}, g_pass_tid{0}, g_rec_tid{0};

struct Grave {
    ID3D12Resource* r;
    uint64_t at;
};
std::vector<Grave> g_grave;  // presenting thread

// ---- the presenting thread's pipeline
ID3D12Device* g_dev = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;        // the old models
ID3D12PipelineState* g_pso2 = nullptr;       // the new (2026-10-27)
ID3D12PipelineState* g_pso2c = nullptr;      // (2026-10-28) the new, its colour not written: a count of a model's pixels only
ID3D12PipelineState* g_pso_probe = nullptr;  // the scene light probe
ID3D12DescriptorHeap* g_srv_heap = nullptr;  // kSlots x kPerSlot SRVs (each slot's written when it is free), then the probe's
ID3D12DescriptorHeap* g_dsv_heap = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // the probe target's
ID3D12Resource* g_vb = nullptr;
ID3D12Resource* g_ib = nullptr;
ID3D12Resource* g_zbuf = nullptr;  // the round's own depth (it hides its own far side)
ID3D12Resource* g_atlas_tex = nullptr;
ID3D12Resource* g_atlas_up = nullptr;  // its upload, released once the copy is behind
// (the review of 2026-10-27) the atlas's state: 0 its copy not yet recorded (the texture in COPY_DEST, unwritten), 1
// recorded in slot g_atlas_slot's list (that list may still fail to close), 2 submitted (a shader resource from then
// on). Until 2 the new rounds are drawn only in the list that copied it (after the copy); any other draws the old
// lathe. list_submitted (xr_blit, after the list's Close) moves 1 to 2, or back to 0 when it was not submitted
int g_atlas_state = 0, g_atlas_slot = -1;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_atlas_fp[kMips] = {};
ID3D12Resource* g_probe_tex = nullptr;  // kProbeTexels x 1 RGBA16F, kept in PIXEL_SHADER_RESOURCE between uses
ID3D12Resource* g_probe_rb = nullptr;   // kSlots x 512 bytes: each slot's copy of it (read when the slot comes round)
bool g_probe_pending[kSlots] = {};
ID3D12QueryHeap* g_qheap = nullptr;
ID3D12Resource* g_qread = nullptr;  // kSlots x kQ occlusion counts
constexpr int kQ = 8;  // a slot's: the hand's (2 eyes), the row's (2), the held magazine's seen (2) and all of it (2)
UINT g_srv_step = 0;
uint32_t g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
bool g_ready = false, g_failed = false;
bool g_q_pending[kSlots] = {};
std::atomic<uint64_t> g_drawn{0}, g_no_cam{0}, g_no_depth{0}, g_records{0}, g_probes{0};
std::atomic<uint64_t> g_px[2] = {0, 0};  // the last counted pixels per eye
std::atomic<uint64_t> g_px_frames{0}, g_px_seen[2] = {0, 0};

// ---- the eye capture (a test aid)
std::mutex g_grab_mutex;
std::string g_grab_path, g_grab_result;
std::atomic<int> g_grab_phase{0};  // 0 idle, 1 armed, 2 copied (slot g_grab_slot), 3 done
int g_grab_slot = -1, g_grab_eye = 0;
bool g_grab_full = false;
ID3D12Resource* g_grab_rb = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_grab_fp{};
int g_grab_cx = 0, g_grab_cy = 0;
HANDLE g_grab_done = nullptr;

bool show_now() { return g_show_on.load(std::memory_order_relaxed); }

using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                   ID3DBlob**, ID3DBlob**);

ID3DBlob* compile(Compile_t fn, const char* src, size_t len, const char* name, const char* entry, const char* target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = fn(src, len, name, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) log::error("[round] %s %s does not compile: %s", name, entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    if (err) err->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}

bool make(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    g_dev = dev;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!fn) return false;
    ID3DBlob* vs = compile(fn, kShader, sizeof(kShader) - 1, "rdrvr_round", "vs", "vs_5_0");
    ID3DBlob* ps = compile(fn, kShader, sizeof(kShader) - 1, "rdrvr_round", "ps", "ps_5_0");
    ID3DBlob* vs2 = compile(fn, kShader2, sizeof(kShader2) - 1, "rdrvr_round2", "vs", "vs_5_0");
    ID3DBlob* ps2 = compile(fn, kShader2, sizeof(kShader2) - 1, "rdrvr_round2", "ps", "ps_5_0");
    ID3DBlob* vsp = compile(fn, kShaderProbe, sizeof(kShaderProbe) - 1, "rdrvr_round_probe", "vs", "vs_5_0");
    ID3DBlob* psp = compile(fn, kShaderProbe, sizeof(kShaderProbe) - 1, "rdrvr_round_probe", "ps", "ps_5_0");
    bool ok = vs && ps && vs2 && ps2 && vsp && psp;
    if (ok) {
        // the root signature (all three pipelines): the constants; t0-t2 (the depth, the atlas, the eye image) for the
        // eye; t3 the probe's texels; a linear sampler stopping at the atlas's last mip
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE range2{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0};
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, kConsts};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &range2};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC smp{};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = static_cast<float>(kMips - 1);
        smp.ShaderRegister = 0;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{3, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ID3DBlob *sig = nullptr, *err = nullptr;
        ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) &&
             SUCCEEDED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root)));
        if (!ok && err) log::error("[round] the root signature: %s", static_cast<const char*>(err->GetBufferPointer()));
        if (sig) sig->Release();
        if (err) err->Release();
    }
    if (ok) {
        const D3D12_INPUT_ELEMENT_DESC il[3] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_root;
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.InputLayout = {il, 3};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // the round's own depth sorts its sides
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = fmt;
        pd.SampleDesc.Count = 1;
        const pso::OwnCreation own_pso;  // (run 19: [Stereo] InstancedShadersAuto leaves the mod's own pipelines alone)
        ok = SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso)));
        // the new: blended by the shader's alpha (the silhouette's and the fingers' soft edge), the image's alpha kept
        pd.VS = {vs2->GetBufferPointer(), vs2->GetBufferSize()};
        pd.PS = {ps2->GetBufferPointer(), ps2->GetBufferSize()};
        D3D12_RENDER_TARGET_BLEND_DESC& bd = pd.BlendState.RenderTarget[0];
        bd.BlendEnable = TRUE;
        bd.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        bd.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        bd.BlendOp = D3D12_BLEND_OP_ADD;
        bd.SrcBlendAlpha = D3D12_BLEND_ZERO;
        bd.DestBlendAlpha = D3D12_BLEND_ONE;
        bd.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bd.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        ok = ok && SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso2)));
        bd.BlendEnable = FALSE;  // (the held magazine's count of all its pixels: nothing written to the image)
        bd.RenderTargetWriteMask = 0;
        ok = ok && SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso2c)));
        // the probe: one texel a draw, blended toward the new value by the blend factor (the smoothing), no depth
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pp{};
        pp.pRootSignature = g_root;
        pp.VS = {vsp->GetBufferPointer(), vsp->GetBufferSize()};
        pp.PS = {psp->GetBufferPointer(), psp->GetBufferSize()};
        D3D12_RENDER_TARGET_BLEND_DESC& pb = pp.BlendState.RenderTarget[0];
        pb.BlendEnable = TRUE;
        pb.SrcBlend = pb.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        pb.DestBlend = pb.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        pb.BlendOp = pb.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pp.SampleMask = UINT_MAX;
        pp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pp.RasterizerState.DepthClipEnable = TRUE;
        pp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pp.NumRenderTargets = 1;
        pp.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pp.SampleDesc.Count = 1;
        ok = ok && SUCCEEDED(dev->CreateGraphicsPipelineState(&pp, IID_PPV_ARGS(&g_pso_probe)));
    }
    for (ID3DBlob* b : {vs, ps, vs2, ps2, vsp, psp})
        if (b) b->Release();
    if (!ok) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * kPerSlot + 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srv_heap))) || FAILED(dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&g_dsv_heap))) ||
        FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtv_heap))))
        return false;
    g_srv_step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // the meshes and their indices, in upload buffers read in place (a few thousand vertices)
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto upload = [&](const void* src, size_t n, ID3D12Resource** out) {
        bd.Width = n ? n : 4;
        if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(out))))
            return false;
        void* p = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED((*out)->Map(0, &none, &p)) || !p) return false;
        std::memcpy(p, src, n);
        (*out)->Unmap(0, nullptr);
        return true;
    };
    if (!upload(g_mesh.data(), g_mesh.size() * sizeof(Vtx), &g_vb) || !upload(g_idx.data(), g_idx.size() * sizeof(uint16_t), &g_ib)) return false;
    // the round's own depth
    D3D12_HEAP_PROPERTIES dh{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC zd{};
    zd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    zd.Width = w;
    zd.Height = h;
    zd.DepthOrArraySize = zd.MipLevels = 1;
    zd.Format = DXGI_FORMAT_D32_FLOAT;
    zd.SampleDesc.Count = 1;
    zd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &zd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&g_zbuf))))
        return false;
    g_zbuf->SetName(L"RDRVR round depth");
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(g_zbuf, &dv, g_dsv_heap->GetCPUDescriptorHandleForHeapStart());
    // the atlas (2026-10-27): a DEFAULT texture, copied once from its upload in the first list that draws (record)
    D3D12_RESOURCE_DESC ad{};
    ad.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ad.Width = kAtlasW;
    ad.Height = static_cast<UINT>(g_atlas_h);
    ad.DepthOrArraySize = 1;
    ad.MipLevels = kMips;
    ad.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ad.SampleDesc.Count = 1;
    if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_atlas_tex))))
        return false;
    g_atlas_tex->SetName(L"RDRVR round atlas");
    g_atlas_state = 0;
    {
        UINT rows[kMips] = {};
        UINT64 row_bytes[kMips] = {}, total = 0;
        dev->GetCopyableFootprints(&ad, 0, kMips, 0, g_atlas_fp, rows, row_bytes, &total);
        bd.Width = total;
        if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_atlas_up))))
            return false;
        uint8_t* p = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(g_atlas_up->Map(0, &none, reinterpret_cast<void**>(&p))) || !p) return false;
        for (int m = 0; m < kMips; ++m)
            for (UINT y = 0; y < rows[m]; ++y)
                std::memcpy(p + g_atlas_fp[m].Offset + static_cast<UINT64>(y) * g_atlas_fp[m].Footprint.RowPitch,
                            g_atlas[m].data() + static_cast<size_t>(y) * (kAtlasW >> m) * 4, static_cast<size_t>(kAtlasW >> m) * 4);
        g_atlas_up->Unmap(0, nullptr);
    }
    // the probe's texels and their readback
    D3D12_RESOURCE_DESC pd2{};
    pd2.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    pd2.Width = kProbeTexels;
    pd2.Height = 1;
    pd2.DepthOrArraySize = pd2.MipLevels = 1;
    pd2.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd2.SampleDesc.Count = 1;
    pd2.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &pd2, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_probe_tex))))
        return false;
    g_probe_tex->SetName(L"RDRVR round light probe");
    dev->CreateRenderTargetView(g_probe_tex, nullptr, g_rtv_heap->GetCPUDescriptorHandleForHeapStart());
    // the views that never change: each slot's atlas view (t1 of both eyes' tables), the probe's (after the slots)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = kMips;
        for (int s = 0; s < kSlots; ++s)
            for (int e = 0; e < 2; ++e) {
                D3D12_CPU_DESCRIPTOR_HANDLE hc = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
                hc.ptr += static_cast<SIZE_T>(s * kPerSlot + e * 3 + 1) * g_srv_step;
                dev->CreateShaderResourceView(g_atlas_tex, &sv, hc);
            }
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE hc = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        hc.ptr += static_cast<SIZE_T>(kSlots * kPerSlot) * g_srv_step;
        dev->CreateShaderResourceView(g_probe_tex, &sv, hc);
        hc.ptr += g_srv_step;  // a null view: root parameter 2 while the probe texels are the render target
        dev->CreateShaderResourceView(nullptr, &sv, hc);
    }
    // the occlusion counts
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kSlots * kQ, 0};  // per slot: the round's two eyes, the row's, the magazine's
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_qheap)))) return false;
    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    bd.Width = kSlots * kQ * sizeof(uint64_t);
    if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_qread))))
        return false;
    bd.Width = kSlots * 512;
    if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_probe_rb))))
        return false;
    g_w = w;
    g_h = h;
    g_fmt = fmt;
    return true;
}

// The game's copy into "Depth Resolve", in its own list: this frame's next eye's scene depth into our buffer.
void copy_tap(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    const uint64_t frame = state::presents.load(std::memory_order_relaxed);
    const void* cur = g_resolve.load(std::memory_order_relaxed);
    // (2026-10-28: the magazines, PistolMagazines', whatever RoundInHand says)
    auto wanted = [] { return (g_on.load(std::memory_order_relaxed) && held_now()) || row_now() || show_now() || mags_now(); };
    if (dst != cur) {
        // (the cheap tests first: the game records many copies; the review's) the one in use copied into lately: none other
        if (cur && frame - g_resolve_frame.load(std::memory_order_relaxed) <= 3) return;
        if (!wanted()) return;
        if (!d3d::resource_named(dst, "Depth Resolve")) return;
        // another Depth Resolve: taken once the one in use has had no copy for a few frames (a resize, DLSS on or off)
        g_resolve_other.fetch_add(cur ? 1 : 0, std::memory_order_relaxed);
        g_resolve.store(dst, std::memory_order_relaxed);
        const D3D12_RESOURCE_DESC dd = dst->GetDesc();
        log::info("[round] the scene depth taken from \"Depth Resolve\" %p (%llux%u)%s", static_cast<const void*>(dst),
                  static_cast<unsigned long long>(dd.Width), dd.Height, cur ? " (another than before)" : "");
    } else if (!wanted()) {
        return;
    }
    g_resolve_frame.store(frame, std::memory_order_relaxed);
    int k, first;
    {
        std::lock_guard lock(g_depth_mutex);
        if (frame != g_tap_frame) {
            g_tap_frame = frame;
            g_tap_k = 0;
            g_tap_frames.fetch_add(1, std::memory_order_relaxed);
        }
        k = g_tap_k++;
    }
    if (k + 1 > g_tap_max.load(std::memory_order_relaxed)) g_tap_max.store(k + 1, std::memory_order_relaxed);
    g_tap_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (k >= 2) {
        g_tap_extra.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard lock(g_cam_mutex);
        first = g_first_eye[frame % kKeep];
    }
    const int eye = k == 0 ? first : 1 - first, par = static_cast<int>(frame % kKeep);
    D3D12_RESOURCE_DESC d = src->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 || d.DepthOrArraySize != 1) return;  // the scene depth only
    ID3D12Device* dev = nullptr;
    if (FAILED(src->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);  // subresource 0: the depth plane
    dev->Release();
    if (fp.Footprint.Format != DXGI_FORMAT_R24G8_TYPELESS && fp.Footprint.Format != DXGI_FORMAT_R32_TYPELESS &&
        fp.Footprint.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS && fp.Footprint.Format != DXGI_FORMAT_R32_FLOAT) {
        g_tap_extra.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t stride = (total + 511) & ~511ull;
    ID3D12Resource* buf = g_dbuf.load(std::memory_order_acquire);
    if (!buf || g_dbuf_size.load(std::memory_order_acquire) < stride * kKeep * 2) {
        g_need.store(stride * kKeep * 2, std::memory_order_relaxed);  // made on the presenting thread; copied from then on
        return;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = buf;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = fp;
    dl.PlacedFootprint.Offset = stride * static_cast<uint64_t>(par * 2 + eye);
    sl.pResource = src;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    g_taps.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_depth_mutex);
    g_dmeta.w = static_cast<uint32_t>(d.Width);
    g_dmeta.h = d.Height;
    g_dmeta.pitch = fp.Footprint.RowPitch;
    g_dmeta.fmt = static_cast<uint32_t>(d.Format);  // the resource's: its plane's footprint says R32_TYPELESS for D24S8 too
    g_dmeta.stride = stride;
    g_dframe[par][eye] = frame;
}

void ensure_buffer(ID3D12Device* dev, uint64_t frame) {
    for (size_t i = 0; i < g_grave.size();) {  // a replaced buffer, released once no list can still read it
        if (frame > g_grave[i].at + 8) {
            g_grave[i].r->Release();
            g_grave[i] = g_grave.back();
            g_grave.pop_back();
        } else {
            ++i;
        }
    }
    const uint64_t need = g_need.load(std::memory_order_relaxed);
    if (!need || need <= g_dbuf_size.load(std::memory_order_relaxed)) return;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = need;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* b = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b)))) {
        log::error("[round] the depth buffer (%llu bytes) not created", static_cast<unsigned long long>(need));
        g_need = 0;
        return;
    }
    b->SetName(L"RDRVR round scene depth");
    ID3D12Resource* old = g_dbuf.exchange(b, std::memory_order_acq_rel);
    g_dbuf_size.store(need, std::memory_order_release);
    if (old) g_grave.push_back({old, frame});
    log::info("[round] scene depth copies: a buffer of %llu bytes", static_cast<unsigned long long>(need));
}

void normalize3(float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f)
        for (int k = 0; k < 3; ++k) v[k] /= l;
}

// The model drawn for a kind (the new) or a weapon's old family (RoundModels 0: 1 the handgun, 2 the rifle, 3 the shell)
int old_family(int kind) { return kind == kKRifle || kind == kKBig ? 2 : kind == kKShell || kind == kKPaper ? 3 : 1; }
const Model& model_for(int kind, int family, bool newm) {
    if (newm) return g_kind[kind >= 0 && kind < kKinds ? kind : kK4440];
    return g_old[family == 2 ? 1 : family == 3 ? 2 : 0];
}
// the handful's spacing (side by side, touching) and the row's (a little apart): the new rounds' own widths; the old's
// as they were
float held_gap(const Model& m, int family, bool newm) {
    if (newm) return 2.0f * m.rad + 0.0004f;
    return family == 3 ? 0.024f : family == 2 ? 0.014f : 0.013f;
}
float row_gap(const Model& m, int family, bool newm) {
    if (newm) return 2.0f * m.rad + 0.003f;
    return family == 3 ? 0.026f : family == 2 ? 0.017f : 0.016f;
}

// [Reload] RoundsPerGrab: the rounds held side by side, a round's width apart (as the ammo row's), across the pinch:
// square to the round (Y) and to the thumb-to-index line (T x Y: on the fingers' side of the pinch; the other side
// sank them into the thumb, cycle r133's pictures), held while that line is short (a closed pinch: its direction would
// jitter), else along Z. dir: the caller's last good direction (kept between calls)
void handful_dir(const body::BodyPoints& bp, int hand, const float* pz, int n, float* dir, float* S) {
    const float *Y = pz + 3, *Z = pz + 6;
    for (int i = 0; i < 3; ++i) S[i] = Z[i];
    if (n > 1 && bp.tips_ok[hand]) {
        const float T[3] = {bp.index_tip[hand][0] - bp.thumb_tip[hand][0], bp.index_tip[hand][1] - bp.thumb_tip[hand][1],
                            bp.index_tip[hand][2] - bp.thumb_tip[hand][2]};
        float C[3] = {T[1] * Y[2] - T[2] * Y[1], T[2] * Y[0] - T[0] * Y[2], T[0] * Y[1] - T[1] * Y[0]};
        const float cl = std::sqrt(C[0] * C[0] + C[1] * C[1] + C[2] * C[2]);
        if (cl > 0.015f) {  // the tips 1.5 cm apart (square to the round) or more
            for (int i = 0; i < 3; ++i) dir[i] = C[i] / cl;
        }
        if (dir[0] != 0.0f || dir[1] != 0.0f || dir[2] != 0.0f) {  // the last good one, made square to this frame's Y
            const float sy = dir[0] * Y[0] + dir[1] * Y[1] + dir[2] * Y[2];
            for (int i = 0; i < 3; ++i) S[i] = dir[i] - sy * Y[i];
            normalize3(S);
        }
    }
    const float sign = g_fan_sign.load(std::memory_order_relaxed);  // (a test aid: which side of the pinch)
    for (int i = 0; i < 3; ++i) S[i] *= sign;
}

void write_grab();
}  // namespace

int kind_of(int w) {
    switch (w) {
        case 0: return kKVolcanic;
        case 1: return kK38Acp;
        case 2: return kK45Acp;
        case 3: return kKMauser;
        case 4: case 5: case 6: case 7:  // the revolvers
        case 8: case 9: return kK4440;   // the Carbine (the doc's .44-40), the Winchester (its own, the 1873's)
        case 10: return kKHenry;
        case 11: return kKEvans;
        case 12: case 14: case 19: return kKBig;  // the Springfield (a trapdoor: .45-70), the Buffalo, the Rolling Block
        case 13: case 20: case 31: return kKRifle;  // the Bolt Action, the Carcano, the Explosive rifle
        case 15: case 16: case 17: case 18: return kKShell;
        case 34: return kKPaper;
        default: return -1;
    }
}
const char* kind_name(int kind) { return kind >= 0 && kind < kKinds ? kDefs[kind].name : "-"; }

bool pose(const body::BodyPoints& bp, int hand, float* out) {
    if (hand < 0 || hand > 1 || !bp.hand_ok[hand]) return false;
    // the round in the world: the hand's IK target (where the drawn wrist goes, its axes) and the offset in them
    float off[3], ax[3];
    {
        std::lock_guard lock(g_cfg_mutex);
        const bool r = hand == 1 && g_r_round_own;  // (R13: the right hand's own place)
        std::memcpy(off, r ? g_r_offset : g_offset, sizeof(off));
        std::memcpy(ax, r ? g_r_axis : g_axis, sizeof(ax));
    }
    // columns: the target's axes in the world (the drawn wrist's when the target's are not filled)
    auto det3 = [](const float* m) {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    };
    const bool tgt_ok = std::fabs(det3(bp.target_rot[hand]) - 1.0f) < 0.1f;
    const float* R = tgt_ok ? bp.target_rot[hand] : bp.wrist_drawn[hand];
    g_axes_from.store(tgt_ok ? 1 : std::fabs(det3(bp.wrist_drawn[hand]) - 1.0f) < 0.1f ? 2 : 0, std::memory_order_relaxed);
    if (!g_axes_from.load(std::memory_order_relaxed)) return false;
    auto to_world = [&](const float* v, float* o) {
        for (int k = 0; k < 3; ++k) o[k] = R[k * 3] * v[0] + R[k * 3 + 1] * v[1] + R[k * 3 + 2] * v[2];
    };
    float *X = out, *Y = out + 3, *Z = out + 6, *P = out + 9;
    bool axis_auto;
    {
        std::lock_guard lock(g_cfg_mutex);
        axis_auto = hand == 1 && g_r_round_own ? g_r_axis_auto : g_axis_auto;  // (R13, the review)
    }
    to_world(off, P);
    // (cycle r170, 2026-10-27) the hand flat: with a long gun in the other hand John's free hand is drawn flat, the
    // fingers straight and together, the thumb tucked, and makes no pinch: the round placed at the thumb-to-index
    // midpoint was wholly inside the glove (the Henry 0 pixels). Cycle r171: body.cpp now puts the pinch learned with a
    // sidearm on that hand (BodyPoints::round_pinch), and the round goes in it by the pinch path below. The flat path is
    // the fallback only: body says the hand was left flat (round_flat: no pinch learned yet), or the tips are over
    // kFlatGap apart (r171: a 3 cm limit, with "the pinch behind the index tip", took the revolver's own pinch for flat
    // too, and lifted the round off the fingers). Then the round lies along the index finger (Y from the wrist to its
    // tip), its base just past the fingertip, so the hand cannot hide it, a little toward the eye (toward the thumb when
    // the camera is not known)
    bool flat = false;
    float F[3] = {0.0f, 0.0f, 0.0f};  // the index finger's direction: wrist to tip
    if (bp.tips_ok[hand]) {
        for (int k = 0; k < 3; ++k) F[k] = bp.index_tip[hand][k] - bp.hand[hand][k];
        normalize3(F);
        float gap2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float t = bp.index_tip[hand][k] - bp.thumb_tip[hand][k];
            gap2 += t * t;
        }
        // (cycle r172: the tip-gap rule took the game's own sidearm pinch for flat, its tips over 5 cm apart, and the
        // sidearms' rounds went to the fallback, nearly unseen: only body's word now; gap2 kept for the status)
        flat = !bp.round_pinch[hand] && bp.round_flat[hand];
        (void)gap2;
    }
    const int kind = kind_of(g_weapon.load(std::memory_order_relaxed));
    const bool newm = g_models.load(std::memory_order_relaxed);
    const Model& md = model_for(kind, audio::click_family(g_weapon.load(std::memory_order_relaxed)), newm);
    if (bp.tips_ok[hand] && flat) {
        for (int k = 0; k < 3; ++k) Y[k] = F[k];
        const float vis = md.len + 0.002f;  // (the whole round past the fingertip, its base 2 mm out)
        float side[3] = {0.0f, 0.0f, 0.0f};  // off the finger's line, square to it: toward the eye, else the thumb
        if (bp.cam_ok)
            for (int k = 0; k < 3; ++k) side[k] = bp.cam[12 + k] - bp.index_tip[hand][k];
        else
            for (int k = 0; k < 3; ++k) side[k] = bp.thumb_tip[hand][k] - bp.index_tip[hand][k];
        const float sy = side[0] * Y[0] + side[1] * Y[1] + side[2] * Y[2];
        for (int k = 0; k < 3; ++k) side[k] -= sy * Y[k];
        normalize3(side);
        const float lift = 0.004f;  // (past the tip nothing of the hand is in the way: only clear of the finger's own line)
        for (int k = 0; k < 3; ++k) P[k] += bp.index_tip[hand][k] + Y[k] * (vis - md.len) + side[k] * lift;
        g_out_mm.store(vis * 1000.0f, std::memory_order_relaxed);
        g_path.store(2, std::memory_order_relaxed);
        g_path_flat.fetch_add(1, std::memory_order_relaxed);
        g_axes_from.store(g_axes_from.load(std::memory_order_relaxed) + 20, std::memory_order_relaxed);
    } else if (bp.tips_ok[hand]) {  // pinched between the drawn thumb and index tips, its base just inside, pointing out
        float pinch[3];
        for (int k = 0; k < 3; ++k) pinch[k] = 0.5f * (bp.thumb_tip[hand][k] + bp.index_tip[hand][k]);
        if (axis_auto) {
            for (int k = 0; k < 3; ++k) Y[k] = pinch[k] - bp.hand[hand][k];
        } else {
            to_world(ax, Y);
        }
        normalize3(Y);
        // (cycle r169, 2026-10-27) the new models: moved out along the round so its tip passes the index tip by at least
        // 60% of its length (25 mm at most), its base held no further out than 3 mm past that tip; the .44-40 in the
        // revolver's pinch as before
        float out_m = 0.0f;
        if (newm && kind >= 0) {
            const float d_idx = (bp.index_tip[hand][0] - pinch[0]) * Y[0] + (bp.index_tip[hand][1] - pinch[1]) * Y[1] +
                                (bp.index_tip[hand][2] - pinch[2]) * Y[2];
            const float vis = std::fmin(0.025f, 0.6f * md.len);
            out_m = std::clamp(d_idx + vis - (md.len - 0.006f), 0.0f, std::fmax(0.0f, d_idx + 0.009f));
        }
        if (!std::isfinite(out_m)) out_m = 0.0f;  // (the review's guard)
        g_out_mm.store(out_m * 1000.0f, std::memory_order_relaxed);
        for (int k = 0; k < 3; ++k) P[k] += pinch[k] + (out_m - 0.006f) * Y[k];
        g_path.store(1, std::memory_order_relaxed);
        g_path_pinch.fetch_add(1, std::memory_order_relaxed);
        g_axes_from.store(g_axes_from.load(std::memory_order_relaxed) + 10, std::memory_order_relaxed);
    } else {
        for (int k = 0; k < 3; ++k) P[k] += bp.hand[hand][k];
        to_world(ax, Y);
        normalize3(Y);
        g_path.store(0, std::memory_order_relaxed);
    }
    // (the review's guard) a pose that is not finite: at the hand, along the configured axis (else up)
    if (!(std::isfinite(P[0]) && std::isfinite(P[1]) && std::isfinite(P[2]) && std::isfinite(Y[0]) && std::isfinite(Y[1]) && std::isfinite(Y[2]))) {
        to_world(off, P);
        for (int k = 0; k < 3; ++k) P[k] += bp.hand[hand][k];
        to_world(ax, Y);
        normalize3(Y);
        if (!(std::isfinite(Y[0]) && std::isfinite(Y[1]) && std::isfinite(Y[2])) || Y[0] * Y[0] + Y[1] * Y[1] + Y[2] * Y[2] < 0.5f)
            Y[0] = 0.0f, Y[1] = 1.0f, Y[2] = 0.0f;
        if (!(std::isfinite(P[0]) && std::isfinite(P[1]) && std::isfinite(P[2]))) return false;
        g_path_bad.fetch_add(1, std::memory_order_relaxed);
    }
    {  // any axis across it
        const float t[3] = {std::fabs(Y[1]) < 0.9f ? 0.0f : 1.0f, std::fabs(Y[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
        X[0] = t[1] * Y[2] - t[2] * Y[1];
        X[1] = t[2] * Y[0] - t[0] * Y[2];
        X[2] = t[0] * Y[1] - t[1] * Y[0];
        normalize3(X);
        Z[0] = X[1] * Y[2] - X[2] * Y[1];
        Z[1] = X[2] * Y[0] - X[0] * Y[2];
        Z[2] = X[0] * Y[1] - X[1] * Y[0];
    }
    return true;
}

namespace {
// the row's poses: standing (base to tip up), side by side along the body's right, centred on the zone (rz: the
// right-handed third axis, X x Y: the body's back)
void row_pose(const Row& row, int r, int nr, float gap, float* out) {
    const float side = (static_cast<float>(r) - 0.5f * static_cast<float>(nr - 1)) * gap;
    float rz[3];
    rz[0] = row.rgt[1] * row.up[2] - row.rgt[2] * row.up[1];
    rz[1] = row.rgt[2] * row.up[0] - row.rgt[0] * row.up[2];
    rz[2] = row.rgt[0] * row.up[1] - row.rgt[1] * row.up[0];
    for (int k = 0; k < 3; ++k) {
        out[k] = row.rgt[k];
        out[3 + k] = row.up[k];
        out[6 + k] = rz[k];
        out[9 + k] = row.at[k] + row.rgt[k] * side;
    }
}

bool write_bmp(const std::string& path, int w, int h, const uint8_t* rgba, int pitch, bool bgr, std::string* res) {
    const int row = (w * 3 + 3) & ~3;
    std::vector<uint8_t> out(54 + static_cast<size_t>(row) * h, 0);
    uint8_t* hdr = out.data();
    auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(2, static_cast<uint32_t>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    hdr[26] = 1;
    hdr[28] = 24;
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba + static_cast<size_t>(y) * pitch;
        uint8_t* dst = out.data() + 54 + static_cast<size_t>(h - 1 - y) * row;
        for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = bgr ? src[x * 4 + 0] : src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = bgr ? src[x * 4 + 2] : src[x * 4 + 0];
        }
    }
    std::wstring wpath(path.begin(), path.end());
    FILE* f = nullptr;
    _wfopen_s(&f, wpath.c_str(), L"wb");
    if (!f) {
        *res = "ERROR cannot open " + path;
        return false;
    }
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!ok) *res = "ERROR writing " + path;
    return ok;
}

void write_grab() {  // presenting thread: the copied eye, cropped around the round, as a 24-bit BMP
    uint8_t* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(g_grab_fp.Footprint.RowPitch) * g_grab_fp.Footprint.Height};
    std::string res;
    if (FAILED(g_grab_rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) {
        res = "ERROR the capture's Map failed";
    } else {
        const int W = static_cast<int>(g_grab_fp.Footprint.Width), H = static_cast<int>(g_grab_fp.Footprint.Height);
        const int cw = g_grab_full || W < 640 ? W : 640, ch = g_grab_full || H < 640 ? H : 640;  // "full": the whole image
        int x0 = g_grab_cx - cw / 2, y0 = g_grab_cy - ch / 2;
        x0 = x0 < 0 ? 0 : x0 + cw > W ? W - cw : x0;
        y0 = y0 < 0 ? 0 : y0 + ch > H ? H - ch : y0;
        const bool bgr = g_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || g_fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
        const uint8_t* src = p + g_grab_fp.Offset + static_cast<uint64_t>(y0) * g_grab_fp.Footprint.RowPitch + static_cast<uint64_t>(x0) * 4;
        const bool ok = write_bmp(g_grab_path, cw, ch, src, static_cast<int>(g_grab_fp.Footprint.RowPitch), bgr, &res);
        g_grab_rb->Unmap(0, nullptr);
        if (ok) {
            char buf[400];
            std::snprintf(buf, sizeof(buf), "wrote %s: eye %d, %dx%d at (%d, %d) of %dx%d, the round at (%d, %d)", g_grab_path.c_str(), g_grab_eye, cw, ch, x0,
                          y0, W, H, g_grab_cx, g_grab_cy);
            res = buf;
        }
    }
    {
        std::lock_guard lock(g_grab_mutex);
        g_grab_result = res;
    }
    g_grab_phase.store(3, std::memory_order_release);
    SetEvent(g_grab_done);
}

void grab_copy(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* const dst[2], int slot, int cx, int cy) {
    if (g_grab_phase.load(std::memory_order_acquire) == 1 && dst[g_grab_eye]) {  // the capture: the image as drawn, read back
        ID3D12Resource* img = dst[g_grab_eye];
        D3D12_RESOURCE_DESC d = img->GetDesc();
        UINT rows = 0;
        UINT64 rb = 0, total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &g_grab_fp, &rows, &rb, &total);
        if (g_grab_rb && g_grab_rb->GetDesc().Width < total) {
            g_grab_rb->Release();
            g_grab_rb = nullptr;
        }
        if (!g_grab_rb) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_grab_rb));
        }
        if (g_grab_rb) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = img;
            b.Transition.Subresource = 0;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
            dl.pResource = g_grab_rb;
            dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dl.PlacedFootprint = g_grab_fp;
            sl.pResource = img;
            sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sl.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            cl->ResourceBarrier(1, &b);
            g_grab_cx = cx;
            g_grab_cy = cy;
            g_grab_slot = slot;
            g_grab_phase.store(2, std::memory_order_release);
        }
    }
}

void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

}  // namespace

void init() {
    lathe(kHandgun, static_cast<int>(sizeof(kHandgun) / sizeof(kHandgun[0])), &g_old[0]);
    lathe(kRifle, static_cast<int>(sizeof(kRifle) / sizeof(kRifle[0])), &g_old[1]);
    lathe(kShell, static_cast<int>(sizeof(kShell) / sizeof(kShell[0])), &g_old[2]);
    // the new models (2026-10-27): the profiles' bands, the atlas packed and painted, then each mesh with its UVs in it
    for (int k = 0; k < kKinds; ++k) kind_bands(k);
    pack_atlas();
    make_atlas();
    for (int k = 0; k < kKinds; ++k) lathe2(k, &g_kind[k]);
    build_mags();  // (2026-10-28) the magazines and the clip, in the same buffers
    g_grab_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        const int m = config::get_int("Reload", "RoundInHand", 0);
        g_mode = m < 0 ? 0 : m > 2 ? 2 : m;
        g_on = g_mode.load() > 0;
    }
    g_models = config::get_bool("Reload", "RoundModels", true);
    {
        std::lock_guard lock(g_cfg_mutex);
        sscanf_s(config::get_string("Reload", "RoundOffset", "0 0 0").c_str(), "%f %f %f", &g_offset[0], &g_offset[1], &g_offset[2]);
        sscanf_s(config::get_string("Reload", "MagazineOffset", "0 0 0").c_str(), "%f %f %f", &g_mag_off[0], &g_mag_off[1], &g_mag_off[2]);
        for (float& v : g_mag_off) v = std::isfinite(v) ? std::fmin(0.12f, std::fmax(-0.12f, v)) : 0.0f;
        g_mag_turn = std::fmin(180.0f, std::fmax(-180.0f, config::get_float("Reload", "MagazineTurn", 0.0f)));
        g_mag_tilt = std::fmin(90.0f, std::fmax(-90.0f, config::get_float("Reload", "MagazineTilt", 0.0f)));
        if (!std::isfinite(g_mag_turn)) g_mag_turn = 0.0f;
        if (!std::isfinite(g_mag_tilt)) g_mag_tilt = 0.0f;
        const std::string ax = config::get_string("Reload", "RoundAxis", "1 0 0");  // along the fingers (the wrist's x)
        g_axis_auto = ax == "auto" || sscanf_s(ax.c_str(), "%f %f %f", &g_axis[0], &g_axis[1], &g_axis[2]) != 3;
        // (2026-10-28, the review) a zero (or not finite) axis collapses the model: taken as auto, the axis as default
        const float al = std::sqrt(g_axis[0] * g_axis[0] + g_axis[1] * g_axis[1] + g_axis[2] * g_axis[2]);
        if (!(al > 1e-3f) || !std::isfinite(al)) {
            g_axis_auto = true;
            g_axis[0] = 1.0f, g_axis[1] = 0.0f, g_axis[2] = 0.0f;
        }
        normalize3(g_axis);
        g_bright = config::get_float("Reload", "RoundBrightness", 1.0f);
        {  // (R13) the right hand's own, if set
            const std::string ro = config::get_string("Reload", "RoundOffsetRight", ""), ra = config::get_string("Reload", "RoundAxisRight", "");
            g_r_round_own = !ro.empty() && sscanf_s(ro.c_str(), "%f %f %f", &g_r_offset[0], &g_r_offset[1], &g_r_offset[2]) == 3;
            if (g_r_round_own) {
                for (float& v : g_r_offset) v = std::isfinite(v) ? std::fmin(0.2f, std::fmax(-0.2f, v)) : 0.0f;
                if (ra.empty()) {  // (the review) no RoundAxisRight: the left's axis
                    std::memcpy(g_r_axis, g_axis, sizeof(g_r_axis));
                    g_r_axis_auto = g_axis_auto;
                } else {
                    g_r_axis_auto = ra == "auto" || sscanf_s(ra.c_str(), "%f %f %f", &g_r_axis[0], &g_r_axis[1], &g_r_axis[2]) != 3;
                }
                const float rl = std::sqrt(g_r_axis[0] * g_r_axis[0] + g_r_axis[1] * g_r_axis[1] + g_r_axis[2] * g_r_axis[2]);
                if (!(rl > 1e-3f) || !std::isfinite(rl)) g_r_axis[0] = 1.0f, g_r_axis[1] = 0.0f, g_r_axis[2] = 0.0f;
                normalize3(g_r_axis);
            }
            const std::string mo = config::get_string("Reload", "MagazineOffsetRight", "");
            g_r_mag_own = !mo.empty() && sscanf_s(mo.c_str(), "%f %f %f", &g_r_mag_off[0], &g_r_mag_off[1], &g_r_mag_off[2]) == 3;
            if (g_r_mag_own) {
                for (float& v : g_r_mag_off) v = std::isfinite(v) ? std::fmin(0.12f, std::fmax(-0.12f, v)) : 0.0f;
                g_r_mag_turn = std::fmin(180.0f, std::fmax(-180.0f, config::get_float("Reload", "MagazineTurnRight", 0.0f)));
                g_r_mag_tilt = std::fmin(90.0f, std::fmax(-90.0f, config::get_float("Reload", "MagazineTiltRight", 0.0f)));
                if (!std::isfinite(g_r_mag_turn)) g_r_mag_turn = 0.0f;
                if (!std::isfinite(g_r_mag_tilt)) g_r_mag_tilt = 0.0f;
            }
        }
    }
    {  // (C38) each magazine pistol's own, each hand's
        std::lock_guard lock(g_cfg_mutex);
        for (int w = 1; w <= 3; ++w)
            for (int hs = 0; hs < 2; ++hs) {
                const std::string sec = std::string("Weapon.") + holster::weapon_token(w), sfx = hs == 0 ? "Left" : "";
                const std::string mo = config::get_string(sec.c_str(), ("HeldMagazineOffset" + sfx).c_str(), "");
                WMag& m = g_wmag[w][hs];
                m.own = !mo.empty() && sscanf_s(mo.c_str(), "%f %f %f", &m.off[0], &m.off[1], &m.off[2]) == 3;
                if (!m.own) continue;
                for (float& v : m.off) v = std::isfinite(v) ? std::fmin(0.12f, std::fmax(-0.12f, v)) : 0.0f;
                m.turn = std::fmin(180.0f, std::fmax(-180.0f, config::get_float(sec.c_str(), ("HeldMagazineTurn" + sfx).c_str(), 0.0f)));
                m.tilt = std::fmin(90.0f, std::fmax(-90.0f, config::get_float(sec.c_str(), ("HeldMagazineTilt" + sfx).c_str(), 0.0f)));
                if (!std::isfinite(m.turn)) m.turn = 0.0f;
                if (!std::isfinite(m.tilt)) m.tilt = 0.0f;
            }
    }
    g_row_on = config::get_bool("Reload", "ShowAmmo", false);
    d3d::set_copy_tap(copy_tap);
    std::string tris;
    for (int k = 0; k < kKinds; ++k) tris += std::string(k ? ", " : "") + kDefs[k].name + " " + std::to_string(g_kind[k].count / 3);
    log::info("[round] the round in hand %s, models %s (the old: %u, %u, %u vertices; the new, triangles: %s; the atlas %dx%d)",
              g_on.load() ? "on" : "off", g_models.load() ? "new" : "old", g_old[0].count, g_old[1].count, g_old[2].count, tris.c_str(), kAtlasW,
              g_atlas_h);
}

bool enabled() { return g_on.load(std::memory_order_relaxed); }
bool models() { return g_models.load(std::memory_order_relaxed); }
void set_models(bool new_models, bool save) {
    if (g_models.exchange(new_models) != new_models) log::info("[round] the round models: %s", new_models ? "new" : "old");
    if (save) config::set("Reload", "RoundModels", new_models ? "1" : "0");
}
bool show_ammo() { return g_row_on.load(std::memory_order_relaxed); }
void set_show_ammo(bool on, bool save) {
    if (g_row_on.exchange(on) != on) log::info("[round] the rounds shown at the chest: %s", on ? "on" : "off");
    if (save) config::set("Reload", "ShowAmmo", on ? "1" : "0");
}
void set_mag_row(int weapon, int n, int rounds, const float at[3], const float rgt[3], const float up[3], const float fwd[3]) {
    std::lock_guard lock(g_mag_mutex);
    g_mrow.weapon = weapon;
    g_mrow.n = weapon >= 0 ? n : 0;
    g_mrow.rounds = rounds;
    if (at && rgt && up && fwd) {
        std::memcpy(g_mrow.at, at, sizeof(g_mrow.at));
        std::memcpy(g_mrow.rgt, rgt, sizeof(g_mrow.rgt));
        std::memcpy(g_mrow.up, up, sizeof(g_mrow.up));
        std::memcpy(g_mrow.fwd, fwd, sizeof(g_mrow.fwd));
    }
    g_mrow.ms = log::now_ms();
}
void set_row(int weapon, int n, const float at[3], const float rgt[3], const float up[3], const float fwd[3]) {
    std::lock_guard lock(g_row_mutex);
    g_row.weapon = weapon;
    g_row.n = n;
    if (audio::click_family(weapon) > 0 && n > 0) {
        std::memcpy(g_row.at, at, sizeof(g_row.at));
        std::memcpy(g_row.rgt, rgt, sizeof(g_row.rgt));
        std::memcpy(g_row.up, up, sizeof(g_row.up));
        std::memcpy(g_row.fwd, fwd, sizeof(g_row.fwd));
    }
    g_row.ms = log::now_ms();
}
int mode() { return g_mode.load(std::memory_order_relaxed); }
Place place(int hand) {
    std::lock_guard lock(g_cfg_mutex);
    Place p;
    const bool r = hand == 1 && g_r_round_own;  // (R13: the right hand's own, else the left's it uses)
    const float* ax = r ? g_r_axis : g_axis;
    std::memcpy(p.offset, r ? g_r_offset : g_offset, sizeof(p.offset));
    std::memcpy(p.axis, ax, sizeof(p.axis));
    const bool fingers = std::fabs(ax[0] - 1.0f) < 1e-3f && std::fabs(ax[1]) < 1e-3f && std::fabs(ax[2]) < 1e-3f;
    p.axis_mode = (r ? g_r_axis_auto : g_axis_auto) ? 1 : fingers ? 0 : 2;
    p.bright = g_bright;
    return p;
}
void set_place(const Place& p, bool save, int hand) {
    if (hand == 1) {  // (R13) the right hand's own from now on
        char b[64];
        std::string axis;
        {
            std::lock_guard lock(g_cfg_mutex);
            g_r_round_own = true;
            for (int k = 0; k < 3; ++k) g_r_offset[k] = p.offset[k] < -0.2f ? -0.2f : p.offset[k] > 0.2f ? 0.2f : p.offset[k];
            g_r_axis_auto = p.axis_mode == 1;
            if (p.axis_mode == 0) {
                g_r_axis[0] = 1.0f, g_r_axis[1] = 0.0f, g_r_axis[2] = 0.0f;
            } else if (p.axis_mode == 2) {
                const float l = std::sqrt(p.axis[0] * p.axis[0] + p.axis[1] * p.axis[1] + p.axis[2] * p.axis[2]);
                if (l > 1e-3f)
                    for (int k = 0; k < 3; ++k) g_r_axis[k] = p.axis[k] / l;
            }
            g_bright = p.bright < 0.1f ? 0.1f : p.bright > 5.0f ? 5.0f : p.bright;
            if (g_r_axis_auto) {
                axis = "auto";
            } else {
                std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", g_r_axis[0], g_r_axis[1], g_r_axis[2]);
                axis = b;
            }
        }
        if (!save) return;
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", p.offset[0], p.offset[1], p.offset[2]);
        config::set("Reload", "RoundOffsetRight", b);
        config::set("Reload", "RoundAxisRight", axis);
        std::snprintf(b, sizeof(b), "%.2f", p.bright);
        config::set("Reload", "RoundBrightness", b);
        log::info("[round] its place in the right hand: offset %.3f %.3f %.3f, axis %s", p.offset[0], p.offset[1], p.offset[2], axis.c_str());
        return;
    }
    char b[64];
    std::string axis;
    {
        std::lock_guard lock(g_cfg_mutex);
        for (int k = 0; k < 3; ++k) g_offset[k] = p.offset[k] < -0.2f ? -0.2f : p.offset[k] > 0.2f ? 0.2f : p.offset[k];
        g_axis_auto = p.axis_mode == 1;
        if (p.axis_mode == 0) {
            g_axis[0] = 1.0f, g_axis[1] = 0.0f, g_axis[2] = 0.0f;
        } else if (p.axis_mode == 2) {
            const float l = std::sqrt(p.axis[0] * p.axis[0] + p.axis[1] * p.axis[1] + p.axis[2] * p.axis[2]);
            if (l > 1e-3f)
                for (int k = 0; k < 3; ++k) g_axis[k] = p.axis[k] / l;
        }
        g_bright = p.bright < 0.1f ? 0.1f : p.bright > 5.0f ? 5.0f : p.bright;
        if (g_axis_auto) {
            axis = "auto";
        } else {
            std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", g_axis[0], g_axis[1], g_axis[2]);
            axis = b;
        }
    }
    if (!save) return;
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", p.offset[0], p.offset[1], p.offset[2]);
    config::set("Reload", "RoundOffset", b);
    config::set("Reload", "RoundAxis", axis);
    std::snprintf(b, sizeof(b), "%.2f", p.bright);
    config::set("Reload", "RoundBrightness", b);
    log::info("[round] its place: offset %.3f %.3f %.3f, axis %s, brightness %.2f", p.offset[0], p.offset[1], p.offset[2], axis.c_str(), p.bright);
}

MagPlace mag_place(int hand, int weapon, bool* own) {
    std::lock_guard lock(g_cfg_mutex);
    MagPlace p;
    const int hs = hand == 1 ? 1 : 0;
    if (own) *own = false;
    if (wmag_ok(weapon) && g_wmag[weapon][hs].own) {  // (C38) the pistol's own
        const WMag& m = g_wmag[weapon][hs];
        std::memcpy(p.offset, m.off, sizeof(p.offset));
        p.turn = m.turn;
        p.tilt = m.tilt;
        if (own) *own = true;
        return p;
    }
    const bool r = hand == 1 && g_r_mag_own;  // (R13)
    std::memcpy(p.offset, r ? g_r_mag_off : g_mag_off, sizeof(p.offset));
    p.turn = r ? g_r_mag_turn : g_mag_turn;
    p.tilt = r ? g_r_mag_tilt : g_mag_tilt;
    return p;
}
void clear_mag_place(int hand, int weapon) {
    if (!wmag_ok(weapon)) return;
    const int hs = hand == 1 ? 1 : 0;
    {
        std::lock_guard lock(g_cfg_mutex);
        g_wmag[weapon][hs].own = false;
    }
    const std::string sec = std::string("Weapon.") + holster::weapon_token(weapon), sfx = hs == 0 ? "Left" : "";
    for (const char* k : {"HeldMagazineOffset", "HeldMagazineTurn", "HeldMagazineTilt"}) config::set(sec.c_str(), (k + sfx).c_str(), "");
    log::info("[round] the %s's magazine in the %s hand: back to that hand's", holster::weapon_label(weapon), hs ? "right" : "left");
}
void set_mag_place(const MagPlace& p, bool save, int hand, int weapon) {
    if (wmag_ok(weapon)) {  // (C38) that pistol's own for the hand
        const int hs = hand == 1 ? 1 : 0;
        WMag q;
        {
            std::lock_guard lock(g_cfg_mutex);
            WMag& m = g_wmag[weapon][hs];
            m.own = true;
            for (int k = 0; k < 3; ++k) m.off[k] = std::isfinite(p.offset[k]) ? std::fmin(0.12f, std::fmax(-0.12f, p.offset[k])) : 0.0f;
            m.turn = std::isfinite(p.turn) ? std::fmin(180.0f, std::fmax(-180.0f, p.turn)) : 0.0f;
            m.tilt = std::isfinite(p.tilt) ? std::fmin(90.0f, std::fmax(-90.0f, p.tilt)) : 0.0f;
            q = m;
        }
        if (!save) return;
        const std::string sec = std::string("Weapon.") + holster::weapon_token(weapon), sfx = hs == 0 ? "Left" : "";
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", q.off[0], q.off[1], q.off[2]);
        config::set(sec.c_str(), ("HeldMagazineOffset" + sfx).c_str(), b);
        std::snprintf(b, sizeof(b), "%.0f", q.turn);
        config::set(sec.c_str(), ("HeldMagazineTurn" + sfx).c_str(), b);
        std::snprintf(b, sizeof(b), "%.0f", q.tilt);
        config::set(sec.c_str(), ("HeldMagazineTilt" + sfx).c_str(), b);
        log::info("[round] the %s's magazine in the %s hand: offset %.3f %.3f %.3f, turn %.0f, tilt %.0f", holster::weapon_label(weapon), hs ? "right" : "left",
                  q.off[0], q.off[1], q.off[2], q.turn, q.tilt);
        return;
    }
    if (hand == 1) {  // (R13) the right hand's own from now on
        MagPlace q;
        {
            std::lock_guard lock(g_cfg_mutex);
            g_r_mag_own = true;
            for (int k = 0; k < 3; ++k) g_r_mag_off[k] = std::isfinite(p.offset[k]) ? std::fmin(0.12f, std::fmax(-0.12f, p.offset[k])) : 0.0f;
            g_r_mag_turn = std::isfinite(p.turn) ? std::fmin(180.0f, std::fmax(-180.0f, p.turn)) : 0.0f;
            g_r_mag_tilt = std::isfinite(p.tilt) ? std::fmin(90.0f, std::fmax(-90.0f, p.tilt)) : 0.0f;
            std::memcpy(q.offset, g_r_mag_off, sizeof(q.offset));
            q.turn = g_r_mag_turn;
            q.tilt = g_r_mag_tilt;
        }
        if (!save) return;
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", q.offset[0], q.offset[1], q.offset[2]);
        config::set("Reload", "MagazineOffsetRight", b);
        std::snprintf(b, sizeof(b), "%.0f", q.turn);
        config::set("Reload", "MagazineTurnRight", b);
        std::snprintf(b, sizeof(b), "%.0f", q.tilt);
        config::set("Reload", "MagazineTiltRight", b);
        log::info("[round] the magazine's place in the right hand: offset %.3f %.3f %.3f, turn %.0f, tilt %.0f", q.offset[0], q.offset[1], q.offset[2],
                  q.turn, q.tilt);
        return;
    }
    MagPlace q;
    {
        std::lock_guard lock(g_cfg_mutex);
        for (int k = 0; k < 3; ++k) g_mag_off[k] = std::isfinite(p.offset[k]) ? std::fmin(0.12f, std::fmax(-0.12f, p.offset[k])) : 0.0f;
        g_mag_turn = std::isfinite(p.turn) ? std::fmin(180.0f, std::fmax(-180.0f, p.turn)) : 0.0f;
        g_mag_tilt = std::isfinite(p.tilt) ? std::fmin(90.0f, std::fmax(-90.0f, p.tilt)) : 0.0f;
        std::memcpy(q.offset, g_mag_off, sizeof(q.offset));
        q.turn = g_mag_turn;
        q.tilt = g_mag_tilt;
    }
    if (!save) return;
    char b[64];
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", q.offset[0], q.offset[1], q.offset[2]);
    config::set("Reload", "MagazineOffset", b);
    std::snprintf(b, sizeof(b), "%.0f", q.turn);
    config::set("Reload", "MagazineTurn", b);
    std::snprintf(b, sizeof(b), "%.0f", q.tilt);
    config::set("Reload", "MagazineTilt", b);
    log::info("[round] the magazine's place in the hand: offset %.3f %.3f %.3f, turn %.0f, tilt %.0f", q.offset[0], q.offset[1], q.offset[2], q.turn, q.tilt);
}

void set_mode(int m, bool save) {
    m = m < 0 ? 0 : m > 2 ? 2 : m;
    if (g_mode.exchange(m) != m) log::info("[round] the round in hand: mode %d%s", m, save ? "" : " (the session)");
    g_on = m > 0;
    if (save) config::set("Reload", "RoundInHand", std::to_string(m));
}

void set_enabled(bool on) { set_mode(on ? (g_mode.load() ? g_mode.load() : 1) : 0); }

void note_pass(int eye, const float* cam, float l, float r, float u, float d) {
    if (!(g_on.load(std::memory_order_relaxed) || g_row_on.load(std::memory_order_relaxed) || show_now() || mags_now()) || eye < 0 || eye > 1 || !cam)
        return;
    const uint64_t frame = state::presents.load(std::memory_order_relaxed);
    g_pass_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    std::lock_guard lock(g_cam_mutex);
    PassCam& c = g_cam[frame % kKeep][eye];
    for (int k = 0; k < 3; ++k) {
        c.a[k] = cam[k];
        c.b[k] = cam[4 + k];
        c.c[k] = cam[8 + k];
        c.d[k] = cam[12 + k];
    }
    c.l = l;
    c.r = r;
    c.u = u;
    c.dn = d;
    c.frame = frame;
    if (g_cam_frame_seen != frame) {  // the frame's first pass
        g_cam_frame_seen = frame;
        g_first_eye[frame % kKeep] = eye;
    }
}

void set_held(int john_hand, int weapon, int count, const float* fwd) {
    g_hand.store(john_hand, std::memory_order_relaxed);
    g_weapon.store(weapon, std::memory_order_relaxed);
    g_count.store(count < 1 ? 1 : count > 8 ? 8 : count, std::memory_order_relaxed);
    g_held_ms.store(log::now_ms(), std::memory_order_relaxed);
    if (fwd) {
        std::lock_guard lock(g_fwd_mutex);
        const float l = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
        if (l > 1e-3f) g_fwd[0] = fwd[0] / l, g_fwd[1] = 0.0f, g_fwd[2] = fwd[2] / l;
    }
}

int held_hand() { return held_now() ? g_hand.load(std::memory_order_relaxed) : -1; }

int held_mag_hand() {
    std::lock_guard lock(g_mag_mutex);
    return log::now_ms() - g_mag_ms < 250.0 && mag_model(g_mag_weapon) >= 0 ? g_mag_hand : -1;
}
float mag_length(int weapon) {
    const int m = mag_model(weapon);
    return m >= 0 ? g_magm[m].len : 0.0f;
}
void set_held_mag(int john_hand, int weapon, int rounds) {
    std::lock_guard lock(g_mag_mutex);
    g_mag_hand = john_hand;
    g_mag_weapon = weapon;
    g_mag_rounds = rounds < 0 ? 0 : rounds;
    g_mag_ms = john_hand >= 0 && john_hand <= 1 && mag_model(weapon) >= 0 ? log::now_ms() : -1e12;
}
bool mag_pose(const body::BodyPoints& bp, int hand, int weapon, float* out) {
    if (hand < 0 || hand > 1 || !bp.hand_ok[hand]) return false;
    auto det3 = [](const float* m) {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    };
    const float* R = std::fabs(det3(bp.target_rot[hand]) - 1.0f) < 0.1f ? bp.target_rot[hand] : bp.wrist_drawn[hand];
    if (std::fabs(det3(R) - 1.0f) >= 0.1f) return false;
    float off[3], ax[3];
    bool axis_auto;
    {
        std::lock_guard lock(g_cfg_mutex);
        const bool r = hand == 1 && g_r_round_own;  // (R13)
        std::memcpy(off, r ? g_r_offset : g_offset, sizeof(off));
        std::memcpy(ax, r ? g_r_axis : g_axis, sizeof(ax));
        axis_auto = r ? g_r_axis_auto : g_axis_auto;
    }
    auto to_world = [&](const float* v, float* o) {
        for (int k = 0; k < 3; ++k) o[k] = R[k * 3] * v[0] + R[k * 3 + 1] * v[1] + R[k * 3 + 2] * v[2];
    };
    float *X = out, *Y = out + 3, *Z = out + 6, *P = out + 9, pinch[3];
    if (bp.tips_ok[hand]) {
        for (int k = 0; k < 3; ++k) pinch[k] = 0.5f * (bp.thumb_tip[hand][k] + bp.index_tip[hand][k]);
    } else {
        to_world(off, pinch);
        for (int k = 0; k < 3; ++k) pinch[k] += bp.hand[hand][k];
    }
    // (r218: the held magazine not seen in the hand) out of the hand: from the wrist through the drawn pinch, whatever
    // RoundAxis says (a long thing along the configured axis lay across the closed hand, hidden by it); the configured
    // axis only without the drawn tips, or with the pinch at the wrist
    float out_d[3];
    for (int k = 0; k < 3; ++k) out_d[k] = pinch[k] - bp.hand[hand][k];
    const float out_l = std::sqrt(out_d[0] * out_d[0] + out_d[1] * out_d[1] + out_d[2] * out_d[2]);
    if (bp.tips_ok[hand] && out_l > 0.02f) {
        for (int k = 0; k < 3; ++k) Y[k] = out_d[k] / out_l;
    } else {
        to_world(ax, Y);
    }
    (void)axis_auto;
    normalize3(Y);
    // (r223: the glove a pointing hand, the magazine out of its index tip along the view ray, a stub seen) the grip's own
    // axis: the gun hand's drawn pistol's up (its grip toward its slide) in that hand's target, mirrored across the hand's
    // side (x: the controllers' frames share their axes, x right on both, as holster's palm_y_of reads them) onto this
    // hand. The magazine stands in the fist as the pistol's grip does in the other, its base in the thumb-index ring, its
    // body up out of it; body.cpp draws the free hand with the gun hand's grip, mirrored, while it holds one ([Hands]
    // CopyGrip's). Held up before the face as a pistol is held, its side is to the eye. Not for the r219 control
    const bool hold_old = g_mag_hold_old.load(std::memory_order_relaxed);
    const int gjh = bp.gun;
    // (r224: "grip axis 0" in every hold: body.cpp publishes the drawn gun's frame for the long guns only, so a pistol never
    // had one) the gun's axes in the gun hand's target: the drawn frame's when published, else the gun hand's own (its
    // barrel as aim measures it, its right the target's x, its up square to both: actions' and holster's own fallback,
    // the model's origin 8 cm ahead of the wrist). Its reason kept for the status (g_mag_axis_why) and r44's check
    int grip_axis = 0;  // 0 none, 1 the drawn gun's frame, 2 the gun hand's target
    const char* why = "";
    float gl_up[3] = {0.0f, 1.0f, 0.0f}, gl_o[3] = {0.0f, 0.0f, 0.0f}, gl_r[3] = {1.0f, 0.0f, 0.0f}, gl_b[3] = {0.0f, 0.0f, 1.0f};  // the gun's axes, origin (the gun hand's target)
    if (hold_old) why = "the r219 control";
    else if (gjh < 0 || gjh > 1 || gjh == hand) why = "no other hand holds the gun";
    else if (!bp.hand_ok[gjh]) why = "the gun hand not tracked";
    else {
        const float* Tg = bp.target_rot[gjh];
        auto loc = [&](const float* wv, float* o) {
            for (int a = 0; a < 3; ++a) o[a] = Tg[0 * 3 + a] * wv[0] + Tg[1 * 3 + a] * wv[1] + Tg[2 * 3 + a] * wv[2];
        };
        if (bp.gun_frame_ok) {
            const float gx[3] = {bp.gun_frame_R[0], bp.gun_frame_R[3], bp.gun_frame_R[6]}, gy[3] = {bp.gun_frame_R[1], bp.gun_frame_R[4], bp.gun_frame_R[7]};
            const float gz[3] = {bp.gun_frame_R[2], bp.gun_frame_R[5], bp.gun_frame_R[8]};
            float go[3];
            for (int k = 0; k < 3; ++k) go[k] = bp.gun_frame_o[k] - bp.hand[gjh][k];
            loc(gx, gl_r), loc(gy, gl_up), loc(gz, gl_b), loc(go, gl_o);
            grip_axis = 1;
        } else {
            float bt[3] = {0.0f, 0.174f, -0.985f};  // the barrel in the target (the measured constant), else aim's
            aim::barrel_in_target(bt);
            const float bl = std::sqrt(bt[0] * bt[0] + bt[1] * bt[1] + bt[2] * bt[2]);
            if (bl > 0.5f) {
                for (int k = 0; k < 3; ++k) bt[k] /= bl;
                gl_r[0] = 1.0f, gl_r[1] = 0.0f, gl_r[2] = 0.0f;
                gl_up[0] = 0.0f, gl_up[1] = -bt[2], gl_up[2] = bt[1];  // right x forward (x cross bt)
                const float ul = std::sqrt(gl_up[1] * gl_up[1] + gl_up[2] * gl_up[2]);
                if (ul > 0.5f) {
                    gl_up[1] /= ul, gl_up[2] /= ul;
                    // (the review) back = right x up, square to both (the barrel need not be square to the right)
                    gl_b[0] = gl_r[1] * gl_up[2] - gl_r[2] * gl_up[1];
                    gl_b[1] = gl_r[2] * gl_up[0] - gl_r[0] * gl_up[2];
                    gl_b[2] = gl_r[0] * gl_up[1] - gl_r[1] * gl_up[0];
                    for (int k = 0; k < 3; ++k) gl_o[k] = bt[k] * 0.08f;  // (the origin 8 cm ahead along the barrel)
                    grip_axis = 2;
                } else {
                    why = "the barrel's up not found";
                }
            } else {
                why = "no barrel in the target";
            }
        }
    }
    if (grip_axis) {
        float l[3] = {-gl_up[0], gl_up[1], gl_up[2]}, A[3];  // mirrored across the hand's side
        to_world(l, A);
        const float al = std::sqrt(A[0] * A[0] + A[1] * A[1] + A[2] * A[2]);
        if (std::isfinite(al) && al > 0.5f) {
            for (int k = 0; k < 3; ++k) Y[k] = A[k] / al;
        } else {
            grip_axis = 0;
            why = "the mirrored axis not finite";
        }
    }
    g_mag_grip_axis.store(grip_axis, std::memory_order_relaxed);
    g_mag_axis_why.store(why, std::memory_order_relaxed);
    if (!grip_axis) g_mag_cover.store(-1.0f, std::memory_order_relaxed);  // (the review: never a stale reading; -1 none)
    // its back (the rounds' bases): the hand's own z, square to Y (else its y)
    const float zt[3] = {R[2], R[5], R[8]}, yt[3] = {R[1], R[4], R[7]};
    const float* zs = std::fabs(zt[0] * Y[0] + zt[1] * Y[1] + zt[2] * Y[2]) < 0.9f ? zt : yt;
    const float dz = zs[0] * Y[0] + zs[1] * Y[1] + zs[2] * Y[2];
    for (int k = 0; k < 3; ++k) Z[k] = zs[k] - dz * Y[k];
    normalize3(Z);
    const bool clipm = weapon == 3;  // the clip held by its top, its foot out along the fingers: turned end for end
    const float len = mag_length(weapon);
    // (the amendment: r219's showed a 2-3 cm stub past the fingertips, the rest in the glove) held by its base AT the
    // fingertips: the held end (a magazine's foot, the clip's top) 1 cm short of the farthest drawn tip out along Y (the
    // thumb's or the index's, from the pinch), the body wholly out past the fingers
    float reach = 0.0f;
    if (bp.tips_ok[hand]) {
        const float* tips[2] = {bp.thumb_tip[hand], bp.index_tip[hand]};
        for (const float* t : tips) reach = std::fmax(reach, (t[0] - pinch[0]) * Y[0] + (t[1] - pinch[1]) * Y[1] + (t[2] - pinch[2]) * Y[2]);
        if (!std::isfinite(reach) || reach > 0.12f) reach = 0.0f;
    }
    float held[3];
    // the grip's hold: its held end 2.5 cm down into the top of the fist: the top of the pistol's grip in the other hand
    // (its frame's point there, kGripTop) mirrored onto this hand as the axis is, so the fingers close on its base and
    // the body stands up out of the fist; else (no gun frame) r223's, 1 cm short of the farther fingertip; the control
    // (maghold old): r219's, 1.5 cm behind the pinch
    const float at = hold_old ? -0.015f : reach - 0.010f;
    for (int k = 0; k < 3; ++k) held[k] = pinch[k] + Y[k] * at;
    if (grip_axis) {
        // a magazine: its base 2.5 cm down into the top of the fist (the top of the pistol's grip: under the slide, the
        // grip's middle); the Mauser's clip: held by its top 2.5 cm up into the bottom of the fist (the grip's bottom),
        // hanging down out of it, as a stripper clip is brought down onto the open action (r224: held up out of the fist,
        // its foot up, it met the gun's top from below, behind the gun). The gun's frame points [I: from the meshes]
        const float top_hp[3] = {0.0f, 0.012f, 0.050f}, top_sa[3] = {0.0f, 0.012f, 0.060f}, bot_ma[3] = {0.0f, -0.062f, 0.082f};
        const float* gt = weapon == 2 ? top_hp : weapon == 1 ? top_sa : bot_ma;
        float rel[3];
        for (int a = 0; a < 3; ++a) rel[a] = gl_o[a] + gl_r[a] * gt[0] + gl_up[a] * gt[1] + gl_b[a] * gt[2];
        rel[0] = -rel[0];  // mirrored across the hand's side
        float tw[3];
        to_world(rel, tw);
        // (r225: 0.45 / 0.51 of the High Power's and the Semi-Auto's seen, both eyes alike: 5.1 of its 9.3 cm hidden in
        // the fist where 2.5 were meant. The drawn fist's knuckles and fingers reach about 2.6 cm above the grip's top
        // point (the closed glove is thicker than the grip it copies); and at the well the same 0.47 / 0.55: the same
        // occluder, its own fist, not the gun or the arm) a magazine's foot AT the grip's top: the knuckles' 2-3 cm hold
        // its base, the rest stands out. The clip held by its top as before, 2.5 cm into the fist's bottom
        for (int k = 0; k < 3; ++k) held[k] = bp.hand[hand][k] + tw[k] + Y[k] * (clipm ? 0.025f : 0.0f);
        // the length of it the drawn fingers cover from its held end: the farther of the thumb's and the index tip's
        // reach along it (the status's, the hand's volume read from the drawn tips)
        float cov = 0.0f;
        if (bp.tips_ok[hand]) {
            const float* tips[2] = {bp.thumb_tip[hand], bp.index_tip[hand]};
            for (const float* t : tips) {
                const float a = (t[0] - held[0]) * Y[0] + (t[1] - held[1]) * Y[1] + (t[2] - held[2]) * Y[2];
                cov = std::fmax(cov, clipm ? -a : a);
            }
        }
        g_mag_cover.store(cov, std::memory_order_relaxed);
    }
    if (clipm && grip_axis) {  // (the clip hangs down: its foot a length below its held top, its own y up the grip's)
        for (int k = 0; k < 3; ++k) P[k] = held[k] - Y[k] * len;
    } else {
        for (int k = 0; k < 3; ++k) P[k] = clipm ? held[k] + Y[k] * len : held[k];  // (the clip: its foot a length out)
        if (clipm)
            for (int k = 0; k < 3; ++k) Y[k] = -Y[k];
    }
    {  // (2026-10-29, round 38) the player's own place for it: turned about the held end (the hand's up, then its right), then
       // moved (the hand's axes, as the round's offset); the well and a let-go take it as drawn
        float mo[3], mt, ml;
        {
            std::lock_guard lock(g_cfg_mutex);
            const bool r = hand == 1 && g_r_mag_own;  // (R13)
            std::memcpy(mo, r ? g_r_mag_off : g_mag_off, sizeof(mo));
            mt = r ? g_r_mag_turn : g_mag_turn;
            ml = r ? g_r_mag_tilt : g_mag_tilt;
            const int hs = hand == 1 ? 1 : 0;
            if (wmag_ok(weapon) && g_wmag[weapon][hs].own) {  // (C38) the pistol's own
                std::memcpy(mo, g_wmag[weapon][hs].off, sizeof(mo));
                mt = g_wmag[weapon][hs].turn;
                ml = g_wmag[weapon][hs].tilt;
            }
        }
        if (mt != 0.0f || ml != 0.0f) {
            const float a = mt * 0.0174532925f, c = ml * 0.0174532925f;
            const float Ry[9] = {std::cos(a), 0.0f, std::sin(a), 0.0f, 1.0f, 0.0f, -std::sin(a), 0.0f, std::cos(a)};
            const float Rx[9] = {1.0f, 0.0f, 0.0f, 0.0f, std::cos(c), -std::sin(c), 0.0f, std::sin(c), std::cos(c)};
            float L[9], RL[9], Ra[9];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) L[i * 3 + j] = Ry[i * 3] * Rx[j] + Ry[i * 3 + 1] * Rx[3 + j] + Ry[i * 3 + 2] * Rx[6 + j];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) RL[i * 3 + j] = R[i * 3] * L[j] + R[i * 3 + 1] * L[3 + j] + R[i * 3 + 2] * L[6 + j];
            for (int i = 0; i < 3; ++i)  // Ra = R L R^T
                for (int j = 0; j < 3; ++j) Ra[i * 3 + j] = RL[i * 3] * R[j * 3] + RL[i * 3 + 1] * R[j * 3 + 1] + RL[i * 3 + 2] * R[j * 3 + 2];
            auto turn = [&](float* v) {
                const float v0[3] = {v[0], v[1], v[2]};
                for (int k = 0; k < 3; ++k) v[k] = Ra[k * 3] * v0[0] + Ra[k * 3 + 1] * v0[1] + Ra[k * 3 + 2] * v0[2];
            };
            float d[3] = {P[0] - held[0], P[1] - held[1], P[2] - held[2]};
            turn(Y);
            turn(Z);
            turn(d);
            for (int k = 0; k < 3; ++k) P[k] = held[k] + d[k];
        }
        float mw[3];
        to_world(mo, mw);
        for (int k = 0; k < 3; ++k) P[k] += mw[k];
    }
    X[0] = Y[1] * Z[2] - Y[2] * Z[1];
    X[1] = Y[2] * Z[0] - Y[0] * Z[2];
    X[2] = Y[0] * Z[1] - Y[1] * Z[0];
    for (int k = 0; k < 12; ++k)
        if (!std::isfinite(out[k])) return false;
    return true;
}
void drop_mag(const float pose[12], int weapon, int rounds) {
    if (!pose || mag_model(weapon) < 0) return;
    for (int k = 0; k < 12; ++k)
        if (!std::isfinite(pose[k])) return;
    std::lock_guard lock(g_mag_mutex);
    Drop& x = g_drop[g_ndrop++ % 3];
    std::memcpy(x.pz, pose, sizeof(x.pz));
    x.t0 = log::now_ms();
    x.weapon = weapon;
    x.rounds = rounds < 0 ? 0 : rounds;
    g_drops_made.fetch_add(1, std::memory_order_relaxed);
}
void seat_clip(const float pose[12], int rounds) {
    if (!pose) return;
    for (int k = 0; k < 12; ++k)
        if (!std::isfinite(pose[k])) return;
    std::lock_guard lock(g_mag_mutex);
    std::memcpy(g_seat.pz, pose, sizeof(g_seat.pz));
    g_seat.rounds = rounds < 0 ? 0 : rounds;
    g_seat.ms = log::now_ms();
}

int shell_poses(const body::BodyPoints& bp, float* out, int max, int* n_hand) {
    int n = 0;
    *n_hand = 0;
    const bool newm = g_models.load(std::memory_order_relaxed);
    const int hand = g_hand.load(std::memory_order_relaxed), w = g_weapon.load(std::memory_order_relaxed);
    if (kind_of(w) == kKShell && g_on.load(std::memory_order_relaxed) && held_now() && hand >= 0 && hand <= 1 && bp.hand_ok[hand]) {
        float pz[12];
        if (pose(bp, hand, pz)) {  // the handful's, as record() places them
            const int cnt = g_count.load(std::memory_order_relaxed);
            static float s_dir[3] = {0.0f, 0.0f, 0.0f};  // (the holsters' thread's own: record keeps its)
            float S[3];
            handful_dir(bp, hand, pz, cnt, s_dir, S);
            const Model& m = model_for(kKShell, 3, newm);
            const float gap = held_gap(m, 3, newm);
            for (int r = 0; r < cnt && n < max; ++r, ++n) {
                std::memcpy(out + n * 12, pz, sizeof(pz));
                for (int k = 0; k < 3; ++k) out[n * 12 + 9 + k] = pz[9 + k] + S[k] * gap * r;
            }
        }
    }
    *n_hand = n;
    Row row;
    {
        std::lock_guard lock(g_row_mutex);
        row = g_row;
    }
    if (g_row_on.load(std::memory_order_relaxed) && row.n > 0 && kind_of(row.weapon) == kKShell && log::now_ms() - row.ms < 250.0) {
        const int nr = row.n > 6 ? 6 : row.n;
        const Model& m = model_for(kKShell, 3, newm);
        for (int r = 0; r < nr && n < max; ++r, ++n) row_pose(row, r, nr, row_gap(m, 3, newm), out + n * 12);
    }
    return n;
}

void list_submitted(int slot, bool ok) {
    if (g_atlas_state != 1 || slot != g_atlas_slot) return;
    if (ok) {
        g_atlas_state = 2;
        g_grave.push_back({g_atlas_up, state::presents.load(std::memory_order_relaxed)});  // released once no list can still read it
        g_atlas_up = nullptr;
        log::info("[round] the atlas copied (%dx%d, %d mips)", kAtlasW, g_atlas_h, kMips);
    } else {
        g_atlas_state = 0;  // (its list never ran: still in COPY_DEST, copied again in the next)
        log::warn("[round] the atlas's copy was in a list not submitted; copied again");
    }
}

void record(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* const dst[2], const D3D12_CPU_DESCRIPTOR_HANDLE rtv[2],
            uint32_t w, uint32_t h, DXGI_FORMAT fmt, int slot, uint32_t vw, uint32_t vh, uint32_t dw, uint32_t dh, const EyeImages* eyes) {
    if (slot < 0 || slot >= kSlots) return;
    g_records.fetch_add(1, std::memory_order_relaxed);
    g_rec_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_ready && g_q_pending[slot]) {  // this slot's counts from its last use (the GPU has passed it)
        uint64_t* q = nullptr;
        D3D12_RANGE r{slot * kQ * sizeof(uint64_t), (slot * kQ + kQ) * sizeof(uint64_t)};
        if (SUCCEEDED(g_qread->Map(0, &r, reinterpret_cast<void**>(&q))) && q) {
            g_px[0] = q[slot * kQ];
            g_px[1] = q[slot * kQ + 1];
            g_row_px[0] = q[slot * kQ + 2];
            g_row_px[1] = q[slot * kQ + 3];
            if (g_q_mag[slot]) {  // (only a slot that drew a measured magazine: else the last drawn's kept)
                for (int e = 0; e < 2; ++e) {
                    g_mag_seen[e].store(q[slot * kQ + 4 + e], std::memory_order_relaxed);
                    g_mag_all[e].store(q[slot * kQ + 6 + e], std::memory_order_relaxed);
                }
                g_mag_seen_kind.store(g_q_kind[slot], std::memory_order_relaxed);
            }
            for (int e = 0; e < 2; ++e)
                if (q[slot * kQ + e]) g_px_seen[e].fetch_add(1, std::memory_order_relaxed);
            D3D12_RANGE none{0, 0};
            g_qread->Unmap(0, &none);
            g_px_frames.fetch_add(1, std::memory_order_relaxed);
        }
        g_q_pending[slot] = false;
    }
    if (g_ready && g_probe_pending[slot]) {  // this slot's copy of the probe's texels (the status's)
        uint8_t* p = nullptr;
        D3D12_RANGE r{static_cast<SIZE_T>(slot) * 512, static_cast<SIZE_T>(slot) * 512 + kProbeTexels * 8};
        if (SUCCEEDED(g_probe_rb->Map(0, &r, reinterpret_cast<void**>(&p))) && p) {
            std::lock_guard lock(g_probe_mutex);
            for (int i = 0; i < kProbeTexels; ++i)
                for (int c = 0; c < 4; ++c) {
                    uint16_t hv;
                    std::memcpy(&hv, p + slot * 512 + i * 8 + c * 2, 2);
                    g_probe_val[i][c] = half_to_float(hv);
                }
            D3D12_RANGE none{0, 0};
            g_probe_rb->Unmap(0, &none);
        }
        g_probe_pending[slot] = false;
    }
    if (g_grab_phase.load(std::memory_order_acquire) == 2 && g_grab_slot == slot) write_grab();
    const bool want_mag = mags_now();  // (2026-10-28: the magazines are PistolMagazines', whatever RoundInHand says)
    if (!g_on.load(std::memory_order_relaxed) && !g_row_on.load(std::memory_order_relaxed) && !show_now() && !want_mag) return;
    const uint64_t frame = state::presents.load(std::memory_order_relaxed);
    if (!g_ready && !g_failed) {
        g_ready = make(dev, w, h, fmt);
        g_failed = !g_ready;
        log::info("[round] the round's pipeline %s (%ux%u, format %d)", g_ready ? "made" : "NOT made", w, h, static_cast<int>(fmt));
    }
    if (!g_ready || w != g_w || h != g_h || fmt != g_fmt) return;
    bool atlas_here = g_atlas_state == 2;
    if (g_atlas_state == 0 && g_atlas_up) {  // (2026-10-27) the atlas's one copy, in the first list after it is made
        for (UINT m = 0; m < kMips; ++m) {
            D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
            dl.pResource = g_atlas_tex;
            dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dl.SubresourceIndex = m;
            sl.pResource = g_atlas_up;
            sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sl.PlacedFootprint = g_atlas_fp[m];
            cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        }
        barrier(cl, g_atlas_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_atlas_state = 1;
        g_atlas_slot = slot;
        atlas_here = true;  // (this list: after the copy and its barrier)
    }
    // [XR] EyeShape: the eye image's rect at the image's origin (else the whole image)
    const uint32_t iw = vw && vw <= w ? vw : w, ih = vh && vh <= h ? vh : h;
    g_iw_last.store(iw, std::memory_order_relaxed);  // ("round project")
    g_ih_last.store(ih, std::memory_order_relaxed);
    int cx = static_cast<int>(iw / 2), cy = static_cast<int>(ih / 2);  // the capture's crop centre (the round's, when drawn)
    struct AtEnd {
        ID3D12Device* dev;
        ID3D12GraphicsCommandList* cl;
        ID3D12Resource* const* dst;
        int slot;
        int *cx, *cy;
        ~AtEnd() { grab_copy(dev, cl, dst, slot, *cx, *cy); }
    } at_end{dev, cl, dst, slot, &cx, &cy};
    ensure_buffer(dev, frame);
    const int hand = g_hand.load(std::memory_order_relaxed);
    body::BodyPoints bp;
    const bool have_bp = body::body_points(&bp);
    const bool want_round = g_on.load(std::memory_order_relaxed) && held_now() && hand >= 0 && hand <= 1 && have_bp && bp.hand_ok[hand];
    Row row;
    {
        std::lock_guard lock(g_row_mutex);
        row = g_row;
    }
    // (as before: a gun with a family, audio::click_family > 0; the same weapons kind_of names)
    const bool want_row = g_row_on.load(std::memory_order_relaxed) && row.n > 0 && audio::click_family(row.weapon) > 0 && log::now_ms() - row.ms < 250.0;
    if (!want_row) g_row_px[0] = g_row_px[1] = 0;  // no row drawn: none of it seen (its last count would stay)
    Show show;
    {
        std::lock_guard lock(g_show_mutex);
        show = g_show;
    }
    const bool want_show = show.on;
    if (!want_round && !want_row && !want_show && !want_mag) return;
    // the newest frame with both eyes' cameras (and depths, if they are used)
    PassCam cam[2];
    uint64_t use = ~0ull;
    int par = -1;
    {
        std::lock_guard lock(g_cam_mutex);
        for (int i = 0; i < kKeep; ++i) {
            const uint64_t f = g_cam[i][0].frame;
            if (f == ~0ull || g_cam[i][1].frame != f || frame - f > 3) continue;
            if (use == ~0ull || f > use) {
                use = f;
                par = i;
            }
        }
        if (par >= 0) {
            cam[0] = g_cam[par][0];
            cam[1] = g_cam[par][1];
        }
    }
    if (par < 0) {
        g_no_cam.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    DepthMeta dm;
    bool have_depth;
    {
        std::lock_guard lock(g_depth_mutex);
        dm = g_dmeta;
        have_depth = g_dframe[par][0] == use && g_dframe[par][1] == use;
    }
    ID3D12Resource* buf = g_dbuf.load(std::memory_order_acquire);
    const bool occlude = g_occlude.load(std::memory_order_relaxed);
    static uint64_t s_no_depth_run = 0;  // frames in a row wanted but not drawn for want of the scene depth
    if (!buf || (occlude && (!have_depth || !dm.w))) {  // never drawn without the buffer bound (the shader names it)
        g_no_depth.fetch_add(1, std::memory_order_relaxed);
        if (++s_no_depth_run == 450)
            log::warn("[round] the rounds wanted for 450 frames but not drawn: no scene depth copy (%s; taps %llu)",
                      g_resolve.load(std::memory_order_relaxed) ? "a Depth Resolve taken, no copy this frame" : "no Depth Resolve copy seen",
                      static_cast<unsigned long long>(g_taps.load()));
        return;
    }
    if (s_no_depth_run >= 450) log::info("[round] drawn again after %llu frames without the scene depth", static_cast<unsigned long long>(s_no_depth_run));
    s_no_depth_run = 0;
    float pz[12] = {}, bright, bias;
    const bool round_ok = want_round && pose(bp, hand, pz);
    const float *X = pz, *Y = pz + 3, *Z = pz + 6, *P = pz + 9;
    {
        std::lock_guard lock(g_cfg_mutex);
        bright = g_bright;
        bias = g_bias;
    }
    if (!round_ok && !want_row && !want_show && !want_mag) return;
    // (the new models only with the atlas in its shader-resource state: never an unwritten texture)
    const bool newm = g_models.load(std::memory_order_relaxed) && atlas_here;
    // the magazines (2026-10-28): held, falling, seated; the new models' pipeline only (they need the atlas)
    struct MagDraw {
        int model, weapon, rounds;
        float pz[12];
    } mitems[7];
    int nm = 0, held_mi = -1, show_mi = -1;  // (the held one's index; the shown one's)
    if (want_mag && atlas_here && have_bp) {  // (RoundModels 0 too: their own pipeline, the new, switched to for them)
        const double tnow = log::now_ms();
        std::lock_guard lock(g_mag_mutex);
        if (tnow - g_mag_ms < 250.0 && mag_model(g_mag_weapon) >= 0 && mag_pose(bp, g_mag_hand, g_mag_weapon, mitems[nm].pz)) {
            mitems[nm].model = mag_model(g_mag_weapon), mitems[nm].weapon = g_mag_weapon, mitems[nm].rounds = g_mag_rounds;
            held_mi = nm;
            ++nm;
        }
        if (g_mshow.on && mag_model(g_mshow.weapon) >= 0) {
            mitems[nm].model = mag_model(g_mshow.weapon), mitems[nm].weapon = g_mshow.weapon, mitems[nm].rounds = g_mshow.rounds;
            std::memcpy(mitems[nm].pz, g_mshow.pz, sizeof(g_mshow.pz));
            if (held_mi < 0) show_mi = nm;  // (measured when none is held: r44's open-air and hidden controls)
            ++nm;
        }
        // (step 7, R12) the magazine row: upright (the mag's y up), its front (-z) the body's forward, 5 cm apart
        if (g_mrow.n > 0 && tnow - g_mrow.ms < 250.0 && mag_model(g_mrow.weapon) >= 0) {
            for (int i = 0; i < g_mrow.n && i < 2 && nm < 7; ++i) {
                MagDraw& md = mitems[nm++];
                md.model = mag_model(g_mrow.weapon), md.weapon = g_mrow.weapon, md.rounds = g_mrow.rounds;
                for (int k = 0; k < 3; ++k) {
                    md.pz[k] = g_mrow.rgt[k];
                    md.pz[3 + k] = g_mrow.up[k];
                    md.pz[6 + k] = -g_mrow.fwd[k];
                    md.pz[9 + k] = g_mrow.at[k] + g_mrow.rgt[k] * (0.05f * static_cast<float>(i));
                }
            }
            g_mrow_drawn.fetch_add(1, std::memory_order_relaxed);
        }
        if (tnow - g_seat.ms < 250.0) {
            mitems[nm].model = 2, mitems[nm].weapon = 3, mitems[nm].rounds = g_seat.rounds;
            std::memcpy(mitems[nm].pz, g_seat.pz, sizeof(g_seat.pz));
            ++nm;
        }
        for (const Drop& x : g_drop) {  // falling (a push down, then gravity), the same way up
            const double t = (tnow - x.t0) * 0.001;
            if (!(t >= 0.0 && t * 1000.0 < kDropMs) || mag_model(x.weapon) < 0 || nm >= 7) continue;
            MagDraw& md = mitems[nm++];
            md.model = mag_model(x.weapon), md.weapon = x.weapon, md.rounds = x.rounds;
            std::memcpy(md.pz, x.pz, sizeof(md.pz));
            md.pz[10] += static_cast<float>(-0.4 * t - 0.5 * 9.81 * t * t);
        }
    }
    if (!round_ok && !want_row && !want_show && nm == 0) return;
    const int meas_mi = held_mi >= 0 ? held_mi : show_mi;  // (the seen and all counts' subject)
    const int weapon = g_weapon.load(std::memory_order_relaxed), kind = kind_of(weapon), fam = audio::click_family(weapon);
    const Model& m = model_for(kind, fam, newm);
    const int held_n = round_ok ? g_count.load(std::memory_order_relaxed) : 0;
    static float s_dir[3] = {0.0f, 0.0f, 0.0f};
    float S[3] = {Z[0], Z[1], Z[2]};
    if (round_ok) handful_dir(bp, hand, pz, held_n, s_dir, S);
    const float hgap = held_gap(m, fam, newm);
    const int rkind = kind_of(row.weapon), rfam = audio::click_family(row.weapon);
    const Model& rm = model_for(rkind, rfam, newm);
    const float gap = row_gap(rm, rfam, newm);
    const int nr = want_row ? (row.n > 6 ? 6 : row.n) : 0;
    const Model& sm = model_for(show.kind, old_family(show.kind), newm);
    // [Reload] RoundInHand = 2 (2026-10-27): the game's own shells (held_prop's pool) draw the shells' poses (the
    // handful's from the pinch out, then the row's: shell_poses' order); a pose none of them drew in the last 60 ms is
    // drawn here (the first 2 s of a session, a creation that failed, an object the game removed: the stand-in)
    const bool hand_game = g_mode.load(std::memory_order_relaxed) == 2 && kind == kKShell;
    const bool row_game = g_mode.load(std::memory_order_relaxed) == 2 && rkind == kKShell;
    bool skip_hand[8] = {}, skip_row[6] = {};
    int hcov = 0, rcov = 0;
    for (int r = 0; r < held_n && r < 8; ++r) {
        skip_hand[r] = hand_game && held_prop::pool_shown(r);
        hcov += skip_hand[r] ? 1 : 0;
    }
    for (int r = 0; r < nr; ++r) {
        skip_row[r] = row_game && held_prop::pool_shown(held_n + r);
        rcov += skip_row[r] ? 1 : 0;
    }
    const int hwant = hand_game ? held_n : 0, rwant = row_game ? nr : 0;
    g_covered.fetch_add(static_cast<uint64_t>(hcov + rcov), std::memory_order_relaxed);
    g_stand_ins.fetch_add(static_cast<uint64_t>(hwant - hcov + rwant - rcov), std::memory_order_relaxed);
    g_hand_cover.store(hcov, std::memory_order_relaxed);
    g_hand_want.store(hwant, std::memory_order_relaxed);
    g_row_cover.store(rcov, std::memory_order_relaxed);
    g_row_want.store(rwant, std::memory_order_relaxed);
    float n = 0.1f, f = 7500.0f;
    camera_lever::scene_clip(&n, &f);
    if (!(n > 0.001f) || !(f > n)) n = 0.1f, f = 7500.0f;
    // this slot's views: each eye's depth (t0) and eye image (t2) (the atlas, t1, was written once)
    const bool probe_ok = newm && eyes && eyes->tex[0] && eyes->tex[1] && g_probe_on.load(std::memory_order_relaxed) && eyes->cw && eyes->ch;
    for (int e = 0; e < 2; ++e) {
        D3D12_CPU_DESCRIPTOR_HANDLE hc = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        hc.ptr += static_cast<SIZE_T>(slot * kPerSlot + e * 3) * g_srv_step;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.NumElements = static_cast<UINT>(g_dbuf_size.load(std::memory_order_relaxed) / 4);
        sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        dev->CreateShaderResourceView(buf, &sv, hc);
        hc.ptr += static_cast<SIZE_T>(2) * g_srv_step;
        D3D12_SHADER_RESOURCE_VIEW_DESC ev{};
        ev.Format = probe_ok ? eyes->fmt : DXGI_FORMAT_R8G8B8A8_UNORM;
        ev.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        ev.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ev.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(probe_ok ? eyes->tex[e] : nullptr, &ev, hc);  // (none: a null view)
    }
    auto table = [&](int e) {
        D3D12_GPU_DESCRIPTOR_HANDLE hg = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        hg.ptr += static_cast<UINT64>(slot * kPerSlot + e * 3) * g_srv_step;
        return hg;
    };
    D3D12_GPU_DESCRIPTOR_HANDLE probe_srv = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
    probe_srv.ptr += static_cast<UINT64>(kSlots * kPerSlot) * g_srv_step;
    D3D12_GPU_DESCRIPTOR_HANDLE null_srv = probe_srv;
    null_srv.ptr += g_srv_step;
    cl->SetGraphicsRootSignature(g_root);
    cl->SetDescriptorHeaps(1, &g_srv_heap);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv{g_vb->GetGPUVirtualAddress(), static_cast<UINT>(g_mesh.size() * sizeof(Vtx)), sizeof(Vtx)};
    D3D12_INDEX_BUFFER_VIEW ibv{g_ib->GetGPUVirtualAddress(), static_cast<UINT>(g_idx.size() * sizeof(uint16_t)), DXGI_FORMAT_R16_UINT};
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->IASetIndexBuffer(&ibv);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    const int gpar = static_cast<int>(use % kKeep);
    auto depth_scale = [&](float* sx, float* sy) {
        *sx = dw ? static_cast<float>(dw) / static_cast<float>(iw) : dm.w ? static_cast<float>(dm.w) / static_cast<float>(iw) : 1.0f;
        *sy = dh ? static_cast<float>(dh) / static_cast<float>(ih) : dm.h ? static_cast<float>(dm.h) / static_cast<float>(ih) : 1.0f;
    };
    const uint32_t dfmt0 = dm.fmt == DXGI_FORMAT_R32_TYPELESS || dm.fmt == DXGI_FORMAT_D32_FLOAT || dm.fmt == DXGI_FORMAT_R32_FLOAT ||
                                   dm.fmt == DXGI_FORMAT_R32G8X24_TYPELESS || dm.fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                               ? 1u
                               : 0u;  // else D24 in the low bits (measured: the scene depth is D24S8, standard Z)
    // the world's light (2026-10-27): a key from above, the body's right and front (toward the light; as the old view-
    // space key, 0.35 right, 0.85 up, 0.40 toward the viewer, but fixed to the body: the highlight stays when the head
    // turns), and the world's up for the sky and ground ambient
    float Lw[3], Uw[3] = {0.0f, 1.0f, 0.0f};
    {
        float fw[3];
        {
            std::lock_guard lock(g_fwd_mutex);
            std::memcpy(fw, g_fwd, sizeof(fw));
        }
        const float rg[3] = {-fw[2], 0.0f, fw[0]};  // fwd x up (y up): the body's right
        for (int k = 0; k < 3; ++k) Lw[k] = 0.35f * rg[k] + 0.85f * Uw[k] - 0.40f * fw[k];
        normalize3(Lw);
    }
    // the subjects' centres and sizes (world), for the probe's rings: the handful, the row, the show's round
    struct Subj {
        bool on = false;
        float c[3] = {};
        float r = 0.0f;
    } subj[kSubjects];
    if (round_ok) {
        subj[kSubHand].on = true;
        const float spread = hgap * static_cast<float>(held_n - 1);
        for (int k = 0; k < 3; ++k) subj[kSubHand].c[k] = P[k] + Y[k] * 0.5f * m.len + S[k] * 0.5f * spread;
        subj[kSubHand].r = 0.5f * m.len + 0.5f * spread + m.rad;
    }
    if (!round_ok && nm > 0) {  // (the magazine in the hand's place: the hand's probe)
        subj[kSubHand].on = true;
        const float hl = 0.5f * g_magm[mitems[0].model].len;
        for (int k = 0; k < 3; ++k) subj[kSubHand].c[k] = mitems[0].pz[9 + k] + mitems[0].pz[3 + k] * hl;
        subj[kSubHand].r = hl + 0.02f;
    }
    if (nr > 0) {
        subj[kSubRow].on = true;
        for (int k = 0; k < 3; ++k) subj[kSubRow].c[k] = row.at[k] + row.up[k] * 0.5f * rm.len;
        subj[kSubRow].r = 0.5f * gap * static_cast<float>(nr - 1) + 0.5f * rm.len;
    }
    if (want_show) {
        subj[kSubShow].on = true;
        for (int k = 0; k < 3; ++k) subj[kSubShow].c[k] = show.pz[9 + k] + show.pz[3 + k] * 0.5f * sm.len;
        subj[kSubShow].r = 0.5f * sm.len + sm.rad;
    }
    auto to_view = [&](const PassCam& c, const float* p, double* v) {
        const double pd[3] = {static_cast<double>(p[0]) - c.d[0], static_cast<double>(p[1]) - c.d[1], static_cast<double>(p[2]) - c.d[2]};
        const double* rows[3] = {c.a, c.b, c.c};
        for (int i = 0; i < 3; ++i) v[i] = rows[i][0] * pd[0] + rows[i][1] * pd[1] + rows[i][2] * pd[2];
    };
    auto to_px = [&](const PassCam& c, const double* v, double* x, double* y) {  // a view-space point in the image (px)
        const double nx = (2 * v[0] / -v[2] - (c.r + c.l)) / (c.r - c.l), ny = (2 * v[1] / -v[2] - (c.u + c.dn)) / (c.u - c.dn);
        *x = (nx * 0.5 + 0.5) * iw;
        *y = (0.5 - ny * 0.5) * ih;
    };
    // (the amendment) the held magazine's model box projected into each eye: the bounding rectangle of its 8 corners on
    // the image, clipped to it (px; an upper bound of its silhouette): r44's measure of how much of it must be seen
    g_mag_measured.store(held_mi >= 0 ? 1 : show_mi >= 0 ? 2 : 0, std::memory_order_relaxed);
    if (meas_mi >= 0) {
        const MagDraw& md = mitems[meas_mi];
        const float* b = g_magb[md.model];
        for (int e = 0; e < 2; ++e) {  // its long axis (y) against the eye's ray to its middle
            const float hl = 0.5f * g_magm[md.model].len;
            double ray[3], rl = 0.0;
            for (int k = 0; k < 3; ++k) {
                ray[k] = static_cast<double>(md.pz[9 + k] + md.pz[3 + k] * hl) - cam[e].d[k];
                rl += ray[k] * ray[k];
            }
            rl = std::sqrt(rl);
            double cs = rl > 1e-6 ? (ray[0] * md.pz[3] + ray[1] * md.pz[4] + ray[2] * md.pz[5]) / rl : 1.0;
            cs = std::clamp(std::fabs(cs), 0.0, 1.0);
            g_mag_view_deg[e].store(static_cast<float>(std::acos(cs) * 57.29578), std::memory_order_relaxed);
        }
        for (int e = 0; e < 2; ++e) {
            double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
            bool ok = true;
            for (int c8 = 0; c8 < 8 && ok; ++c8) {
                const float lx = c8 & 1 ? b[2] : b[0], ly = c8 & 2 ? b[5] : b[4], lz = c8 & 4 ? b[3] : b[1];
                float wc[3];
                for (int k = 0; k < 3; ++k) wc[k] = md.pz[9 + k] + md.pz[k] * lx + md.pz[3 + k] * ly + md.pz[6 + k] * lz;
                double v[3], x, y;
                to_view(cam[e], wc, v);
                if (v[2] > -0.02) ok = false;
                else {
                    to_px(cam[e], v, &x, &y);
                    x0 = std::fmin(x0, x), y0 = std::fmin(y0, y), x1 = std::fmax(x1, x), y1 = std::fmax(y1, y);
                }
            }
            const double cw = ok ? std::fmax(0.0, std::fmin(x1, static_cast<double>(iw)) - std::fmax(x0, 0.0)) : 0.0;
            const double chh = ok ? std::fmax(0.0, std::fmin(y1, static_cast<double>(ih)) - std::fmax(y0, 0.0)) : 0.0;
            g_mag_proj[e].store(static_cast<float>(cw * chh), std::memory_order_relaxed);
        }
    }
    // the probe pass: each eye's subjects' texels, each blended toward its new value by 1 - e^(-dt / 0.3 s) (at once
    // after half a second unprobed), then a copy for the status and back to a shader resource for the rounds
    if (probe_ok) {
        barrier(cl, g_probe_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE prtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        cl->OMSetRenderTargets(1, &prtv, FALSE, nullptr);
        cl->SetPipelineState(g_pso_probe);
        cl->SetGraphicsRootDescriptorTable(2, null_srv);
        const double now = log::now_ms();
        for (int s = 0; s < kSubjects; ++s) {
            if (!subj[s].on) continue;
            const double dt = now - g_probe_ms[s];
            const float kf = dt > 500.0 || dt < 0.0 ? 1.0f : static_cast<float>(1.0 - std::exp(-dt / 300.0));
            g_probe_ms[s] = now;
            const float bf[4] = {kf, kf, kf, kf};
            cl->OMSetBlendFactor(bf);
            for (int e = 0; e < 2; ++e) {
                const PassCam& c = cam[e];
                double v[3], x, y;
                to_view(c, subj[s].c, v);
                if (v[2] > -0.02) continue;  // behind the eye: kept as it was
                to_px(c, v, &x, &y);
                const double rpx = subj[s].r / -v[2] * (2.0 / (c.r - c.l)) * iw * 0.5;
                ProbeConsts pk{};
                pk.c0[0] = static_cast<float>(x);
                pk.c0[1] = static_cast<float>(y);
                pk.c0[2] = static_cast<float>(std::fmax(6.0, 1.8 * rpx));
                pk.c0[3] = eyes->gamma;
                pk.c1[0] = static_cast<float>(eyes->cw) / static_cast<float>(iw);
                pk.c1[1] = static_cast<float>(eyes->ch) / static_cast<float>(ih);
                depth_scale(&pk.c1[2], &pk.c1[3]);
                pk.d0[0] = static_cast<uint32_t>(dm.stride * static_cast<uint64_t>(gpar * 2 + e));
                pk.d0[1] = dm.pitch;
                pk.d0[2] = dm.w;
                pk.d0[3] = dm.h;
                pk.d1[0] = dfmt0;
                pk.d1[1] = have_depth && dm.w ? 1u : 0u;
                pk.d1[2] = iw;
                pk.d1[3] = ih;
                const int texel = e * 4 + s;
                D3D12_VIEWPORT pv{static_cast<float>(texel), 0, 1, 1, 0, 1};
                D3D12_RECT pr{texel, 0, texel + 1, 1};
                cl->RSSetViewports(1, &pv);
                cl->RSSetScissorRects(1, &pr);
                cl->SetGraphicsRootDescriptorTable(1, table(e));
                cl->SetGraphicsRoot32BitConstants(0, sizeof(pk) / 4, &pk, 0);
                cl->DrawInstanced(3, 1, 0, 0);
            }
        }
        barrier(cl, g_probe_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = g_probe_rb;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dl.PlacedFootprint.Offset = static_cast<UINT64>(slot) * 512;
        dl.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, kProbeTexels, 1, 1, 256};
        sl.pResource = g_probe_tex;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        barrier(cl, g_probe_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_probe_pending[slot] = true;
        g_probes.fetch_add(1, std::memory_order_relaxed);
    }
    cl->SetPipelineState(newm ? g_pso2 : g_pso);
    D3D12_VIEWPORT vp{0, 0, static_cast<float>(iw), static_cast<float>(ih), 0, 1};
    D3D12_RECT sc{0, 0, static_cast<LONG>(iw), static_cast<LONG>(ih)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootDescriptorTable(2, probe_srv);  // (every root parameter set for both pipelines)
    const float fade = eyes ? std::clamp(eyes->fade, 0.0f, 1.0f) : 0.0f;
    bool crop_set = false;
    auto draw = [&](const Model& md) {
        if (md.indexed) cl->DrawIndexedInstanced(md.count, 1, md.first, md.base, 0);
        else cl->DrawInstanced(md.count, 1, md.first, 0);
    };
    for (int e = 0; e < 2; ++e) {
        if (eyes && eyes->skip[e]) {  // (r55 item 11: the scope's eye) nothing drawn; its queries issued empty (each resolved below)
            for (int q = 0; q < 4; ++q) {
                cl->BeginQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 2 * q + e));
                cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 2 * q + e));
            }
            continue;
        }
        const PassCam& c = cam[e];
        Consts k{};
        const double* rows[3] = {c.a, c.b, c.c};
        // the model-to-eye rows for a pose (its axes X, Y, Z and its position P); vz: its centre's eye z
        auto place = [&](const float* X_, const float* Y_, const float* Z_, const float* P_, double* vz_) {
            const double pd[3] = {static_cast<double>(P_[0]) - c.d[0], static_cast<double>(P_[1]) - c.d[1], static_cast<double>(P_[2]) - c.d[2]};
            for (int i = 0; i < 3; ++i) {
                const double* a = rows[i];
                k.L[i][0] = static_cast<float>(a[0] * X_[0] + a[1] * X_[1] + a[2] * X_[2]);
                k.L[i][1] = static_cast<float>(a[0] * Y_[0] + a[1] * Y_[1] + a[2] * Y_[2]);
                k.L[i][2] = static_cast<float>(a[0] * Z_[0] + a[1] * Z_[1] + a[2] * Z_[2]);
                const double t = a[0] * pd[0] + a[1] * pd[1] + a[2] * pd[2];
                k.L[i][3] = static_cast<float>(t);
                if (i == 2 && vz_) *vz_ = t;
            }
        };
        k.proj[0] = 2.0f / (c.r - c.l);
        k.proj[1] = (c.r + c.l) / (c.r - c.l);
        k.proj[2] = 2.0f / (c.u - c.dn);
        k.proj[3] = (c.u + c.dn) / (c.u - c.dn);
        k.zc[0] = f / (n - f);
        k.zc[1] = k.zc[0] * n;
        k.zc[2] = n;
        k.zc[3] = f;
        depth_scale(&k.scale[0], &k.scale[1]);
        k.scale[2] = bright * (newm ? 1.0f - fade : 1.0f);
        k.scale[3] = bias;
        k.dinfo[0] = static_cast<uint32_t>(dm.stride * static_cast<uint64_t>(gpar * 2 + e));
        k.dinfo[1] = dm.pitch;
        k.dinfo[2] = dm.w;
        k.dinfo[3] = dm.h;
        k.dfmt[0] = dfmt0;
        k.dfmt[1] = occlude && buf ? 1u : 0u;
        k.dfmt[2] = g_ndc.load(std::memory_order_relaxed) ? 1u : 0u;
        for (int i = 0; i < 3; ++i) {  // the world's key and up in this eye's view
            k.key[i] = static_cast<float>(rows[i][0] * Lw[0] + rows[i][1] * Lw[1] + rows[i][2] * Lw[2]);
            k.upv[i] = static_cast<float>(rows[i][0] * Uw[0] + rows[i][1] * Uw[1] + rows[i][2] * Uw[2]);
        }
        k.key[3] = 0.60f;
        k.upv[3] = 0.55f;
        k.probe[0] = g_probe_ref.load(std::memory_order_relaxed);
        k.probe[1] = g_probe_pull.load(std::memory_order_relaxed);
        k.probe[3] = probe_ok ? 1.0f : 0.0f;
        auto subject = [&](int s) { k.probe[2] = static_cast<float>(e * 4 + s); };
        auto crop = [&](const float* centre) {  // the capture's crop centre: the first round drawn in that eye
            if (e != g_grab_eye || crop_set) return;
            double v[3], x, y;
            to_view(c, centre, v);
            if (v[2] > -0.01) return;
            to_px(c, v, &x, &y);
            cx = static_cast<int>(x);
            cy = static_cast<int>(y);
            crop_set = true;
        };
        cl->SetGraphicsRootDescriptorTable(1, table(e));
        cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cl->OMSetRenderTargets(1, &rtv[e], FALSE, &dsv);
        // (the review) all of the held magazine's pixels: its model alone, the scene's depth not tested, nothing written to
        // the image, its own depth as the seen draw has it (cleared after); then the real draws
        cl->BeginQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 6 + e));
        if (meas_mi >= 0) {
            const MagDraw& md = mitems[meas_mi];
            cl->SetPipelineState(g_pso2c);
            place(md.pz, md.pz + 3, md.pz + 6, md.pz + 9, nullptr);
            const uint32_t occ_was = k.dfmt[1];
            k.dfmt[1] = 0u;
            cl->SetGraphicsRoot32BitConstants(0, kConsts, &k, 0);
            draw(g_magm[md.model]);
            k.dfmt[1] = occ_was;
            cl->SetPipelineState(newm ? g_pso2 : g_pso);
            cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
        cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 6 + e));
        // every query is ended each use (an empty one counts 0): the round's (and the show's), then the row's
        cl->BeginQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + e));
        if (round_ok) {
            subject(kSubHand);
            for (int r = 0; r < held_n; ++r) {  // the handful, from the pinch out
                const float Pr[3] = {P[0] + S[0] * hgap * r, P[1] + S[1] * hgap * r, P[2] + S[2] * hgap * r};
                if (r < 8 && skip_hand[r]) continue;  // the game's own shell is drawn there
                place(X, Y, Z, Pr, nullptr);
                crop(Pr);
                cl->SetGraphicsRoot32BitConstants(0, kConsts, &k, 0);
                draw(m);
            }
            {
                std::lock_guard lock(g_last_mutex);
                place(X, Y, Z, P, nullptr);
                std::memcpy(g_last_k[e], &k, sizeof(k));
            }
        }
        if (want_show) {
            subject(kSubShow);
            place(show.pz, show.pz + 3, show.pz + 6, show.pz + 9, nullptr);
            crop(show.pz + 9);
            cl->SetGraphicsRoot32BitConstants(0, kConsts, &k, 0);
            draw(sm);
        }
        cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + e));
        if (nm > 0 && !newm) cl->SetPipelineState(g_pso2);  // (RoundModels 0: the rounds the old, the magazines the new)
        cl->BeginQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 4 + e));  // (the held magazine's seen)
        bool seen_open = true;
        for (int i = 0; i < nm; ++i) {  // (2026-10-28) the magazines and the clip, and the rounds in them
            const MagDraw& md = mitems[i];
            const float *mX = md.pz, *mY = md.pz + 3, *mZ = md.pz + 6, *mP = md.pz + 9;
            subject(kSubHand);
            place(mX, mY, mZ, mP, nullptr);
            crop(mP);
            cl->SetGraphicsRoot32BitConstants(0, kConsts, &k, 0);
            if (i == meas_mi) {  // (its model's own pixels only: the query ends before its rounds)
                draw(g_magm[md.model]);
                cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 4 + e));
                seen_open = false;
            } else {
                if (seen_open) {  // (any drawn before the held one: outside its query)
                    cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 4 + e));
                    seen_open = false;
                }
                draw(g_magm[md.model]);
            }
            const int rk = kind_of(md.weapon);
            const Model& rmd = model_for(rk, audio::click_family(md.weapon), true);
            // a magazine's top round in its lips (its base at the back wall), or each of the clip's rounds in it; lying
            // along -z: the round's axes X, -Z, Y
            const int nrd = md.model == 2 ? std::min(md.rounds, kClipN) : md.rounds > 0 ? 1 : 0;
            const float nZ[3] = {-mZ[0], -mZ[1], -mZ[2]};
            for (int r = 0; r < nrd; ++r) {
                const float ly = md.model == 2 ? (kClipFoot + kClipGap * static_cast<float>(r)) * 0.001f : g_magm[md.model].len - rmd.rad - 0.0015f;
                const float lz = md.model == 2 ? kClipHead * 0.001f : 0.015f;
                float Pr[3];
                for (int q = 0; q < 3; ++q) Pr[q] = mP[q] + mY[q] * ly + mZ[q] * lz;
                place(mX, nZ, mY, Pr, nullptr);
                cl->SetGraphicsRoot32BitConstants(0, kConsts, &k, 0);
                draw(rmd);
            }
        }
        if (seen_open) cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 4 + e));
        if (nm > 0 && !newm) cl->SetPipelineState(g_pso);  // (back to the old for the row)
        cl->BeginQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 2 + e));
        if (nr > 0) {
            subject(kSubRow);
            for (int r = 0; r < nr; ++r) {
                if (skip_row[r]) continue;
                float rp[12];
                row_pose(row, r, nr, gap, rp);
                place(rp, rp + 3, rp + 6, rp + 9, nullptr);
                crop(rp + 9);
                cl->SetGraphicsRoot32BitConstants(0, kConsts, &k, 0);
                draw(rm);
            }
        }
        cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ + 2 + e));
    }
    cl->ResolveQueryData(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * kQ), kQ, g_qread, slot * kQ * sizeof(uint64_t));
    g_q_pending[slot] = true;
    g_q_mag[slot] = meas_mi >= 0;
    g_q_kind[slot] = held_mi >= 0 ? 1 : show_mi >= 0 ? 2 : 0;
    if (round_ok) g_drawn.fetch_add(1, std::memory_order_relaxed);
    if (want_row) g_row_drawn.fetch_add(1, std::memory_order_relaxed);
    if (nm > 0) {
        g_mag_drawn.fetch_add(1, std::memory_order_relaxed);
        g_mag_items.store(static_cast<uint64_t>(nm), std::memory_order_relaxed);
    }
}

namespace {
int parse_kind(const std::string& s) {
    for (int k = 0; k < kKinds; ++k)
        if (s == kDefs[k].name) return k;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return end && *end == 0 && v >= 0 && v < kKinds ? static_cast<int>(v) : -1;
}
}  // namespace

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v;
    in >> c;  // "round"
    while (in >> v) {
        if (v == "on" || v == "off") {
            set_enabled(v == "on");
        } else if (v == "mode" && (in >> c)) {  // mode 0|1|2 [session]: session: the ini not written (the tests')
            const int m = std::atoi(c.c_str());
            const std::streampos at = in.tellg();
            std::string x;
            if (in >> x && x == "session") {
                set_mode(m, false);
            } else {
                in.clear();
                in.seekg(at);
                set_mode(m);
            }
        } else if (v == "models" && (in >> c)) {  // models 0|1: [Reload] RoundModels for the session (the perf's and the A/B's)
            set_models(c == "1" || c == "new" || c == "on", false);
        } else if (v == "occlude" && (in >> c)) {  // occlude on|off: the game's depth hides the round (a control)
            g_occlude = c == "on";
        } else if (v == "ndc" && (in >> c)) {  // ndc on|off: the model enlarged at the image's centre (a pipeline test)
            g_ndc = c == "on";
        } else if (v == "probe") {  // probe [on|off] [ref <lum>] [pull <0-1>]: the scene light probe (the session); its texels
            std::string x;
            while (in >> x) {
                if (x == "on" || x == "off") {
                    g_probe_on = x == "on";
                } else if (x == "ref" && (in >> c)) {
                    const float r = static_cast<float>(std::atof(c.c_str()));
                    if (r > 0.001f && r < 10.0f) g_probe_ref = r;
                } else if (x == "pull" && (in >> c)) {
                    g_probe_pull = std::clamp(static_cast<float>(std::atof(c.c_str())), 0.0f, 1.0f);
                } else {
                    break;
                }
            }
            std::lock_guard lock(g_probe_mutex);
            char b[600];
            int o = std::snprintf(b, sizeof(b), "probe %s, ref %.3f, pull %.2f, probes %llu |", g_probe_on.load() ? "on" : "off", g_probe_ref.load(),
                                  g_probe_pull.load(), static_cast<unsigned long long>(g_probes.load()));
            static const char* const kSubName[4] = {"hand", "row", "show", "-"};
            for (int i = 0; i < kProbeTexels && o > 0 && o < static_cast<int>(sizeof(b)); ++i) {
                if ((i & 3) == 3) continue;
                o += std::snprintf(b + o, sizeof(b) - o, " %s %s lum %.4f rgb %.3f %.3f %.3f", i < 4 ? "L" : "R", kSubName[i & 3], g_probe_val[i][3],
                                   g_probe_val[i][0], g_probe_val[i][1], g_probe_val[i][2]);
            }
            return b;
        } else if (v == "project") {  // project x y z: a world point in each eye's image (px), the newest cameras (a test aid)
            float p3[3] = {0.0f, 0.0f, 0.0f};
            if (!(in >> p3[0] >> p3[1] >> p3[2])) return "usage: round project x y z";
            PassCam cm[2];
            bool okc = false;
            {
                std::lock_guard lock(g_cam_mutex);
                uint64_t best = ~0ull;
                int par = -1;
                for (int i = 0; i < kKeep; ++i) {
                    const uint64_t f = g_cam[i][0].frame;
                    if (f == ~0ull || g_cam[i][1].frame != f) continue;
                    if (best == ~0ull || f > best) best = f, par = i;
                }
                if (par >= 0) {
                    cm[0] = g_cam[par][0], cm[1] = g_cam[par][1];
                    okc = true;
                }
            }
            if (!okc) return "project: no cameras";
            std::string o = "project:";
            for (int e = 0; e < 2; ++e) {
                const double pd[3] = {p3[0] - cm[e].d[0], p3[1] - cm[e].d[1], p3[2] - cm[e].d[2]};
                const double* rows[3] = {cm[e].a, cm[e].b, cm[e].c};
                double vv[3];
                for (int i = 0; i < 3; ++i) vv[i] = rows[i][0] * pd[0] + rows[i][1] * pd[1] + rows[i][2] * pd[2];
                char b[120];
                if (vv[2] > -0.01) {
                    std::snprintf(b, sizeof(b), " %s behind", e ? "R" : "L");
                } else {  // (the image's own size as the last record drew it: g_w x g_h, the eye shape's content)
                    const double nx = (2 * vv[0] / -vv[2] - (cm[e].r + cm[e].l)) / (cm[e].r - cm[e].l);
                    const double ny = (2 * vv[1] / -vv[2] - (cm[e].u + cm[e].dn)) / (cm[e].u - cm[e].dn);
                    std::snprintf(b, sizeof(b), " %s (%.1f %.1f) of (%u %u)", e ? "R" : "L", (nx * 0.5 + 0.5) * g_iw_last.load(), (0.5 - ny * 0.5) * g_ih_last.load(),
                                  g_iw_last.load(), g_ih_last.load());
                }
                o += b;
            }
            return o;
        } else if (v == "maghold" && (in >> c)) {  // maghold old|new: the held magazine's hold (r44's control; the session)
            g_mag_hold_old = c == "old";
            return c == "old" ? "maghold old (the r219 hold: 1.5 cm behind the pinch)" : "maghold new (by its base at the fingertips)";
        } else if (v == "magplace") {  // magplace [x y z turn tilt] (2026-10-29, round 38): [Reload] MagazineOffset/Turn/Tilt for the session
            MagPlace p = mag_place();
            float x = 0, y = 0, z = 0, t = 0, l = 0;
            if (in >> x >> y >> z >> t >> l) {
                p.offset[0] = x, p.offset[1] = y, p.offset[2] = z, p.turn = t, p.tilt = l;
                set_mag_place(p, false);
                p = mag_place();
            }
            char b[128];
            std::snprintf(b, sizeof(b), "magplace offset %.3f %.3f %.3f turn %.0f tilt %.0f", p.offset[0], p.offset[1], p.offset[2], p.turn, p.tilt);
            return b;
        } else if (v == "showmag" && (in >> c)) {
            // showmag <hp|sa|clip|off> [rounds] [distance m] [height m] [side m] [spin deg] | showmag <kind> [rounds] at x y z
            // [spin deg] (2026-10-28, a test aid for the pictures): the magazine standing (y up) before John (as "round
            // show" places a round), or at a world point; its front (-z) to his facing turned by the spin
            if (c == "off") {
                std::lock_guard lock(g_mag_mutex);
                g_mshow.on = false;
                return "showmag off";
            }
            const int w = c == "hp" ? 2 : c == "sa" ? 1 : c == "clip" ? 3 : -1;
            if (w < 0) return "round showmag: hp, sa, clip or off";
            RdrvrActorState st{};
            if (!api::actor_state(&st)) return "round showmag: no actor";
            int rounds = 8;
            float dist = 0.4f, up_m = 1.45f, side = 0.0f, spin = 0.0f, at[3] = {0.0f, 0.0f, 0.0f};
            bool absolute = false;
            std::string x;
            if (in >> x) {
                rounds = std::atoi(x.c_str());
                if (in >> x) {
                    if (x == "at") {
                        absolute = static_cast<bool>(in >> at[0] >> at[1] >> at[2]);
                        in >> spin;
                    } else {
                        dist = static_cast<float>(std::atof(x.c_str()));
                        in >> up_m >> side >> spin;
                    }
                }
            }
            const float hh = st.heading_deg * 0.0174532925f, h2 = (st.heading_deg + spin) * 0.0174532925f;
            const float fw[3] = {-std::sin(hh), 0.0f, -std::cos(hh)}, rt[3] = {std::cos(hh), 0.0f, -std::sin(hh)};
            const float r2[3] = {std::cos(h2), 0.0f, -std::sin(h2)}, f2[3] = {-std::sin(h2), 0.0f, -std::cos(h2)};
            MShow s;
            s.on = true;
            s.weapon = w;
            s.rounds = rounds;
            for (int k = 0; k < 3; ++k) {
                s.pz[k] = r2[k];                          // x: his right (turned)
                s.pz[3 + k] = k == 1 ? 1.0f : 0.0f;       // y: up
                s.pz[6 + k] = -f2[k];                     // z: back (its front, -z, toward his facing)
                s.pz[9 + k] = absolute ? at[k] : st.pos[k] + fw[k] * dist + (k == 1 ? up_m : 0.0f) + rt[k] * side;
            }
            {
                std::lock_guard lock(g_mag_mutex);
                g_mshow = s;
            }
            char b[260];
            std::snprintf(b, sizeof(b), "showmag %s at (%.3f %.3f %.3f), rounds %d, heading %.1f, right (%.3f %.3f %.3f) forward (%.3f %.3f %.3f)", c.c_str(),
                          s.pz[9], s.pz[10], s.pz[11], rounds, st.heading_deg, rt[0], rt[1], rt[2], fw[0], fw[1], fw[2]);
            return b;
        } else if (v == "show" && (in >> c)) {
            // show <kind|off> [distance m] [height m] [side m] [spin deg] [tilt deg] (2026-10-27, a test aid): one round of
            // that kind (a name: 44-40 henry evans 38acp 45acp mauser volcanic rifle 45-70 shell paper, or its number) at
            // the place "props show" puts the census prop (before John, his heading, level; X right, Y forward, Z up,
            // turned by the spin about up and the tilt about right: tilt 90 stands it), moved `side` along his right
            if (c == "off") {
                std::lock_guard lock(g_show_mutex);
                g_show.on = false;
                g_show_on = false;
                return "show off";
            }
            const int kind = parse_kind(c);
            if (kind < 0) return "round show: no such kind (44-40 henry evans 38acp 45acp mauser volcanic rifle 45-70 shell paper)";
            RdrvrActorState st{};
            if (!api::actor_state(&st)) return "round show: no actor";
            float dist = 1.2f, up_m = 1.1f, side = 0.0f, spin = 0.0f, tilt = 0.0f;
            in >> dist >> up_m >> side >> spin >> tilt;  // (any left out keeps its default)
            const float hh = st.heading_deg * 0.0174532925f, h2 = (st.heading_deg + spin) * 0.0174532925f, tr = tilt * 0.0174532925f;
            const float fw[3] = {-std::sin(hh), 0.0f, -std::cos(hh)}, rt[3] = {std::cos(hh), 0.0f, -std::sin(hh)};
            const float r2[3] = {std::cos(h2), 0.0f, -std::sin(h2)}, f2[3] = {-std::sin(h2), 0.0f, -std::cos(h2)};
            Show s;
            s.on = true;
            s.kind = kind;
            for (int k = 0; k < 3; ++k) {
                const float upk = k == 1 ? 1.0f : 0.0f;
                s.pz[k] = r2[k];
                s.pz[3 + k] = f2[k] * std::cos(tr) + upk * std::sin(tr);
                s.pz[6 + k] = upk * std::cos(tr) - f2[k] * std::sin(tr);
                s.pz[9 + k] = st.pos[k] + fw[k] * dist + upk * up_m + rt[k] * side;
            }
            {
                std::lock_guard lock(g_show_mutex);
                g_show = s;
            }
            g_show_on = true;
            char b[200];
            std::snprintf(b, sizeof(b), "show %s at (%.3f %.3f %.3f), spin %.0f tilt %.0f, models %s", kDefs[kind].name, s.pz[9], s.pz[10], s.pz[11], spin, tilt,
                          g_models.load() ? "new" : "old");
            return b;
        } else if (v == "atlas" && (in >> c)) {  // atlas <path.bmp>: the generated atlas (mip 0, its colours) for review
            std::string res;
            if (g_atlas[0].empty()) return "no atlas";
            if (!write_bmp(c, kAtlasW, g_atlas_h, g_atlas[0].data(), kAtlasW * 4, false, &res)) return res;
            char b[300];
            std::snprintf(b, sizeof(b), "wrote %s: %dx%d (alpha, the gloss, not written)", c.c_str(), kAtlasW, g_atlas_h);
            return b;
        } else if (v == "mags") {  // mags: the magazines' (2026-10-28, [Reload] PistolMagazines) state, for the tests
            const double tnow = log::now_ms();
            std::lock_guard lock(g_mag_mutex);
            int falling = 0;
            for (const Drop& x : g_drop) falling += tnow - x.t0 < kDropMs ? 1 : 0;
            char b[900];
            std::snprintf(b, sizeof(b),
                          "mags: held %d (hand %d, weapon %d, rounds %d) | falling %d (made %llu) | clip seated %d (rounds %d) | drawn %llu (items %llu) | "
                          "models: hp %u sa %u clip %u triangles, len %.3f %.3f %.3f m | projected L %.0f R %.0f px | showmag %d | mag row %d of weapon %d (%d rounds), drawn %llu | "
                          "held seen L %llu of %llu R %llu of %llu px (of the %s) | hold %s | measured %s | axis to view L %.0f R %.0f deg | grip axis %d (%s) | "
                          "the fingertips %.3f m along it",
                          tnow - g_mag_ms < 250.0 ? 1 : 0, g_mag_hand, g_mag_weapon, g_mag_rounds, falling, static_cast<unsigned long long>(g_drops_made.load()),
                          tnow - g_seat.ms < 250.0 ? 1 : 0, g_seat.rounds, static_cast<unsigned long long>(g_mag_drawn.load()),
                          static_cast<unsigned long long>(g_mag_items.load()), g_magm[0].count / 3, g_magm[1].count / 3, g_magm[2].count / 3, g_magm[0].len,
                          g_magm[1].len, g_magm[2].len, g_mag_proj[0].load(), g_mag_proj[1].load(), g_mshow.on ? 1 : 0,
                          tnow - g_mrow.ms < 250.0 ? g_mrow.n : 0, g_mrow.weapon, g_mrow.rounds, static_cast<unsigned long long>(g_mrow_drawn.load()),
                          static_cast<unsigned long long>(g_mag_seen[0].load()), static_cast<unsigned long long>(g_mag_all[0].load()),
                          static_cast<unsigned long long>(g_mag_seen[1].load()), static_cast<unsigned long long>(g_mag_all[1].load()),
                          g_mag_seen_kind.load() == 1 ? "held" : g_mag_seen_kind.load() == 2 ? "shown" : "none",
                          g_mag_hold_old.load() ? "old" : "new", g_mag_measured.load() == 1 ? "held" : g_mag_measured.load() == 2 ? "shown" : "none",
                          g_mag_view_deg[0].load(), g_mag_view_deg[1].load(), g_mag_grip_axis.load(),
                          g_mag_grip_axis.load() == 1 ? "the drawn gun's frame" : g_mag_grip_axis.load() == 2 ? "the gun hand's target" : g_mag_axis_why.load(),
                          g_mag_cover.load());
            return b;
        } else if (v == "kinds") {  // kinds: each kind's model (triangles, vertices, size) and its rects in the atlas
            std::string o = "kinds:";
            char b[260];
            for (int k = 0; k < kKinds; ++k) {
                const Model& md = g_kind[k];
                const int verts = (k + 1 < kKinds ? g_kind[k + 1].base : static_cast<int32_t>(g_mesh.size())) - md.base;
                const KindTex& t = g_kt[k];
                std::snprintf(b, sizeof(b), " | %s: %u triangles, %d vertices, %.1f x %.1f mm, side %dx%d (%.1f texels/cm round, %.1f along)",
                              kDefs[k].name, md.count / 3, verts, md.len * 1000.0f, md.rad * 2000.0f, t.side.w, t.side.h,
                              t.side.w / (6.2831853f * md.rad * 100.0f), t.arc > 0 ? t.side.h / (t.arc * 0.1f) : 0.0f);
                o += b;
            }
            return o;
        } else if (v == "consts") {  // the last constants of each eye
            std::lock_guard lock(g_last_mutex);
            std::string o;
            char b[96];
            for (int e = 0; e < 2; ++e) {
                o += e ? " | R" : "L";
                for (int i = 0; i < 24; ++i) {
                    std::snprintf(b, sizeof(b), " %.4g", g_last_k[e][i]);
                    o += b;
                }
            }
            return o;
        } else if ((v == "offset" || v == "axis") && (in >> c)) {  // offset x y z | axis x y z | axis auto
            std::lock_guard lock(g_cfg_mutex);
            if (v == "axis" && c == "auto") {
                g_axis_auto = true;
                continue;
            }
            float y = 0, z = 0;
            if (!(in >> y >> z)) return "usage: round offset|axis x y z (or axis auto)";
            float* t = v == "offset" ? g_offset : g_axis;  // ("round offsetr|axisr": the right hand's, below)
            t[0] = static_cast<float>(std::atof(c.c_str()));
            t[1] = y;
            t[2] = z;
            if (v == "axis") {
                const float al = std::sqrt(g_axis[0] * g_axis[0] + g_axis[1] * g_axis[1] + g_axis[2] * g_axis[2]);
                g_axis_auto = !(al > 1e-3f) || !std::isfinite(al);  // (the review: a zero axis: auto, not a collapsed model)
                if (g_axis_auto) g_axis[0] = 1.0f, g_axis[1] = 0.0f, g_axis[2] = 0.0f;
                normalize3(g_axis);
            }
        } else if ((v == "offsetr" || v == "axisr") && (in >> c)) {  // (R13, a test's aid) the right hand's own, for the session
            float y = 0, z = 0;
            if (!(in >> y >> z)) return "usage: round offsetr|axisr x y z";
            std::lock_guard lock(g_cfg_mutex);
            if (!g_r_round_own) {  // (from the left's, then this one changed)
                std::memcpy(g_r_offset, g_offset, sizeof(g_r_offset));
                std::memcpy(g_r_axis, g_axis, sizeof(g_r_axis));
                g_r_axis_auto = g_axis_auto;
                g_r_round_own = true;
            }
            float* t = v == "offsetr" ? g_r_offset : g_r_axis;
            t[0] = static_cast<float>(std::atof(c.c_str()));
            t[1] = y;
            t[2] = z;
            if (v == "axisr") {
                g_r_axis_auto = false;
                normalize3(g_r_axis);
            }
        } else if (v == "bright" && (in >> c)) {
            std::lock_guard lock(g_cfg_mutex);
            g_bright = static_cast<float>(std::atof(c.c_str()));
        } else if (v == "row") {  // row [on|off]: [Reload] ShowAmmo for the session; the row's state and its pixels
            std::string x;
            if (in >> x) set_show_ammo(x == "on", false);
            Row r;
            {
                std::lock_guard lock(g_row_mutex);
                r = g_row;
            }
            char b[300];
            std::snprintf(b, sizeof(b), "row %d: family %d, %d rounds, set %.0f ms ago, drawn %llu, pixels L %llu R %llu, kind %s", g_row_on.load() ? 1 : 0,
                          r.weapon >= 0 ? audio::click_family(r.weapon) : 0, r.n, r.ms > 0 ? log::now_ms() - r.ms : -1.0,
                          static_cast<unsigned long long>(g_row_drawn.load()), static_cast<unsigned long long>(g_row_px[0].load()),
                          static_cast<unsigned long long>(g_row_px[1].load()), kind_name(kind_of(r.weapon)));
            return b;
        } else if (v == "fan" && (in >> c)) {  // fan 1|-1: the handful's side of the pinch (a test aid; the session only)
            g_fan_sign = std::atof(c.c_str()) < 0.0f ? -1.0f : 1.0f;
        } else if (v == "held" && (in >> c)) {  // held <john hand> [weapon] [count]: rounds in that hand, as the holsters say (a test aid)
            int wp = 4, cnt = 1;
            std::string fs, ns;
            if (in >> fs) wp = std::atoi(fs.c_str());
            if (in >> ns) cnt = std::atoi(ns.c_str());
            set_held(std::atoi(c.c_str()), wp, cnt);
        } else if (v == "grab" && (in >> c)) {  // grab <path.bmp> [eye]: the next drawn eye image, cropped around the round
            std::string es;
            const int eye = (in >> es) ? std::atoi(es.c_str()) & 1 : 0;
            std::string fs;
            g_grab_full = (in >> fs) && fs == "full";  // grab <path> <eye> full: the whole image, not the crop round the round
            {
                std::lock_guard lock(g_grab_mutex);
                g_grab_path = c;
                g_grab_result.clear();
            }
            g_grab_eye = eye;
            ResetEvent(g_grab_done);
            g_grab_phase.store(1, std::memory_order_release);
            if (WaitForSingleObject(g_grab_done, 4000) != WAIT_OBJECT_0) {
                g_grab_phase.store(0, std::memory_order_release);
                return "NOT MEASURED: no round drawn within 4 s (held, both cameras and depths?)";
            }
            g_grab_phase.store(0, std::memory_order_release);
            std::lock_guard lock(g_grab_mutex);
            return g_grab_result;
        }
    }
    char st[1100];
    status_text(st, sizeof(st));
    return st;
}

void status_text(char* out, size_t len) {
    DepthMeta dm;
    {
        std::lock_guard lock(g_depth_mutex);
        dm = g_dmeta;
    }
    float o[3], a[3];
    {
        std::lock_guard lock(g_cfg_mutex);
        std::memcpy(o, g_offset, sizeof(o));
        std::memcpy(a, g_axis, sizeof(a));
    }
    const int w = g_weapon.load();
    std::snprintf(out, len,
                  "round %s (mode %d), held %d (hand %d, family %d), pipeline %s, records %llu, drawn %llu, no cameras %llu, no depth %llu, "
                  "pixels L %llu R %llu (frames counted %llu, seen L %llu R %llu), depth taps %llu (frames %llu, most a frame %d, extra %llu), "
                  "depth %ux%u pitch %u format %u, occlude %d, axes from %d, offset %.3f %.3f %.3f, axis %.2f %.2f %.2f, threads pass %u tap %u record %u, "
                  "models %s, kind %s (weapon %d, count %d), the game's shells now: hand %d of %d, row %d of %d (in all covered %llu, stand-ins %llu), "
                  "atlas %d, probes %llu, show %d, out %.1f mm, path %s (pinch %llu, flat %llu, not finite %llu)",
                  g_on.load() ? "on" : "off", g_mode.load(), held_now() ? 1 : 0, g_hand.load(), w >= 0 ? audio::click_family(w) : 0,
                  g_ready ? "made" : g_failed ? "failed" : "not made", static_cast<unsigned long long>(g_records.load()),
                  static_cast<unsigned long long>(g_drawn.load()), static_cast<unsigned long long>(g_no_cam.load()),
                  static_cast<unsigned long long>(g_no_depth.load()), static_cast<unsigned long long>(g_px[0].load()),
                  static_cast<unsigned long long>(g_px[1].load()), static_cast<unsigned long long>(g_px_frames.load()),
                  static_cast<unsigned long long>(g_px_seen[0].load()), static_cast<unsigned long long>(g_px_seen[1].load()),
                  static_cast<unsigned long long>(g_taps.load()), static_cast<unsigned long long>(g_tap_frames.load()), g_tap_max.load(),
                  static_cast<unsigned long long>(g_tap_extra.load()), dm.w, dm.h, dm.pitch, dm.fmt, g_occlude.load() ? 1 : 0, g_axes_from.load(), o[0], o[1], o[2],
                  a[0], a[1], a[2], g_pass_tid.load(), g_tap_tid.load(), g_rec_tid.load(), g_models.load() ? "new" : "old", kind_name(kind_of(w)), w,
                  g_count.load(), g_hand_cover.load(), g_hand_want.load(), g_row_cover.load(), g_row_want.load(),
                  static_cast<unsigned long long>(g_covered.load()), static_cast<unsigned long long>(g_stand_ins.load()), g_atlas_state,
                  static_cast<unsigned long long>(g_probes.load()), g_show_on.load() ? 1 : 0, g_out_mm.load(),
                  g_path.load() == 2 ? "flat" : g_path.load() == 1 ? "pinch" : "wrist", static_cast<unsigned long long>(g_path_pinch.load()),
                  static_cast<unsigned long long>(g_path_flat.load()), static_cast<unsigned long long>(g_path_bad.load()));
}

}  // namespace rdrvr::round_draw
