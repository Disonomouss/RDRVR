#include "core/round_draw.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include "core/body.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/state.h"

namespace rdrvr::round_draw {
namespace {

// ---- the models: lathe profiles (radius, height along the axis, mm; the material), base at 0, tip up
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

struct Vtx {
    float p[4];  // x y z (m), w the material
    float n[3];
};
struct Model {
    uint32_t first = 0, count = 0;
};
Model g_model[3];  // handgun, rifle, shotgun
std::vector<Vtx> g_mesh;

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
                Vtx x{{r * c * 0.001f, y * 0.001f, r * sn * 0.001f, static_cast<float>(a.mat)}, {nr * c, ny, nr * sn}};
                g_mesh.push_back(x);
            };
            v(a.r, a.y, c0, s0);
            v(b.r, b.y, c0, s0);
            v(b.r, b.y, c1, s1);
            v(a.r, a.y, c0, s0);
            v(b.r, b.y, c1, s1);
            v(a.r, a.y, c1, s1);
        }
    }
    out->count = static_cast<uint32_t>(g_mesh.size()) - out->first;
}

// The vertex shader carries the model into the eye's view (L: rows of the model-to-view rotation, w the view-space
// translation) and projects it as the game's Perspective does (row-vector, right-handed, standard Z): x' = (2x + (r+l)z)
// / (r-l), y' likewise, z' = P22 z + P32, w = -z. The pixel shader drops what lies behind the game's own depth there.
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
constexpr int kConsts = 32;  // dwords
struct Consts {
    float L[3][4];
    float proj[4];
    float zc[4];
    float scale[4];
    uint32_t dinfo[4];
    uint32_t dfmt[4];
};
static_assert(sizeof(Consts) == kConsts * 4, "root constants");

constexpr int kSlots = 4;  // >= xr_blit's lists
constexpr int kKeep = 2;   // depth slots (frame parity)

// ---- settings
std::atomic<bool> g_on{false};
std::atomic<int> g_mode{0};  // [Reload] RoundInHand: 0 off, 1 the mod's round, 2 also the game's own shell for shotguns
std::mutex g_cfg_mutex;
float g_offset[3] = {0.0f, 0.0f, 0.0f};  // moved from the pinch in the hand's IK target axes (the wrist bone's), m
float g_axis[3] = {1.0f, 0.0f, 0.0f};    // the round's base-to-tip in those axes (x: along the fingers), when not "auto"
bool g_axis_auto = false;                // auto: out of the hand, from the wrist through the pinch
float g_bright = 1.0f, g_bias = 0.004f;
std::atomic<bool> g_occlude{true}, g_ndc{false};
std::atomic<int> g_axes_from{0};  // the round's axes: 1 the IK target's, 2 the drawn wrist's, 0 neither valid
std::mutex g_last_mutex;
float g_last_k[2][32];  // the last constants per eye (a test aid)

// ---- the holder
std::atomic<int> g_hand{-1}, g_family{0};
std::atomic<double> g_held_ms{0};

bool held_now() {
    return g_hand.load(std::memory_order_relaxed) >= 0 && log::now_ms() - g_held_ms.load(std::memory_order_relaxed) < 250.0;
}

