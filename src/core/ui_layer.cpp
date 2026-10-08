#include "core/ui_layer.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/anchors.h"
#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/post_target.h"
#include "core/state.h"
#include "core/xr_blit.h"

namespace rdrvr::ui_layer {
namespace {

std::atomic<bool> g_redirect{false};
// xr's live-resize guard: the game's targets were made again (their views may be the old ones' handles), so the
// redirect stays off for this start (stop())
std::atomic<bool> g_stopped{false};
std::mutex g_mutex;  // creation and the grab (presenting thread, test thread)
ID3D12Resource* g_ui = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv{};
uint32_t g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
std::atomic<bool> g_ready{false};
// The game's target views: [0] Post FXAA Target, [1] FXAATarget (found once on the presenting thread).
uint64_t g_target_rtv[2][8];
int g_target_nrtv[2] = {};
std::atomic<bool> g_targets_found{false};
bool g_cleared = false;  // recording thread: the UI target was cleared since the target's last bind without depth
std::atomic<uint64_t> g_subst{0}, g_frames_drawn{0}, g_subst_at_frame_end{0};

// grab
std::atomic<int> g_grab_phase{0};  // 1 armed, 2 copied (waiting for the fence), 0 idle
ID3D12CommandAllocator* g_alloc = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Resource* g_readback = nullptr;
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_value = 0;
HANDLE g_fence_event = nullptr, g_done = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp{};
UINT64 g_row_bytes = 0, g_total = 0;
uint64_t g_grab_frame = 0;

int active_target() {
    char* p = *reinterpret_cast<char**>(anchors::addr(anchors::Id::PostFxSingleton));
    return p && !post_target::fxaa(p) ? 1 : 0;
}

bool make_target(ID3D12Device* dev, const D3D12_RESOURCE_DESC& like) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = like.Width;
    d.Height = like.Height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = like.Format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = like.Format;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g_ui))) ||
        FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) {
        log::error("[ui] the UI target could not be created");
        return false;
    }
    g_ui->SetName(L"RDRVR UI layer");
    g_rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(g_ui, nullptr, g_rtv);
    g_w = static_cast<uint32_t>(like.Width);
    g_h = like.Height;
    g_fmt = like.Format;
    return true;
}

// Presenting thread: the game's targets and the mod's UI target, once, when the redirect is first on (the session's
// start: the frame the eyes use, not the game's first one, which its Graphics menu may change before it).
void ensure() {
    if (g_ready.load()) return;
    std::lock_guard lock(g_mutex);
    if (g_ready.load()) return;
    ID3D12Device* dev = state::device.load();
    auto* t0 = static_cast<ID3D12Resource*>(const_cast<void*>(d3d::unique_resource("Post FXAA Target")));
    auto* t1 = static_cast<ID3D12Resource*>(const_cast<void*>(d3d::unique_resource("FXAATarget")));
    if (!dev || !t0) return;
    g_target_nrtv[0] = d3d::rtv_handles_of(t0, g_target_rtv[0], 8);
    g_target_nrtv[1] = t1 ? d3d::rtv_handles_of(t1, g_target_rtv[1], 8) : 0;
    if (!g_target_nrtv[0] || !make_target(dev, t0->GetDesc())) return;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
        return;
    g_list->Close();
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_RESOURCE_DESC d = g_ui->GetDesc();
    UINT rows = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &g_fp, &rows, &g_row_bytes, &g_total);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = g_total;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback))))
        return;
    g_ready = true;
    log::info("[ui] UI target %ux%u format %d; target views: Post FXAA Target %d, FXAATarget %d", g_w, g_h,
              static_cast<int>(g_fmt), g_target_nrtv[0], g_target_nrtv[1]);
}

