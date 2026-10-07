#include "core/xr_blit.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#include "core/anchors.h"
#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/round_draw.h"

namespace rdrvr::xr_blit {
namespace {

// A full-screen triangle; the pixel shader reads the eye image texel for texel (same size), applies the game's
// final gamma and returns the linear value whose sRGB encoding is that display value.
const char kShader[] = R"(
Texture2D<float4> src : register(t0);
cbuffer K : register(b0) { float gamma; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float3 c = saturate(src.Load(int3(pos.xy, 0)).rgb);
    float3 d = pow(c, gamma);
    float3 lin = d <= 0.04045 ? d / 12.92 : pow((d + 0.055) / 1.055, 2.4);
    return float4(lin, 1);
}
)";

constexpr int kLists = 3;
ID3D12Device* g_dev = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_srv_heap = nullptr;  // shader visible: one SRV per staging texture
ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // two RTV slots, written per frame for the acquired images
UINT g_srv_step = 0, g_rtv_step = 0;
ID3D12Resource* g_stage[kStages] = {};
std::atomic<int> g_source[2] = {0, 1};
ID3D12CommandAllocator* g_alloc[kLists] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_event = nullptr;
uint64_t g_fence_value = 0, g_slot_value[kLists] = {};
int g_slot = 0;
uint32_t g_w = 0, g_h = 0;
DXGI_FORMAT g_dst_format = DXGI_FORMAT_UNKNOWN;
bool g_ready = false;
std::atomic<uint64_t> g_blits{0}, g_skips{0};
std::atomic<float> g_last_gamma{0.0f}, g_override{0.0f};

// ---- [XR] EyeShape's paths (eye_shape.h), made only when first used: their own root signature (a linear sampler),
// the resample into an eye-shaped swapchain and the monitor's repaint
const char kShapeShader[] = R"(
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
cbuffer K : register(b0) { float gamma; float pad0; float2 image; float2 content; float2 stage; float2 dst; float2 pad1; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 fetch(float2 p) {  // p: in the staging texture's texels, kept inside the content
    p = clamp(p, 0.5, content - 0.5);
    return saturate(src.SampleLevel(lin, p / stage, 0).rgb);
}
float4 ps_resample(float4 pos : SV_Position) : SV_Target {
    float3 d = pow(fetch(pos.xy * content / image), gamma);
    float3 l = d <= 0.04045 ? d / 12.92 : pow((d + 0.055) / 1.055, 2.4);
    return float4(l, 1);
}
float4 ps_mirror(float4 pos : SV_Position) : SV_Target {
    float s = min(dst.x / content.x, dst.y / content.y);
    float2 p = (pos.xy - 0.5 * (dst - content * s)) / s;
    if (p.x < 0 || p.y < 0 || p.x >= content.x || p.y >= content.y) return float4(0, 0, 0, 1);
    return float4(pow(fetch(p), gamma), 1);
}
)";
uint32_t g_dw = 0, g_dh = 0;  // the eye swapchains (0: the staging textures' size)
ID3D12RootSignature* g_root2 = nullptr;
ID3DBlob *g_vs2 = nullptr, *g_ps_resample = nullptr, *g_ps_mirror = nullptr;
ID3D12PipelineState* g_pso_resample = nullptr;
ID3D12PipelineState* g_pso_mirror = nullptr;
DXGI_FORMAT g_mirror_fmt = DXGI_FORMAT_UNKNOWN;
bool g_shape_failed = false;
ID3D12CommandAllocator* g_m_alloc[kLists] = {};
ID3D12GraphicsCommandList* g_m_list = nullptr;
ID3D12DescriptorHeap* g_m_rtv = nullptr;
ID3D12Fence* g_m_fence = nullptr;
HANDLE g_m_event = nullptr;
uint64_t g_m_value = 0, g_m_slot_value[kLists] = {};
int g_m_slot = 0;
std::atomic<uint64_t> g_resamples{0}, g_rects{0}, g_mirrors{0};

using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                   ID3DBlob**, ID3DBlob**);