// ---- the ammo row ([Reload] ShowAmmo, run 6 item 6b)
std::atomic<bool> g_row_on{false};
struct Row {
    int family = 0, n = 0;
    float at[3] = {}, rgt[3] = {}, up[3] = {}, fwd[3] = {};
    double ms = 0;
};
std::mutex g_row_mutex;
Row g_row;
std::atomic<uint64_t> g_row_px[2] = {0, 0}, g_row_drawn{0};
bool row_now() {  // a row wanted within the last 250 ms
    if (!g_row_on.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_row_mutex);
    return g_row.n > 0 && g_row.family > 0 && log::now_ms() - g_row.ms < 250.0;
}

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
std::atomic<const void*> g_resolve{nullptr};
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
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_srv_heap = nullptr;  // kSlots SRVs of the depth buffer (each written when its slot is free)
ID3D12DescriptorHeap* g_dsv_heap = nullptr;
ID3D12Resource* g_vb = nullptr;
ID3D12Resource* g_zbuf = nullptr;  // the round's own depth (it hides its own far side)
ID3D12QueryHeap* g_qheap = nullptr;
ID3D12Resource* g_qread = nullptr;  // kSlots x 2 occlusion counts
UINT g_srv_step = 0;
uint32_t g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
bool g_ready = false, g_failed = false;
bool g_q_pending[kSlots] = {};
std::atomic<uint64_t> g_drawn{0}, g_no_cam{0}, g_no_depth{0}, g_records{0};
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

using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                   ID3DBlob**, ID3DBlob**);

ID3DBlob* compile(Compile_t fn, const char* entry, const char* target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = fn(kShader, sizeof(kShader) - 1, "rdrvr_round", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) log::error("[round] %s does not compile: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    if (err) err->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

bool make(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    g_dev = dev;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!fn) return false;
    ID3DBlob* vs = compile(fn, "vs", "vs_5_0");
    ID3DBlob* ps = compile(fn, "ps", "ps_5_0");
    bool ok = vs && ps;
    if (ok) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, kConsts};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ID3DBlob *sig = nullptr, *err = nullptr;
        ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) &&
             SUCCEEDED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root)));
        if (sig) sig->Release();
        if (err) err->Release();
    }
    if (ok) {
        const D3D12_INPUT_ELEMENT_DESC il[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_root;
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.InputLayout = {il, 2};
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
        ok = SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso)));
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (!ok) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srv_heap))) || FAILED(dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&g_dsv_heap))))
        return false;
    g_srv_step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // the meshes, in an upload buffer read in place (a few thousand vertices)
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = g_mesh.size() * sizeof(Vtx);
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_vb))))
        return false;
    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(g_vb->Map(0, &none, &p)) || !p) return false;
    std::memcpy(p, g_mesh.data(), g_mesh.size() * sizeof(Vtx));
    g_vb->Unmap(0, nullptr);
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
    // the occlusion counts
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kSlots * 4, 0};  // per slot: the round's two eyes, the row's
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_qheap)))) return false;
    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    bd.Width = kSlots * 4 * sizeof(uint64_t);
    if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_qread))))
        return false;
    g_w = w;
    g_h = h;
    g_fmt = fmt;
    return true;
}

// The game's copy into "Depth Resolve", in its own list: this frame's next eye's scene depth into our buffer.
void copy_tap(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    if (dst != g_resolve.load(std::memory_order_relaxed) || !((g_on.load(std::memory_order_relaxed) && held_now()) || row_now())) return;
    const uint64_t frame = state::presents.load(std::memory_order_relaxed);
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

void write_grab();
}  // namespace