// ---- step 2: the UI target drawn into an XR quad image and onto the monitor's back buffer. The UI blends its alpha
// channel like its colour (SrcAlpha/InvSrcAlpha), so a single layer drawn onto transparent black stores its colour
// premultiplied and its coverage squared (cycle 30: coverage = sqrt(alpha) reproduces the game's own composite on 81%
// of the menu's UI pixels exactly, mean error 1.0/255; overlapping translucent layers make the rest).
const char kShader[] = R"(
Texture2D<float4> ui : register(t0);
// src: the UI pixel drawn at the destination's origin (the wrist HUD's crop); hole: UI pixels left out (x0 y0 x1 y1);
// scale: UI pixels a destination pixel, size: the UI target's (ps_quad_box only)
cbuffer K : register(b0) { float gamma; float2 src; float pad; float4 hole; float2 scale; float2 size; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 display(int2 p, out float a) {
    float4 u = ui.Load(int3(p, 0));
    a = sqrt(saturate(u.a));
    float3 s = a > 1e-4 ? saturate(u.rgb / a) : float3(0, 0, 0);
    return pow(s, gamma);
}
float4 ps_quad(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy) + int2(src);
    if (p.x >= hole.x && p.x < hole.z && p.y >= hole.y && p.y < hole.w) return float4(0, 0, 0, 0);
    float a;
    float3 d = display(p, a);
    float3 lin = d <= 0.04045 ? d / 12.92 : pow((d + 0.055) / 1.055, 2.4);
    return float4(lin * a, a);
}
float4 ps_mirror(float4 pos : SV_Position) : SV_Target {
    float a;
    float3 d = display(int2(pos.xy), a);
    return float4(d, a);
}
// A destination smaller than the UI under it (a frame wider than [XR] UiQuadMaxWidth, or the runtime's largest
// image): each pixel the mean of the UI pixels it covers, each weighted by its overlap (a box filter), taken in the
// quad's linear premultiplied form, so text does not alias as one texel in two would.
float4 ps_quad_box(float4 pos : SV_Position) : SV_Target {
    float2 lo = src + floor(pos.xy) * scale, hi = lo + scale;
    int2 a = int2(floor(lo)), b = min(int2(ceil(hi)) - 1, int2(size) - 1);
    float4 s = 0;
    float wsum = 0;
    [loop] for (int y = a.y; y <= b.y && y < a.y + 8; ++y) {
        float wy = min(hi.y, y + 1.0) - max(lo.y, (float)y);
        [loop] for (int x = a.x; x <= b.x && x < a.x + 8; ++x) {
            float w = wy * (min(hi.x, x + 1.0) - max(lo.x, (float)x));
            float4 t = 0;
            if (!(x >= hole.x && x < hole.z && y >= hole.y && y < hole.w)) {  // display(), then ps_quad's
                float4 u = ui.Load(int3(x, y, 0));
                float al = sqrt(saturate(u.a));
                float3 d = al > 1e-4 ? pow(saturate(u.rgb / al), gamma) : float3(0, 0, 0);
                float3 lin = d <= 0.04045 ? d / 12.92 : pow(saturate((d + 0.055) / 1.055), 2.4);
                t = float4(lin * al, al);
            }
            s += w * t;
            wsum += w;
        }
    }
    return wsum > 0 ? s / wsum : float4(0, 0, 0, 0);
}
)";

constexpr int kSlots = 3;
struct Draw {
    ID3D12CommandAllocator* alloc[kSlots] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t slot_value[kSlots] = {};
    int slot = 0;
};
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso_quad = nullptr;
ID3D12PipelineState* g_pso_quad_box = nullptr;
ID3D12PipelineState* g_pso_mirror = nullptr;
DXGI_FORMAT g_quad_fmt = DXGI_FORMAT_UNKNOWN, g_quad_box_fmt = DXGI_FORMAT_UNKNOWN, g_mirror_fmt = DXGI_FORMAT_UNKNOWN;
ID3D12DescriptorHeap* g_srv_heap = nullptr;
ID3D12DescriptorHeap* g_draw_rtv_heap = nullptr;  // [0] the back buffer, [1] the quad image, [2] the wrist image
UINT g_rtv_step = 0;
ID3DBlob* g_vs = nullptr;
ID3DBlob* g_ps_quad = nullptr;
ID3DBlob* g_ps_quad_box = nullptr;
ID3DBlob* g_ps_mirror = nullptr;
Draw g_draw_mirror, g_draw_quad, g_draw_wrist;
ID3D12Fence* g_draw_fence = nullptr;
HANDLE g_draw_event = nullptr;
uint64_t g_draw_value = 0;
bool g_draw_failed = false;
std::atomic<bool> g_drawn_this_frame{false};
std::atomic<uint64_t> g_mirrors{0}, g_quads{0}, g_draw_skips{0}, g_boxed{0};