ID3DBlob* compile(Compile_t fn, const char* entry, const char* target, const char* source = kShader, size_t size = sizeof(kShader) - 1) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = fn(source, size, "rdrvr_eye_blit", nullptr, nullptr, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) log::error("[xr] eye blit: %s does not compile: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    if (err) err->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

bool make_pipeline() {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!fn) {
        log::error("[xr] eye blit: d3dcompiler_47.dll not available");
        return false;
    }
    ID3DBlob* vs = compile(fn, "vs", "vs_5_0");
    ID3DBlob* ps = compile(fn, "ps", "ps_5_0");
    bool ok = vs && ps;
    if (ok) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 1};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *sig = nullptr, *err = nullptr;
        ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) &&
             SUCCEEDED(g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root)));
        if (sig) sig->Release();
        if (err) err->Release();
    }
    if (ok) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_root;
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = g_dst_format;
        pd.SampleDesc.Count = 1;
        ok = SUCCEEDED(g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso)));
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (!ok) log::error("[xr] eye blit: root signature or pipeline not created");
    return ok;
}

// The eye shape's root signature and shaders, once (one attempt; the reason is logged).
bool ensure_shape() {
    if (g_root2) return true;
    if (g_shape_failed || !g_dev) return false;
    g_shape_failed = true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!fn) return false;
    g_vs2 = compile(fn, "vs", "vs_5_0", kShapeShader, sizeof(kShapeShader) - 1);
    g_ps_resample = compile(fn, "ps_resample", "ps_5_0", kShapeShader, sizeof(kShapeShader) - 1);
    g_ps_mirror = compile(fn, "ps_mirror", "ps_5_0", kShapeShader, sizeof(kShapeShader) - 1);
    if (!g_vs2 || !g_ps_resample || !g_ps_mirror) return false;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 12};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp{};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderRegister = 0;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{2, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *sig = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) &&
              SUCCEEDED(g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root2)));
    if (sig) sig->Release();
    if (err) err->Release();
    if (!ok) {
        log::error("[xr] eye shape: the blit's root signature was not created");
        return false;
    }
    g_shape_failed = false;
    return true;
}

ID3D12PipelineState* make_shape_pso(ID3DBlob* ps, DXGI_FORMAT fmt) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_root2;
    pd.VS = {g_vs2->GetBufferPointer(), g_vs2->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        log::error("[xr] eye shape: a blit pipeline for format %d was not created", static_cast<int>(fmt));
        return nullptr;
    }
    return pso;
}

DXGI_FORMAT unorm_of(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
    }
}

// The shape shader's constants: gamma, -, image, content, stage, dst, -.
void shape_consts(float* k, float gamma, uint32_t iw, uint32_t ih, uint32_t cw, uint32_t ch, uint32_t dw, uint32_t dh) {
    const float v[12] = {gamma, 0.0f, static_cast<float>(iw), static_cast<float>(ih), static_cast<float>(cw), static_cast<float>(ch),
                         static_cast<float>(g_w), static_cast<float>(g_h), static_cast<float>(dw), static_cast<float>(dh), 0.0f, 0.0f};
    std::memcpy(k, v, sizeof(v));
}

}  // namespace

DXGI_FORMAT pick_format(const int64_t* formats, uint32_t count) {
    const DXGI_FORMAT order[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    for (DXGI_FORMAT want : order)
        for (uint32_t i = 0; i < count; ++i)
            if (formats[i] == static_cast<int64_t>(want)) return want;
    return DXGI_FORMAT_UNKNOWN;
}

bool init(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT src_format, DXGI_FORMAT dst_format) {
    if (g_ready) return w == g_w && h == g_h && dst_format == g_dst_format;
    g_dev = dev;
    g_dst_format = dst_format;
    if (!make_pipeline()) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kStages, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srv_heap))) || FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtv_heap)))) {
        log::error("[xr] eye blit: descriptor heaps not created");
        return false;
    }
    g_srv_step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rtv_step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = src_format;
    td.SampleDesc.Count = 1;
    for (int e = 0; e < kStages; ++e) {
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&g_stage[e])))) {
            log::error("[xr] eye blit: staging texture not created");
            return false;
        }
        g_stage[e]->SetName(e == 0 ? L"RDRVR eye stage L" : e == 1 ? L"RDRVR eye stage R" : L"RDRVR eye stage (bind)");
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = src_format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE hcpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        hcpu.ptr += static_cast<SIZE_T>(e) * g_srv_step;
        dev->CreateShaderResourceView(g_stage[e], &sv, hcpu);
    }
    for (auto& a : g_alloc)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
        return false;
    g_list->Close();
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_w = w;
    g_h = h;
    g_ready = true;
    log::info("[xr] eye blit ready: %ux%u, staging format %d, swapchain format %d", w, h, static_cast<int>(src_format),
              static_cast<int>(dst_format));
    return true;
}