bool pose(const body::BodyPoints& bp, int hand, float* out) {
    if (hand < 0 || hand > 1 || !bp.hand_ok[hand]) return false;
    // the round in the world: the hand's IK target (where the drawn wrist goes, its axes) and the offset in them
    float off[3], ax[3];
    {
        std::lock_guard lock(g_cfg_mutex);
        std::memcpy(off, g_offset, sizeof(off));
        std::memcpy(ax, g_axis, sizeof(ax));
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
        axis_auto = g_axis_auto;
    }
    to_world(off, P);
    if (bp.tips_ok[hand]) {  // pinched between the drawn thumb and index tips, its base just inside, pointing out
        float pinch[3];
        for (int k = 0; k < 3; ++k) pinch[k] = 0.5f * (bp.thumb_tip[hand][k] + bp.index_tip[hand][k]);
        if (axis_auto) {
            for (int k = 0; k < 3; ++k) Y[k] = pinch[k] - bp.hand[hand][k];
        } else {
            to_world(ax, Y);
        }
        normalize3(Y);
        for (int k = 0; k < 3; ++k) P[k] += pinch[k] - 0.006f * Y[k];
        g_axes_from.store(g_axes_from.load(std::memory_order_relaxed) + 10, std::memory_order_relaxed);
    } else {
        for (int k = 0; k < 3; ++k) P[k] += bp.hand[hand][k];
        to_world(ax, Y);
        normalize3(Y);
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
        const int row = (cw * 3 + 3) & ~3;
        std::vector<uint8_t> out(54 + static_cast<size_t>(row) * ch, 0);
        uint8_t* hdr = out.data();
        auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
        hdr[0] = 'B';
        hdr[1] = 'M';
        put32(2, static_cast<uint32_t>(out.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, static_cast<uint32_t>(cw));
        put32(22, static_cast<uint32_t>(ch));
        hdr[26] = 1;
        hdr[28] = 24;
        for (int y = 0; y < ch; ++y) {
            const uint8_t* src = p + g_grab_fp.Offset + static_cast<uint64_t>(y0 + y) * g_grab_fp.Footprint.RowPitch + static_cast<uint64_t>(x0) * 4;
            uint8_t* dst = out.data() + 54 + static_cast<size_t>(ch - 1 - y) * row;
            for (int x = 0; x < cw; ++x) {
                dst[x * 3 + 0] = bgr ? src[x * 4 + 0] : src[x * 4 + 2];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = bgr ? src[x * 4 + 2] : src[x * 4 + 0];
            }
        }
        g_grab_rb->Unmap(0, nullptr);
        std::wstring wpath(g_grab_path.begin(), g_grab_path.end());
        FILE* f = nullptr;
        _wfopen_s(&f, wpath.c_str(), L"wb");
        if (!f) {
            res = "ERROR cannot open " + g_grab_path;
        } else {
            const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
            fclose(f);
            char buf[400];
            std::snprintf(buf, sizeof(buf), "%s %s: eye %d, %dx%d at (%d, %d) of %dx%d, the round at (%d, %d)", ok ? "wrote" : "ERROR writing",
                          g_grab_path.c_str(), g_grab_eye, cw, ch, x0, y0, W, H, g_grab_cx, g_grab_cy);
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

}  // namespace

void init() {
    lathe(kHandgun, static_cast<int>(sizeof(kHandgun) / sizeof(kHandgun[0])), &g_model[0]);
    lathe(kRifle, static_cast<int>(sizeof(kRifle) / sizeof(kRifle[0])), &g_model[1]);
    lathe(kShell, static_cast<int>(sizeof(kShell) / sizeof(kShell[0])), &g_model[2]);
    g_grab_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        const int m = config::get_int("Reload", "RoundInHand", 0);
        g_mode = m < 0 ? 0 : m > 2 ? 2 : m;
        g_on = g_mode.load() > 0;
    }
    {
        std::lock_guard lock(g_cfg_mutex);
        sscanf_s(config::get_string("Reload", "RoundOffset", "0 0 0").c_str(), "%f %f %f", &g_offset[0], &g_offset[1], &g_offset[2]);
        const std::string ax = config::get_string("Reload", "RoundAxis", "1 0 0");  // along the fingers (the wrist's x)
        g_axis_auto = ax == "auto" || sscanf_s(ax.c_str(), "%f %f %f", &g_axis[0], &g_axis[1], &g_axis[2]) != 3;
        normalize3(g_axis);
        g_bright = config::get_float("Reload", "RoundBrightness", 1.0f);
    }
    g_row_on = config::get_bool("Reload", "ShowAmmo", false);
    d3d::set_copy_tap(copy_tap);
    log::info("[round] the round in hand %s (models: %u, %u, %u vertices)", g_on.load() ? "on" : "off", g_model[0].count, g_model[1].count,
              g_model[2].count);
}

bool enabled() { return g_on.load(std::memory_order_relaxed); }
bool show_ammo() { return g_row_on.load(std::memory_order_relaxed); }
void set_show_ammo(bool on, bool save) {
    if (g_row_on.exchange(on) != on) log::info("[round] the rounds shown at the chest: %s", on ? "on" : "off");
    if (save) config::set("Reload", "ShowAmmo", on ? "1" : "0");
}
void set_row(int family, int n, const float at[3], const float rgt[3], const float up[3], const float fwd[3]) {
    std::lock_guard lock(g_row_mutex);
    g_row.family = family;
    g_row.n = n;
    if (family > 0 && n > 0) {
        std::memcpy(g_row.at, at, sizeof(g_row.at));
        std::memcpy(g_row.rgt, rgt, sizeof(g_row.rgt));
        std::memcpy(g_row.up, up, sizeof(g_row.up));
        std::memcpy(g_row.fwd, fwd, sizeof(g_row.fwd));
    }
    g_row.ms = log::now_ms();
}
int mode() { return g_mode.load(std::memory_order_relaxed); }
Place place() {
    std::lock_guard lock(g_cfg_mutex);
    Place p;
    std::memcpy(p.offset, g_offset, sizeof(p.offset));
    std::memcpy(p.axis, g_axis, sizeof(p.axis));
    const bool fingers = std::fabs(g_axis[0] - 1.0f) < 1e-3f && std::fabs(g_axis[1]) < 1e-3f && std::fabs(g_axis[2]) < 1e-3f;
    p.axis_mode = g_axis_auto ? 1 : fingers ? 0 : 2;
    p.bright = g_bright;
    return p;
}
void set_place(const Place& p, bool save) {
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

void set_mode(int m) {
    m = m < 0 ? 0 : m > 2 ? 2 : m;
    if (g_mode.exchange(m) != m) log::info("[round] the round in hand: mode %d", m);
    g_on = m > 0;
    config::set("Reload", "RoundInHand", std::to_string(m));
}

void set_enabled(bool on) { set_mode(on ? (g_mode.load() ? g_mode.load() : 1) : 0); }

void note_pass(int eye, const float* cam, float l, float r, float u, float d) {
    if (!(g_on.load(std::memory_order_relaxed) || g_row_on.load(std::memory_order_relaxed)) || eye < 0 || eye > 1 || !cam) return;
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

void set_held(int john_hand, int family) {
    g_hand.store(john_hand, std::memory_order_relaxed);
    g_family.store(family, std::memory_order_relaxed);
    g_held_ms.store(log::now_ms(), std::memory_order_relaxed);
}

void record(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* const dst[2], const D3D12_CPU_DESCRIPTOR_HANDLE rtv[2],
            uint32_t w, uint32_t h, DXGI_FORMAT fmt, int slot) {
    if (slot < 0 || slot >= kSlots) return;
    g_records.fetch_add(1, std::memory_order_relaxed);
    g_rec_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_ready && g_q_pending[slot]) {  // this slot's counts from its last use (the GPU has passed it)
        uint64_t* q = nullptr;
        D3D12_RANGE r{slot * 4 * sizeof(uint64_t), (slot * 4 + 4) * sizeof(uint64_t)};
        if (SUCCEEDED(g_qread->Map(0, &r, reinterpret_cast<void**>(&q))) && q) {
            g_px[0] = q[slot * 4];
            g_px[1] = q[slot * 4 + 1];
            g_row_px[0] = q[slot * 4 + 2];
            g_row_px[1] = q[slot * 4 + 3];
            for (int e = 0; e < 2; ++e)
                if (q[slot * 4 + e]) g_px_seen[e].fetch_add(1, std::memory_order_relaxed);
            D3D12_RANGE none{0, 0};
            g_qread->Unmap(0, &none);
            g_px_frames.fetch_add(1, std::memory_order_relaxed);
        }
        g_q_pending[slot] = false;
    }
    if (g_grab_phase.load(std::memory_order_acquire) == 2 && g_grab_slot == slot) write_grab();
    if (!g_on.load(std::memory_order_relaxed) && !g_row_on.load(std::memory_order_relaxed)) return;
    const uint64_t frame = state::presents.load(std::memory_order_relaxed);
    static uint64_t s_last_find = 0;
    if (!g_resolve.load(std::memory_order_relaxed) || frame - s_last_find > 120) {  // the target, again now and then (a resize remakes it)
        s_last_find = frame;
        g_resolve.store(d3d::find_resource("Depth Resolve"), std::memory_order_relaxed);
    }
    if (!g_ready && !g_failed) {
        g_ready = make(dev, w, h, fmt);
        g_failed = !g_ready;
        log::info("[round] the round's pipeline %s (%ux%u, format %d)", g_ready ? "made" : "NOT made", w, h, static_cast<int>(fmt));
    }
    if (!g_ready || w != g_w || h != g_h || fmt != g_fmt) return;
    int cx = static_cast<int>(w / 2), cy = static_cast<int>(h / 2);  // the capture's crop centre (the round's, when drawn)
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
    const bool want_row = g_row_on.load(std::memory_order_relaxed) && row.n > 0 && row.family > 0 && log::now_ms() - row.ms < 250.0;
    if (!want_row) g_row_px[0] = g_row_px[1] = 0;  // no row drawn: none of it seen (its last count would stay)
    if (!want_round && !want_row) return;
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
    if (!buf || (occlude && (!have_depth || !dm.w))) {  // never drawn without the buffer bound (the shader names it)
        g_no_depth.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float pz[12] = {}, bright, bias;
    bool round_ok = want_round && pose(bp, hand, pz);
    const float *X = pz, *Y = pz + 3, *Z = pz + 6, *P = pz + 9;
    {
        std::lock_guard lock(g_cfg_mutex);
        bright = g_bright;
        bias = g_bias;
    }
    const int fam = g_family.load(std::memory_order_relaxed);
    if (fam == 3 && g_mode.load(std::memory_order_relaxed) == 2) round_ok = false;  // the game's own shell is held (held_prop)
    if (!round_ok && !want_row) return;
    const Model& m = g_model[fam == 2 ? 1 : fam == 3 ? 2 : 0];
    const Model& rm = g_model[row.family == 2 ? 1 : row.family == 3 ? 2 : 0];
    // the row: standing (base to tip up), side by side along the body's right, centred on the zone; a shell's width
    // apart, a rifle round's, a handgun round's
    const float gap = row.family == 3 ? 0.026f : row.family == 2 ? 0.017f : 0.016f;
    float rz[3];  // X x Y: the right-handed third axis (the body's back)
    rz[0] = row.rgt[1] * row.up[2] - row.rgt[2] * row.up[1];
    rz[1] = row.rgt[2] * row.up[0] - row.rgt[0] * row.up[2];
    rz[2] = row.rgt[0] * row.up[1] - row.rgt[1] * row.up[0];
    float n = 0.1f, f = 7500.0f;
    camera_lever::scene_clip(&n, &f);
    if (!(n > 0.001f) || !(f > n)) n = 0.1f, f = 7500.0f;
    if (buf) {  // this slot's view of the depth buffer
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.NumElements = static_cast<UINT>(g_dbuf_size.load(std::memory_order_relaxed) / 4);
        sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE hc = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        hc.ptr += static_cast<SIZE_T>(slot) * g_srv_step;
        dev->CreateShaderResourceView(buf, &sv, hc);
    }
    cl->SetGraphicsRootSignature(g_root);
    cl->SetDescriptorHeaps(1, &g_srv_heap);
    cl->SetPipelineState(g_pso);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv{g_vb->GetGPUVirtualAddress(), static_cast<UINT>(g_mesh.size() * sizeof(Vtx)), sizeof(Vtx)};
    cl->IASetVertexBuffers(0, 1, &vbv);
    D3D12_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    D3D12_GPU_DESCRIPTOR_HANDLE hg = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
    hg.ptr += static_cast<UINT64>(slot) * g_srv_step;
    if (buf) cl->SetGraphicsRootDescriptorTable(1, hg);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    const int gpar = static_cast<int>(use % kKeep);
    for (int e = 0; e < 2; ++e) {
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
        double vz = 0;
        place(X, Y, Z, P, &vz);
        k.proj[0] = 2.0f / (c.r - c.l);
        k.proj[1] = (c.r + c.l) / (c.r - c.l);
        k.proj[2] = 2.0f / (c.u - c.dn);
        k.proj[3] = (c.u + c.dn) / (c.u - c.dn);
        k.zc[0] = f / (n - f);
        k.zc[1] = k.zc[0] * n;
        k.zc[2] = n;
        k.zc[3] = f;
        k.scale[0] = dm.w ? static_cast<float>(dm.w) / static_cast<float>(w) : 1.0f;
        k.scale[1] = dm.h ? static_cast<float>(dm.h) / static_cast<float>(h) : 1.0f;
        k.scale[2] = bright;
        k.scale[3] = bias;
        k.dinfo[0] = static_cast<uint32_t>(dm.stride * static_cast<uint64_t>(gpar * 2 + e));
        k.dinfo[1] = dm.pitch;
        k.dinfo[2] = dm.w;
        k.dinfo[3] = dm.h;
        k.dfmt[0] = dm.fmt == DXGI_FORMAT_R32_TYPELESS || dm.fmt == DXGI_FORMAT_D32_FLOAT || dm.fmt == DXGI_FORMAT_R32_FLOAT ||
                            dm.fmt == DXGI_FORMAT_R32G8X24_TYPELESS || dm.fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                        ? 1u
                        : 0u;  // else D24 in the low bits (measured: the scene depth is D24S8, standard Z)
        k.dfmt[1] = occlude && buf ? 1u : 0u;
        k.dfmt[2] = g_ndc.load(std::memory_order_relaxed) ? 1u : 0u;
        {
            std::lock_guard lock(g_last_mutex);
            std::memcpy(g_last_k[e], &k, sizeof(k));
        }
        if (e == g_grab_eye && vz < -0.01) {  // the round's centre in this image (the capture's crop)
            const double x = k.L[0][3], y = k.L[1][3], z = vz;
            const double nx = (2 * x / -z - (c.r + c.l)) / (c.r - c.l), ny = (2 * y / -z - (c.u + c.dn)) / (c.u - c.dn);
            cx = static_cast<int>((nx * 0.5 + 0.5) * w);
            cy = static_cast<int>((0.5 - ny * 0.5) * h);
        }
        cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cl->OMSetRenderTargets(1, &rtv[e], FALSE, &dsv);
        // every query is ended each use (an empty one counts 0): the round's, then the row's
        cl->BeginQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * 4 + e));
        if (round_ok) {
            cl->SetGraphicsRoot32BitConstants(0, kConsts, &k, 0);
            cl->DrawInstanced(m.count, 1, m.first, 0);
        }
        cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * 4 + e));
        cl->BeginQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * 4 + 2 + e));
        if (want_row) {
            const int nr = row.n > 6 ? 6 : row.n;
            for (int r = 0; r < nr; ++r) {
                const float side = (static_cast<float>(r) - 0.5f * static_cast<float>(nr - 1)) * gap;
                const float Pr[3] = {row.at[0] + row.rgt[0] * side, row.at[1] + row.rgt[1] * side, row.at[2] + row.rgt[2] * side};
                place(row.rgt, row.up, rz, Pr, nullptr);
                cl->SetGraphicsRoot32BitConstants(0, kConsts, &k, 0);
                cl->DrawInstanced(rm.count, 1, rm.first, 0);
            }
        }
        cl->EndQuery(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * 4 + 2 + e));
    }
    cl->ResolveQueryData(g_qheap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<UINT>(slot * 4), 4, g_qread, slot * 4 * sizeof(uint64_t));
    g_q_pending[slot] = true;
    if (round_ok) g_drawn.fetch_add(1, std::memory_order_relaxed);
    if (want_row) g_row_drawn.fetch_add(1, std::memory_order_relaxed);

}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v;
    in >> c;  // "round"
    while (in >> v) {
        if (v == "on" || v == "off") {
            set_enabled(v == "on");
        } else if (v == "mode" && (in >> c)) {  // mode 0|1|2
            set_mode(std::atoi(c.c_str()));
        } else if (v == "occlude" && (in >> c)) {  // occlude on|off: the game's depth hides the round (a control)
            g_occlude = c == "on";
        } else if (v == "ndc" && (in >> c)) {  // ndc on|off: the model enlarged at the image's centre (a pipeline test)
            g_ndc = c == "on";
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
            float* t = v == "offset" ? g_offset : g_axis;
            t[0] = static_cast<float>(std::atof(c.c_str()));
            t[1] = y;
            t[2] = z;
            if (v == "axis") {
                normalize3(g_axis);
                g_axis_auto = false;
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
            char b[260];
            std::snprintf(b, sizeof(b), "row %d: family %d, %d rounds, set %.0f ms ago, drawn %llu, pixels L %llu R %llu", g_row_on.load() ? 1 : 0, r.family, r.n,
                          r.ms > 0 ? log::now_ms() - r.ms : -1.0, static_cast<unsigned long long>(g_row_drawn.load()),
                          static_cast<unsigned long long>(g_row_px[0].load()), static_cast<unsigned long long>(g_row_px[1].load()));
            return b;
        } else if (v == "held" && (in >> c)) {  // held <john hand> [family]: a round in that hand, as the holsters say (a test aid)
            int fam = 1;
            std::string fs;
            if (in >> fs) fam = std::atoi(fs.c_str());
            set_held(std::atoi(c.c_str()), fam);
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
    char st[700];
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
    std::snprintf(out, len,
                  "round %s (mode %d), held %d (hand %d, family %d), pipeline %s, records %llu, drawn %llu, no cameras %llu, no depth %llu, "
                  "pixels L %llu R %llu (frames counted %llu, seen L %llu R %llu), depth taps %llu (frames %llu, most a frame %d, extra %llu), "
                  "depth %ux%u pitch %u format %u, occlude %d, axes from %d, offset %.3f %.3f %.3f, axis %.2f %.2f %.2f, threads pass %u tap %u record %u",
                  g_on.load() ? "on" : "off", g_mode.load(), held_now() ? 1 : 0, g_hand.load(), g_family.load(), g_ready ? "made" : g_failed ? "failed" : "not made",
                  static_cast<unsigned long long>(g_records.load()), static_cast<unsigned long long>(g_drawn.load()),
                  static_cast<unsigned long long>(g_no_cam.load()), static_cast<unsigned long long>(g_no_depth.load()),
                  static_cast<unsigned long long>(g_px[0].load()), static_cast<unsigned long long>(g_px[1].load()),
                  static_cast<unsigned long long>(g_px_frames.load()), static_cast<unsigned long long>(g_px_seen[0].load()),
                  static_cast<unsigned long long>(g_px_seen[1].load()), static_cast<unsigned long long>(g_taps.load()),
                  static_cast<unsigned long long>(g_tap_frames.load()), g_tap_max.load(), static_cast<unsigned long long>(g_tap_extra.load()), dm.w,
                  dm.h, dm.pitch, dm.fmt, g_occlude.load() ? 1 : 0, g_axes_from.load(), o[0], o[1], o[2], a[0], a[1], a[2], g_pass_tid.load(), g_tap_tid.load(),
                  g_rec_tid.load());
}

}  // namespace rdrvr::round_draw