using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                   ID3DBlob**, ID3DBlob**);

ID3DBlob* compile(Compile_t fn, const char* entry, const char* target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = fn(kShader, sizeof(kShader) - 1, "rdrvr_ui", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &err);
    if (FAILED(hr)) log::error("[ui] %s does not compile: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    if (err) err->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

ID3D12PipelineState* make_pso(ID3D12Device* dev, ID3DBlob* ps, DXGI_FORMAT fmt, bool blend) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_root;
    pd.VS = {g_vs->GetBufferPointer(), g_vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = blend ? (D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE)
                                     : D3D12_COLOR_WRITE_ENABLE_ALL;
    if (blend) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) log::error("[ui] pipeline for format %d not created", static_cast<int>(fmt));
    return pso;
}

bool make_draw(ID3D12Device* dev, Draw& d) {
    for (auto& a : d.alloc)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc[0], nullptr, IID_PPV_ARGS(&d.list)))) return false;
    d.list->Close();
    return true;
}

// Presenting thread: the shared pieces, once (after ensure()).
bool ensure_draw(ID3D12Device* dev) {
    if (g_root) return true;
    if (g_draw_failed) return false;
    g_draw_failed = true;  // one attempt; the reason is logged
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!fn) return false;
    g_vs = compile(fn, "vs", "vs_5_0");
    g_ps_quad = compile(fn, "ps_quad", "ps_5_0");
    g_ps_quad_box = compile(fn, "ps_quad_box", "ps_5_0");
    g_ps_mirror = compile(fn, "ps_mirror", "ps_5_0");
    if (!g_vs || !g_ps_quad || !g_ps_quad_box || !g_ps_mirror) return false;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 12};  // gamma, src x y, pad, hole x0 y0 x1 y1, scale x y, size x y
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *sig = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ID3D12RootSignature* root = nullptr;
    ok = ok && SUCCEEDED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root)));
    if (sig) sig->Release();
    if (err) err->Release();
    D3D12_DESCRIPTOR_HEAP_DESC hs{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC hr{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ok = ok && SUCCEEDED(dev->CreateDescriptorHeap(&hs, IID_PPV_ARGS(&g_srv_heap))) &&
         SUCCEEDED(dev->CreateDescriptorHeap(&hr, IID_PPV_ARGS(&g_draw_rtv_heap))) && make_draw(dev, g_draw_mirror) &&
         make_draw(dev, g_draw_quad) && make_draw(dev, g_draw_wrist) &&
         SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_draw_fence)));
    if (!ok) {
        log::error("[ui] draw objects not created");
        return false;
    }
    g_draw_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_rtv_step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dev->CreateShaderResourceView(g_ui, nullptr, g_srv_heap->GetCPUDescriptorHandleForHeapStart());
    g_root = root;
    g_draw_failed = false;
    return true;
}