ID3D12Resource* stage(int i) { return g_ready && i >= 0 && i < kStages ? g_stage[i] : nullptr; }

void set_source(int eye, int stage_index) {
    if (eye >= 0 && eye < 2 && stage_index >= 0 && stage_index < kStages) g_source[eye].store(stage_index, std::memory_order_relaxed);
}

bool blit(ID3D12CommandQueue* queue, ID3D12Resource* const dst[2], float gamma, const Frame* frame) {
    if (!g_ready || !dst[0] || !dst[1]) return false;
    // [XR] EyeShape: the image drawn at the swapchain image's origin, and how (no frame: the whole stage, texel for texel)
    const uint32_t iw = frame ? frame->iw : g_w, ih = frame ? frame->ih : g_h;
    const bool resample = frame && (frame->cw != frame->iw || frame->ch != frame->ih);
    if (frame && (!iw || !ih || iw > (g_dw ? g_dw : g_w) || ih > (g_dh ? g_dh : g_h) || !frame->cw || !frame->ch ||
                  frame->cw > g_w || frame->ch > g_h))
        return false;
    if (resample && (!ensure_shape() || (!g_pso_resample && !(g_pso_resample = make_shape_pso(g_ps_resample, g_dst_format))))) return false;
    int s = g_slot;
    if (g_fence->GetCompletedValue() < g_slot_value[s]) {
        g_fence->SetEventOnCompletion(g_slot_value[s], g_event);
        if (WaitForSingleObject(g_event, 50) != WAIT_OBJECT_0) {
            g_skips.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    if (FAILED(g_alloc[s]->Reset()) || FAILED(g_list->Reset(g_alloc[s], resample ? g_pso_resample : g_pso))) return false;
    g_list->SetGraphicsRootSignature(resample ? g_root2 : g_root);
    g_list->SetDescriptorHeaps(1, &g_srv_heap);
    if (resample) {
        float k[12];
        shape_consts(k, gamma, iw, ih, frame->cw, frame->ch, 0, 0);
        g_list->SetGraphicsRoot32BitConstants(0, 12, k, 0);
    } else {
        g_list->SetGraphicsRoot32BitConstants(0, 1, &gamma, 0);
    }
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp{0, 0, static_cast<float>(iw), static_cast<float>(ih), 0, 1};
    D3D12_RECT sc{0, 0, static_cast<LONG>(iw), static_cast<LONG>(ih)};
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
    for (int e = 0; e < 2; ++e) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(e) * g_rtv_step;
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = g_dst_format;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_dev->CreateRenderTargetView(dst[e], &rv, rtv);
        D3D12_GPU_DESCRIPTOR_HANDLE srv = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<UINT64>(g_source[e].load(std::memory_order_relaxed)) * g_srv_step;
        g_list->SetGraphicsRootDescriptorTable(1, srv);
        g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g_list->DrawInstanced(3, 1, 0, 0);
        rtvs[e] = rtv;
    }
    // [Reload] RoundInHand, over both eyes (last: its own state); with EyeShape in the image's rect
    round_draw::record(g_dev, g_list, dst, rtvs, g_dw ? g_dw : g_w, g_dh ? g_dh : g_h, g_dst_format, s, frame ? iw : 0, frame ? ih : 0,
                       frame ? frame->dw : 0, frame ? frame->dh : 0);
    if (FAILED(g_list->Close())) return false;
    d3d::submit_internal(queue, g_list);
    g_slot_value[s] = ++g_fence_value;
    queue->Signal(g_fence, g_fence_value);
    g_slot = (s + 1) % kLists;
    g_last_gamma.store(gamma, std::memory_order_relaxed);
    g_blits.fetch_add(1, std::memory_order_relaxed);
    if (frame) (resample ? g_resamples : g_rects).fetch_add(1, std::memory_order_relaxed);
    return true;
}

void set_dst_size(uint32_t w, uint32_t h) {
    g_dw = w;
    g_dh = h;
    if (w != g_w || h != g_h) log::info("[xr] eye blit: the eye swapchains %ux%u, the staging textures %ux%u", w, h, g_w, g_h);
}

bool repaint(ID3D12CommandQueue* queue, ID3D12Resource* bb, uint32_t cw, uint32_t ch, float gamma) {
    static bool made = false, failed = false;
    if (!g_ready || !queue || !bb || !cw || !ch || cw > g_w || ch > g_h || !ensure_shape()) return false;
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    const DXGI_FORMAT f = unorm_of(bd.Format);
    if (!g_pso_mirror || f != g_mirror_fmt) {
        if (g_pso_mirror) g_pso_mirror->Release();
        g_pso_mirror = make_shape_pso(g_ps_mirror, f);
        g_mirror_fmt = f;
        if (!g_pso_mirror) return false;
    }
    if (!made) {  // its own lists, fence and view, once (one attempt)
        if (failed) return false;
        failed = true;
        D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        for (auto& a : g_m_alloc)
            if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
        if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_m_alloc[0], nullptr, IID_PPV_ARGS(&g_m_list))) ||
            FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_m_fence))) ||
            FAILED(g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_m_rtv)))) {
            log::error("[xr] eye shape: the monitor repaint's objects were not created");
            return false;
        }
        g_m_list->Close();
        g_m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        made = true;
        failed = false;
    }
    const int s = g_m_slot;
    if (g_m_fence->GetCompletedValue() < g_m_slot_value[s]) {
        g_m_fence->SetEventOnCompletion(g_m_slot_value[s], g_m_event);
        if (WaitForSingleObject(g_m_event, 50) != WAIT_OBJECT_0) return false;
    }
    if (FAILED(g_m_alloc[s]->Reset()) || FAILED(g_m_list->Reset(g_m_alloc[s], g_pso_mirror))) return false;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_m_rtv->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = f;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_dev->CreateRenderTargetView(bb, &rv, rtv);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_m_list->ResourceBarrier(1, &b);
    g_m_list->SetGraphicsRootSignature(g_root2);
    g_m_list->SetDescriptorHeaps(1, &g_srv_heap);
    float k[12];
    shape_consts(k, gamma, 0, 0, cw, ch, static_cast<uint32_t>(bd.Width), bd.Height);
    g_m_list->SetGraphicsRoot32BitConstants(0, 12, k, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE srv = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<UINT64>(g_source[0].load(std::memory_order_relaxed)) * g_srv_step;  // the left eye
    g_m_list->SetGraphicsRootDescriptorTable(1, srv);
    g_m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp{0, 0, static_cast<float>(bd.Width), static_cast<float>(bd.Height), 0, 1};
    D3D12_RECT sc{0, 0, static_cast<LONG>(bd.Width), static_cast<LONG>(bd.Height)};
    g_m_list->RSSetViewports(1, &vp);
    g_m_list->RSSetScissorRects(1, &sc);
    g_m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_m_list->DrawInstanced(3, 1, 0, 0);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_m_list->ResourceBarrier(1, &b);
    if (FAILED(g_m_list->Close())) return false;
    d3d::submit_internal(queue, g_m_list);
    g_m_slot_value[s] = ++g_m_value;
    queue->Signal(g_m_fence, g_m_value);
    g_m_slot = (s + 1) % kLists;
    g_mirrors.fetch_add(1, std::memory_order_relaxed);
    return true;
}