// Records one full-screen draw of the UI target into `dst` (in state `dst_state`, returned to it) and submits it.
// scale: UI pixels a destination pixel (ps_quad_box; else 1).
bool draw_ui(Draw& d, ID3D12PipelineState* pso, ID3D12Resource* dst, DXGI_FORMAT view_fmt, D3D12_RESOURCE_STATES dst_state, int rtv_slot,
             float gamma, const float src[2] = nullptr, const int hole[4] = nullptr, const float scale[2] = nullptr) {
    const float k[12] = {gamma, src ? src[0] : 0.0f, src ? src[1] : 0.0f, 0.0f, hole ? static_cast<float>(hole[0]) : 0.0f,
                         hole ? static_cast<float>(hole[1]) : 0.0f, hole ? static_cast<float>(hole[2]) : 0.0f,
                         hole ? static_cast<float>(hole[3]) : 0.0f, scale ? scale[0] : 1.0f, scale ? scale[1] : 1.0f,
                         static_cast<float>(g_w), static_cast<float>(g_h)};
    ID3D12CommandQueue* q = state::present_queue.load();
    ID3D12Device* dev = state::device.load();
    if (!q || !dev || !pso) return false;
    int s = d.slot;
    if (g_draw_fence->GetCompletedValue() < d.slot_value[s]) {
        g_draw_fence->SetEventOnCompletion(d.slot_value[s], g_draw_event);
        if (WaitForSingleObject(g_draw_event, 50) != WAIT_OBJECT_0) {
            g_draw_skips.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    if (FAILED(d.alloc[s]->Reset()) || FAILED(d.list->Reset(d.alloc[s], pso))) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_draw_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(rtv_slot) * g_rtv_step;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = view_fmt;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(dst, &rv, rtv);
    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& x : b) {
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    b[0].Transition.pResource = g_ui;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.pResource = dst;
    b[1].Transition.StateBefore = dst_state;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bool dst_transition = dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET;
    d.list->ResourceBarrier(dst_transition ? 2 : 1, b);
    d.list->SetGraphicsRootSignature(g_root);
    d.list->SetDescriptorHeaps(1, &g_srv_heap);
    d.list->SetGraphicsRoot32BitConstants(0, 12, k, 0);
    d.list->SetGraphicsRootDescriptorTable(1, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_RESOURCE_DESC dd = dst->GetDesc();
    D3D12_VIEWPORT vp{0, 0, static_cast<float>(dd.Width), static_cast<float>(dd.Height), 0, 1};
    D3D12_RECT sc{0, 0, static_cast<LONG>(dd.Width), static_cast<LONG>(dd.Height)};
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &sc);
    d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    d.list->DrawInstanced(3, 1, 0, 0);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    d.list->ResourceBarrier(dst_transition ? 2 : 1, b);
    if (FAILED(d.list->Close())) return false;
    d3d::submit_internal(q, d.list);
    d.slot_value[s] = ++g_draw_value;
    q->Signal(g_draw_fence, g_draw_value);
    d.slot = (s + 1) % kSlots;
    return true;
}

DXGI_FORMAT unorm_of(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
    }
}

// Presenting thread, this frame's UI (redirected) onto the back buffer about to be presented, so the monitor keeps it.
void mirror() {
    IDXGISwapChain* sc = state::swapchain.load();
    ID3D12Device* dev = state::device.load();
    if (!sc || !dev || !ensure_draw(dev)) return;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
    ID3D12Resource* bb = nullptr;
    HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) return;
    DXGI_FORMAT f = unorm_of(bb->GetDesc().Format);
    if (!g_pso_mirror || f != g_mirror_fmt) {
        if (g_pso_mirror) g_pso_mirror->Release();
        g_pso_mirror = make_pso(dev, g_ps_mirror, f, true);
        g_mirror_fmt = f;
    }
    if (draw_ui(g_draw_mirror, g_pso_mirror, bb, f, D3D12_RESOURCE_STATE_PRESENT, 0, xr_blit::game_gamma(1.0f)))
        g_mirrors.fetch_add(1, std::memory_order_relaxed);
    bb->Release();
}

// Recording thread, inside the game's OMSetRenderTargets: the UI pass binds the post output with its depth view.
bool subst(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, const D3D12_CPU_DESCRIPTOR_HANDLE* ds,
           D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    if (!g_redirect.load(std::memory_order_relaxed) || !g_ready.load(std::memory_order_acquire) || n != 1 || !rts) return false;
    int t = active_target();
    bool target = false;
    for (int i = 0; i < g_target_nrtv[t]; ++i) target = target || rts[0].ptr == g_target_rtv[t][i];
    if (!target) return false;
    if (!ds) {  // the post chain's own binds of its output (FXAA or tonemap, the hudless pass): a new frame's UI follows
        g_cleared = false;
        return false;
    }
    if (!g_cleared) {
        const float clear[4] = {0, 0, 0, 0};
        cl->ClearRenderTargetView(g_rtv, clear, 0, nullptr);
        g_cleared = true;
    }
    *out = g_rtv;
    g_subst.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void on_frame_end(uint64_t frame) {
    if (g_redirect.load(std::memory_order_relaxed)) ensure();
    uint64_t n = g_subst.load();
    bool drawn = n != g_subst_at_frame_end.exchange(n);
    g_drawn_this_frame = drawn;
    if (drawn) g_frames_drawn.fetch_add(1, std::memory_order_relaxed);
    if (drawn && g_ready.load()) mirror();
    if (g_grab_phase.load() != 1 || !drawn) return;
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q || FAILED(g_alloc->Reset()) || FAILED(g_list->Reset(g_alloc, nullptr))) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_ui;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = g_readback;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = g_fp;
    sl.pResource = g_ui;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    d3d::submit_internal(q, g_list);
    q->Signal(g_fence, ++g_fence_value);
    g_grab_frame = frame;
    g_grab_phase = 2;
    SetEvent(g_done);
}

}  // namespace

void init() {
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d3d::set_rtv_substitute(subst);
    d3d::add_frame_end_listener(on_frame_end);
}

void set_redirect(bool on) {
    if (on && g_stopped.load()) {
        log::limited("ui.stopped", 4, "[ui] redirect stays off: the game's frame was made again at another size (restart the game)");
        return;
    }
    g_redirect = on;
    log::info("[ui] redirect %s", on ? "on: the UI pass draws into the mod's UI target" : "off");
}

void stop(const char* why) {
    if (g_stopped.exchange(true)) return;
    g_redirect = false;
    log::error("[ui] redirect off for this start: %s (the UI stays in the game's own image)", why);
}

bool stopped() { return g_stopped.load(); }

std::string grab(const std::string& path, unsigned timeout_ms) {
    if (!g_ready.load()) return "ERROR the UI target is not made (NOT MEASURED)";
    if (!g_redirect.load()) return "ERROR the redirect is off (NOT MEASURED)";
    ResetEvent(g_done);
    g_grab_phase = 1;
    if (WaitForSingleObject(g_done, timeout_ms) != WAIT_OBJECT_0) {
        g_grab_phase = 0;
        return "ERROR no frame drew the UI (NOT MEASURED)";
    }
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    if (WaitForSingleObject(g_fence_event, 3000) != WAIT_OBJECT_0) {
        g_grab_phase = 0;
        return "ERROR the copy timed out (NOT MEASURED)";
    }
    uint8_t* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(g_total)};
    std::string out;
    if (SUCCEEDED(g_readback->Map(0, &r, reinterpret_cast<void**>(&p))) && p) {
        std::wstring wpath(path.begin(), path.end()), tmp = wpath + L".tmp";
        FILE* f = nullptr;
        bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
        if (ok) {
            const uint32_t hdr[8] = {0x31454452u /* RDE1 */, g_w, g_h, static_cast<uint32_t>(g_fmt), 0xffffffffu,
                                     static_cast<uint32_t>(g_grab_frame), static_cast<uint32_t>(g_row_bytes), 0};
            fwrite(hdr, 1, sizeof(hdr), f);
            for (uint32_t y = 0; y < g_h; ++y)
                fwrite(p + g_fp.Offset + static_cast<UINT64>(y) * g_fp.Footprint.RowPitch, 1, static_cast<size_t>(g_row_bytes), f);
            ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        D3D12_RANGE none{0, 0};
        g_readback->Unmap(0, &none);
        char line[320];
        std::snprintf(line, sizeof(line), "%s %s %ux%u (frame %llu)", ok ? "wrote" : "ERROR writing", path.c_str(), g_w, g_h,
                      static_cast<unsigned long long>(g_grab_frame));
        out = line;
    } else {
        out = "ERROR Map failed";
    }
    g_grab_phase = 0;
    return out;
}