float game_gamma(float def) {
    float o = g_override.load(std::memory_order_relaxed);
    if (o > 0) return o;
    char* p = *reinterpret_cast<char**>(anchors::addr(anchors::Id::PostFxSingleton));
    if (!p) return def;
    float g = *reinterpret_cast<float*>(p + 0x88c);
    return g > 0.05f && g < 10.0f ? g : def;
}

void set_gamma_override(float g) {
    g_override = g;
    log::info("[xr] eye blit gamma: %s %.4f", g > 0 ? "fixed" : "the game's", g);
}

void status_text(char* out, size_t len) {
    int n = std::snprintf(out, len, "blit %s, %llu frames, %llu skipped, gamma %.4f%s", g_ready ? "ready" : "not made",
                          static_cast<unsigned long long>(g_blits.load()), static_cast<unsigned long long>(g_skips.load()),
                          g_last_gamma.load(), g_override.load() > 0 ? " (FIXED, control)" : "");
    if ((g_dw && (g_dw != g_w || g_dh != g_h)) || g_rects.load() || g_resamples.load() || g_mirrors.load())
        if (n > 0 && static_cast<size_t>(n) < len)
            std::snprintf(out + n, len - static_cast<size_t>(n), " (eye shape: rect %llu, resampled %llu, monitor %llu)",
                          static_cast<unsigned long long>(g_rects.load()), static_cast<unsigned long long>(g_resamples.load()),
                          static_cast<unsigned long long>(g_mirrors.load()));
}

}  // namespace rdrvr::xr_blit