bool has_ui() { return g_ready.load() && g_drawn_this_frame.load(); }

void size(uint32_t* w, uint32_t* h) {
    *w = g_w;
    *h = g_h;
}

// The pipeline for a quad image: texel for texel when it is the UI's size there (as before), else the box filter.
ID3D12PipelineState* quad_pso(ID3D12Device* dev, DXGI_FORMAT fmt, bool box) {
    if (box) {
        if (!g_pso_quad_box || fmt != g_quad_box_fmt) {
            if (g_pso_quad_box) g_pso_quad_box->Release();
            g_pso_quad_box = make_pso(dev, g_ps_quad_box, fmt, false);
            g_quad_box_fmt = fmt;
        }
        return g_pso_quad_box;
    }
    if (!g_pso_quad || fmt != g_quad_fmt) {
        if (g_pso_quad) g_pso_quad->Release();
        g_pso_quad = make_pso(dev, g_ps_quad, fmt, false);
        g_quad_fmt = fmt;
    }
    return g_pso_quad;
}

bool draw_quad(ID3D12Resource* dst, DXGI_FORMAT fmt, const int hole[4]) {
    ID3D12Device* dev = state::device.load();
    if (!has_ui() || !dev || !ensure_draw(dev)) return false;
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    if (!dd.Width || !dd.Height || dd.Width > g_w || dd.Height > g_h) return false;
    const bool box = dd.Width != g_w || dd.Height != g_h;
    const float scale[2] = {static_cast<float>(g_w) / static_cast<float>(dd.Width), static_cast<float>(g_h) / static_cast<float>(dd.Height)};
    bool ok = draw_ui(g_draw_quad, quad_pso(dev, fmt, box), dst, fmt, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, xr_blit::game_gamma(1.0f),
                      nullptr, hole, box ? scale : nullptr);
    if (ok) g_quads.fetch_add(1, std::memory_order_relaxed);
    if (ok && box) g_boxed.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool draw_crop(ID3D12Resource* dst, DXGI_FORMAT fmt, int x0, int y0, uint32_t cw, uint32_t ch) {
    ID3D12Device* dev = state::device.load();
    if (!has_ui() || !dev || !ensure_draw(dev)) return false;
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    if (!dd.Width || !dd.Height || x0 < 0 || y0 < 0 || !cw || !ch || static_cast<uint64_t>(x0) + cw > g_w ||
        static_cast<uint64_t>(y0) + ch > g_h || dd.Width > cw || dd.Height > ch)
        return false;
    const bool box = dd.Width != cw || dd.Height != ch;
    const float src[2] = {static_cast<float>(x0), static_cast<float>(y0)};
    const float scale[2] = {static_cast<float>(cw) / static_cast<float>(dd.Width), static_cast<float>(ch) / static_cast<float>(dd.Height)};
    return draw_ui(g_draw_wrist, quad_pso(dev, fmt, box), dst, fmt, D3D12_RESOURCE_STATE_RENDER_TARGET, 2, xr_blit::game_gamma(1.0f), src,
                   nullptr, box ? scale : nullptr);
}

void status_text(char* out, size_t len) {
    std::snprintf(out, len,
                  "ui target %s, redirect %s, binds redirected %llu, frames with UI %llu, mirrored %llu, quads %llu, skipped %llu | "
                  "target %ux%u, quads filtered %llu%s",
                  g_ready.load() ? "made" : "not made", g_redirect.load() ? "on" : "off", static_cast<unsigned long long>(g_subst.load()),
                  static_cast<unsigned long long>(g_frames_drawn.load()), static_cast<unsigned long long>(g_mirrors.load()),
                  static_cast<unsigned long long>(g_quads.load()), static_cast<unsigned long long>(g_draw_skips.load()), g_w, g_h,
                  static_cast<unsigned long long>(g_boxed.load()), g_stopped.load() ? ", STOPPED (the game's frame made again)" : "");
}

}  // namespace rdrvr::ui_layer
