#include "core/menu.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/actions.h"
#include "core/aim.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/body.h"
#include "core/holster.h"
#include "core/reload.h"
#include "core/gestures.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/dual.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/hands.h"
#include "core/log.h"
#include "core/physics.h"
#include "core/pose.h"
#include "core/render_settings.h"
#include "core/state.h"
#include "core/ui_layer.h"
#include "core/vr_mode.h"
#include "core/xr.h"
#include "core/round_draw.h"

namespace rdrvr::menu {
namespace {

constexpr int kW = 1024, kH = 768, kSlots = 3, kSrvs = 64;
constexpr float kQuadW = 1.0f, kQuadH = kQuadW * kH / kW, kDistance = 1.2f;

std::atomic<bool> g_visible{false}, g_place{false};
std::atomic<uint64_t> g_frames{0}, g_clicks{0};
bool g_ready = false, g_failed = false;

ID3D12Resource* g_rt = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // [0] the menu target, [1] the quad image
ID3D12DescriptorHeap* g_srv_heap = nullptr;
UINT g_rtv_step = 0, g_srv_step = 0;
int g_srv_next = 0;
std::vector<int> g_srv_free;
D3D12_GPU_DESCRIPTOR_HANDLE g_rt_srv{};
ID3D12CommandAllocator* g_alloc[kSlots] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_event = nullptr;
uint64_t g_fence_value = 0, g_slot_value[kSlots] = {};
int g_slot = 0;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
DXGI_FORMAT g_quad_fmt = DXGI_FORMAT_UNKNOWN;
XrSwapchain g_sc = XR_NULL_HANDLE;
std::vector<ID3D12Resource*> g_images;
XrPosef g_pose{{0, 0, 0, 1}, {0, 0, -kDistance}};

std::mutex g_find_mutex;
std::atomic<float> g_scroll_req{-1.0f};  // "menu scroll": the window's scroll wanted at its next draw (-1 none)
std::atomic<float> g_scroll_now{0.0f};   // the window's scroll at its last draw
struct Rect {
    float x0, y0, x1, y1;
};
std::map<std::string, Rect> g_rects;  // widget label -> pixel rect, last frame
XrPosef g_pose_shared{{0, 0, 0, 1}, {0, 0, -kDistance}};

// ImGui's colours are sRGB values; it draws them over transparent black premultiplied, alpha as coverage. Into the
// sRGB quad: the straight colour decoded to linear, premultiplied again.
const char kShader[] = R"(
Texture2D<float4> menu : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float4 c = menu.Load(int3(pos.xy, 0));
    float a = saturate(c.a);
    float3 s = a > 1e-4 ? saturate(c.rgb / a) : float3(0, 0, 0);
    float3 lin = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
    return float4(lin * a, a);
}
)";

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    int i;
    if (!g_srv_free.empty()) {
        i = g_srv_free.back();
        g_srv_free.pop_back();
    } else {
        i = g_srv_next++;
    }
    if (i >= kSrvs) i = kSrvs - 1;  // never expected: ImGui allocates one per texture (the font atlas)
    *cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
    cpu->ptr += static_cast<SIZE_T>(i) * g_srv_step;
    *gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
    gpu->ptr += static_cast<UINT64>(i) * g_srv_step;
}
void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    g_srv_free.push_back(static_cast<int>((cpu.ptr - g_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srv_step));
}

using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                   ID3DBlob**, ID3DBlob**);

bool make_converter(ID3D12Device* dev) {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!fn) return false;
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(fn(kShader, sizeof(kShader) - 1, "rdrvr_menu", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)) &&
              SUCCEEDED(fn(kShader, sizeof(kShader) - 1, "rdrvr_menu", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err));
    if (err) {
        log::error("[menu] shader: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    if (ok) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {1, &range};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob* sig = nullptr;
        ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr)) &&
             SUCCEEDED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root)));
        if (sig) sig->Release();
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
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = g_quad_fmt;
        pd.SampleDesc.Count = 1;
        ok = SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso)));
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    return ok;
}

bool ensure(XrSession session) {
    if (g_ready) return true;
    if (g_failed) return false;
    g_failed = true;  // one attempt; the reason is logged
    ID3D12Device* dev = state::device.load();
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!dev || !q) return false;
    uint32_t nf = 0;
    int64_t formats[64];
    xrEnumerateSwapchainFormats(session, 64, &nf, formats);
    for (uint32_t i = 0; i < nf && g_quad_fmt == DXGI_FORMAT_UNKNOWN; ++i)
        if (formats[i] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || formats[i] == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            g_quad_fmt = static_cast<DXGI_FORMAT>(formats[i]);
    if (g_quad_fmt == DXGI_FORMAT_UNKNOWN) {
        log::error("[menu] no sRGB swapchain format");
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = static_cast<int64_t>(g_quad_fmt);
    ci.sampleCount = 1;
    ci.width = kW;
    ci.height = kH;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &g_sc))) {
        log::error("[menu] xrCreateSwapchain failed");
        return false;
    }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages(g_sc, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_sc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    for (auto& im : imgs) g_images.push_back(im.texture);

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kW;
    td.Height = kH;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvs, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                            IID_PPV_ARGS(&g_rt))) ||
        FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtv_heap))) ||
        FAILED(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srv_heap))))
        return false;
    g_rt->SetName(L"RDRVR menu");
    g_rtv_step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_srv_step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dev->CreateRenderTargetView(g_rt, nullptr, g_rtv_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    srv_alloc(nullptr, &cpu, &g_rt_srv);
    dev->CreateShaderResourceView(g_rt, nullptr, cpu);
    for (auto& a : g_alloc)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
        return false;
    g_list->Close();
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!make_converter(dev)) {
        log::error("[menu] converter pipeline not created");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
    io.FontGlobalScale = 1.6f;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.6f);
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = dev;
    ii.CommandQueue = q;
    ii.NumFramesInFlight = kSlots;
    ii.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_srv_heap;
    ii.SrvDescriptorAllocFn = srv_alloc;
    ii.SrvDescriptorFreeFn = srv_free;
    {
        d3d::InternalSubmitScope internal;
        if (!ImGui_ImplDX12_Init(&ii)) {
            log::error("[menu] ImGui DX12 backend not initialised");
            return false;
        }
    }
    g_ready = true;
    g_failed = false;
    log::info("[menu] ready: %dx%d ImGui %s, quad format %d, %zu images", kW, kH, IMGUI_VERSION, static_cast<int>(g_quad_fmt),
              g_images.size());
    return true;
}

void rotate(const XrQuaternionf& q, const float v[3], float out[3]) {
    float tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

void place(const XrView* views) {
    const XrQuaternionf& q = views[0].pose.orientation;
    float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float yaw = std::atan2(-fx, -fz);
    float mx = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
    float my = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
    float mz = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
    g_pose.orientation = {0, std::sin(0.5f * yaw), 0, std::cos(0.5f * yaw)};
    g_pose.position = {mx - kDistance * std::sin(yaw), my - 0.1f, mz - kDistance * std::cos(yaw)};
    std::lock_guard lock(g_find_mutex);
    g_pose_shared = g_pose;
    log::info("[menu] placed 1.2 m ahead: (%.2f %.2f %.2f), yaw %.1f deg", g_pose.position.x, g_pose.position.y, g_pose.position.z,
              yaw * 57.29578f);
}

// A hand's ray onto the quad, in menu pixels; false when it misses.
bool laser(int hand, float* px, float* py) {
    hands::Hand h = hands::get(hand);
    if (!h.valid) return false;
    XrQuaternionf hq{h.rot[0], h.rot[1], h.rot[2], h.rot[3]};
    const float fwd[3] = {0, 0, -1}, nrm[3] = {0, 0, 1};
    float d[3], n[3];
    rotate(hq, fwd, d);
    rotate(g_pose.orientation, nrm, n);
    float c[3] = {g_pose.position.x, g_pose.position.y, g_pose.position.z};
    float denom = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    if (std::fabs(denom) < 1e-5f) return false;
    float t = ((c[0] - h.pos[0]) * n[0] + (c[1] - h.pos[1]) * n[1] + (c[2] - h.pos[2]) * n[2]) / denom;
    if (t <= 0) return false;
    float rel[3] = {h.pos[0] + t * d[0] - c[0], h.pos[1] + t * d[1] - c[1], h.pos[2] + t * d[2] - c[2]};
    XrQuaternionf inv{-g_pose.orientation.x, -g_pose.orientation.y, -g_pose.orientation.z, g_pose.orientation.w};
    float local[3];
    rotate(inv, rel, local);
    float u = local[0] / kQuadW + 0.5f, v = 0.5f - local[1] / kQuadH;
    if (u < 0 || u > 1 || v < 0 || v > 1) return false;
    *px = u * kW;
    *py = v * kH;
    return true;
}

void track(const char* label) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    {
        std::lock_guard lock(g_find_mutex);
        g_rects[label] = {a.x, a.y, b.x, b.y};
    }
    if (ImGui::IsItemClicked()) {
        g_clicks.fetch_add(1, std::memory_order_relaxed);
        log::info("[menu] clicked \"%s\"", label);
    }
}

// Settings the menu shows, read once from the ini files; each change is written to the user ini and applied.
struct Settings {
    bool loaded = false;
    bool auto_mode = true, ui_quad = true, cut3d = false;
    int aa = 1;
    int dlss_q = 0;  // [Render] DlssQuality (the game's index 0..5)
    float separation = 1.0f;
    bool split[13] = {};
};
Settings g_set;
const char* const kSplits[13] = {"grass", "gust", "lights", "forest", "post", "masks", "exposure",
                                 "rain", "pfxmap", "godrays", "damage", "clouds", "sunvis"};

// round 13: an adjustment by arrows (sliders were hard to set): for right, up and forward a left and a right arrow,
// one step a press, the value between; true when a press changed it. Each arrow tracked as "<label> <axis> minus" / "plus"
bool nudge3(const char* label, float v[3], float step, float lim = 1.0f) {
    static const char* const kAxis[3] = {"right", "up", "forward"};
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    for (int i = 0; i < 3; ++i) {
        char name[96];
        ImGui::PushID(i);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##dn", ImGuiDir_Left)) v[i] -= step, changed = true;
        std::snprintf(name, sizeof(name), "%s %s minus", label, kAxis[i]);
        track(name);
        ImGui::SameLine();
        ImGui::Text("%s %+.3f", kAxis[i], v[i]);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Right)) v[i] += step, changed = true;
        std::snprintf(name, sizeof(name), "%s %s plus", label, kAxis[i]);
        track(name);
        ImGui::PopID();
        if (changed) v[i] = std::fmin(lim, std::fmax(-lim, std::round(v[i] / step) * step));
    }
    ImGui::PopID();
    return changed;
}

// one value by arrows (a ring's size), as nudge3
bool nudge1(const char* label, float* v, float step, float lo, float hi, const char* fmt = "%.3f") {
    bool changed = false;
    char name[96];
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##dn", ImGuiDir_Left)) *v -= step, changed = true;
    std::snprintf(name, sizeof(name), "%s minus", label);
    track(name);
    ImGui::SameLine();
    ImGui::Text(fmt, *v);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##up", ImGuiDir_Right)) *v += step, changed = true;
    std::snprintf(name, sizeof(name), "%s plus", label);
    track(name);
    ImGui::PopID();
    if (changed) *v = std::fmin(hi, std::fmax(lo, std::round(*v / step) * step));
    return changed;
}

void load_settings() {
    g_set.auto_mode = config::get_bool("XR", "AutoMode", true);
    g_set.ui_quad = config::get_bool("XR", "UiQuad", true);
    g_set.cut3d = config::get_string("Screen", "CutsceneMode", "Screen") == "3D";
    g_set.separation = config::get_float("Screen", "Cutscene3DSeparation", 1.0f);
    g_set.aa = config::get_int("Render", "ForceAntiAliasing", 1);
    g_set.dlss_q = config::get_int("Render", "DlssQuality", 0);
    for (int i = 0; i < 13; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "Split_%s", kSplits[i]);
        g_set.split[i] = config::get_bool("Stereo", key, true);
    }
    g_set.loaded = true;
}

void draw() {
    if (!g_set.loaded) load_settings();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kW), static_cast<float>(kH)));
    ImGui::Begin("RDRVR", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    if (const float sy = g_scroll_req.exchange(-1.0f); sy >= 0.0f) ImGui::SetScrollY(sy);
    g_scroll_now.store(ImGui::GetScrollY(), std::memory_order_relaxed);
    if (ImGui::BeginTabBar("tabs")) {
        bool tab_general = ImGui::BeginTabItem("General");
        track("General");  // the tab's own rect, selected or not
        if (tab_general) {
            if (ImGui::Checkbox("Automatic view modes", &g_set.auto_mode)) {
                vr_mode::set_auto(g_set.auto_mode);
                config::set("XR", "AutoMode", g_set.auto_mode ? "1" : "0");
            }
            track("Automatic view modes");
            if (ImGui::Checkbox("HUD on its own quad", &g_set.ui_quad)) {
                ui_layer::set_redirect(g_set.ui_quad && xr::submitting());
                config::set("XR", "UiQuad", g_set.ui_quad ? "1" : "0");
            }
            track("HUD on its own quad");
            bool imp = camera_lever::level_imposters();
            if (ImGui::Checkbox("Distant trees: no spinning with the head", &imp)) {
                camera_lever::set_level_imposters(imp);
                config::set("XR", "LevelImposters", imp ? "1" : "0");
            }
            track("Distant trees: no spinning with the head");
            bool wrist = xr::hud_on_wrist();
            if (ImGui::Checkbox("HUD on the left wrist (look at it; the prompts stay on the quad)", &wrist)) xr::set_hud_on_wrist(wrist);
            track("HUD on the left wrist (look at it; the prompts stay on the quad)");
            {  // DLSS is chosen for the next start: switching to or from it while the game runs would re-create its swapchain
                const char* aa[] = {"Off", "FXAA", "Native TAA", "DLSS (DLAA, a history per eye; from the next start)"};
                int a = g_set.aa < 0 ? 1 : g_set.aa > 3 ? 3 : g_set.aa;
                if (ImGui::Combo("Anti-aliasing", &a, aa, 4)) {
                    g_set.aa = a;
                    if (a <= 2) render_settings::set_aa(a);  // refused (kept for the next start) while DLSS runs
                    config::set("Render", "ForceAntiAliasing", std::to_string(a));
                }
                track("Anti-aliasing");
                if (g_set.aa == 3) {  // the game's quality index: what DLSS renders before it upscales to the game's resolution
                    static const char* const kQ[6] = {"DLAA (native resolution: the sharpest, the dearest)", "Dynamic (DLAA here)",
                                                      "Ultra performance (1/3 per axis)", "Performance (1/2 per axis)",
                                                      "Balanced (0.58 per axis)", "Quality (2/3 per axis)"};
                    int q = g_set.dlss_q < 0 ? 0 : g_set.dlss_q > 5 ? 5 : g_set.dlss_q;
                    if (ImGui::Combo("  DLSS quality (from the next start)", &q, kQ, 6)) {
                        g_set.dlss_q = q;
                        config::set("Render", "DlssQuality", std::to_string(q));
                    }
                    track("DLSS quality");
                }
                const int running = render_settings::forced_aa();
                if (running >= 0 && ((running == 3) != (g_set.aa == 3) ||
                                     (running == 3 && g_set.dlss_q != render_settings::dlss_quality())))
                    ImGui::TextDisabled("  Restart the game to apply (DLSS can only change at the start).");
            }
            ImGui::EndTabItem();
        }
        bool tab_comfort = ImGui::BeginTabItem("Comfort");
        track("Comfort");  // the tab's own rect, selected or not
        if (tab_comfort) {
            if (ImGui::Button("Recentre")) camera_lever::recentre();
            track("Recentre");
            bool fp = pose::anchor_enabled();
            if (ImGui::Checkbox("First person (camera anchor)", &fp)) {
                pose::set_anchor(fp);
                config::set("Body", "CameraAnchor", fp ? "1" : "0");
            }
            track("First person (camera anchor)");
            bool hide = body::hide_enabled();
            if (ImGui::Checkbox("Hide head and hat in first person", &hide)) {
                body::set_hide_enabled(hide);
                config::set("Body", "HideHead", hide ? "1" : "0");
            }
            track("Hide head and hat in first person");
            int st = body::stance();
            if (ImGui::RadioButton("Upper body upright", st == 1)) st = 1;
            track("Upper body upright");
            ImGui::SameLine();
            if (ImGui::RadioButton("Remove lean only", st == 2)) st = 2;
            track("Remove lean only");
            ImGui::SameLine();
            if (ImGui::RadioButton("Game animation", st == 0)) st = 0;
            track("Game animation");
            if (st != body::stance()) {
                body::set_stance(st);
                config::set("Body", "Stance", st == 0 ? "game" : st == 2 ? "lean" : "upright");
            }
            float back = body::body_back();
            if (ImGui::SliderFloat("Body back (m)", &back, 0.0f, 0.4f, "%.2f")) body::set_body_back(back);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char v[16];
                std::snprintf(v, sizeof(v), "%.2f", back);
                config::set("Body", "BodyBack", v);
            }
            track("Body back (m)");
            float pitch = body::torso_pitch();
            if (ImGui::SliderFloat("Torso pitch (deg)", &pitch, -20.0f, 20.0f, "%.0f")) body::set_torso_pitch(pitch);
            if (ImGui::IsItemDeactivatedAfterEdit()) config::set("Body", "TorsoPitch", std::to_string(static_cast<int>(pitch)));
            track("Torso pitch (deg)");
            float lift = pose::eye_lift();
            if (ImGui::SliderFloat("Eye height offset (m)", &lift, -0.2f, 0.3f, "%.2f")) pose::set_eye_lift(lift);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char v[16];
                std::snprintf(v, sizeof(v), "%.2f", lift);
                config::set("Body", "SeatedLift", v);
            }
            track("Eye height offset (m)");
            bool lockf = body::locks_facing();
            if (ImGui::Checkbox("Lock body facing", &lockf)) {
                body::set_locks_facing(lockf);
                config::set("Body", "LockFacing", lockf ? "1" : "0");
            }
            track("Lock body facing");
            bool follow = body::follows_head();
            if (ImGui::Checkbox("Body moves with the headset", &follow)) {
                body::set_follows_head(follow);
                config::set("Body", "BodyFollowsHead", follow ? "1" : "0");
            }
            track("Body moves with the headset");
            int turn = pose::snap_turning() ? 1 : 0;
            if (ImGui::RadioButton("Smooth turning", turn == 0)) turn = 0;
            track("Smooth turning");
            if (ImGui::RadioButton("Snap turning", turn == 1)) turn = 1;
            track("Snap turning");
            if ((turn == 1) != pose::snap_turning()) {
                pose::set_turning(turn == 1, pose::turn_speed(), pose::snap_angle());
                config::set("Comfort", "TurnMode", turn ? "snap" : "smooth");
            }
            float speed = pose::turn_speed(), angle = pose::snap_angle();
            if (ImGui::SliderFloat("Turn speed (deg/s)", &speed, 30.0f, 240.0f, "%.0f")) pose::set_turning(pose::snap_turning(), speed, angle);
            if (ImGui::IsItemDeactivatedAfterEdit()) config::set("Comfort", "TurnSpeed", std::to_string(static_cast<int>(speed)));
            track("Turn speed (deg/s)");
            if (ImGui::SliderFloat("Snap angle (deg)", &angle, 10.0f, 90.0f, "%.0f")) pose::set_turning(pose::snap_turning(), speed, angle);
            if (ImGui::IsItemDeactivatedAfterEdit()) config::set("Comfort", "SnapAngle", std::to_string(static_cast<int>(angle)));
            track("Snap angle (deg)");
            // riding
            int steer = pose::steer_by_head() ? 1 : 0;
            if (ImGui::RadioButton("Horse: steer with the stick", steer == 0)) steer = 0;
            track("Horse: steer with the stick");
            ImGui::SameLine();
            if (ImGui::RadioButton("Horse: steer with the head", steer == 1)) steer = 1;
            track("Horse: steer with the head");
            if ((steer == 1) != pose::steer_by_head()) pose::set_steer_by_head(steer == 1);
            bool saddle = pose::saddle_anchor();
            float stau = pose::saddle_smoothing();
            if (ImGui::Checkbox("Horse: view from the saddle (not the bouncing head)", &saddle)) pose::set_saddle(saddle, stau, true);
            track("Horse: view from the saddle (not the bouncing head)");
            bool climb = pose::saddle_climb();
            if (ImGui::Checkbox("  ... the view keeps its height on climbs (never down at the saddle)", &climb)) pose::set_saddle_climb(climb, true);
            track("  ... the view keeps its height on climbs (never down at the saddle)");
            if (ImGui::SliderFloat("Saddle smoothing (s)", &stau, 0.0f, 0.6f, "%.2f")) pose::set_saddle(saddle, stau, false);
            if (ImGui::IsItemDeactivatedAfterEdit()) pose::set_saddle(saddle, stau, true);
            track("Saddle smoothing (s)");
            bool cb = controls::click_brake();
            if (ImGui::Checkbox("Horse: the left stick click brakes (pull the stick back too for a hard stop)", &cb)) controls::set_click_brake(cb);
            track("Horse: the left stick click brakes (pull the stick back too for a hard stop)");
            ImGui::EndTabItem();
        }
        bool tab_hands = ImGui::BeginTabItem("Hands");
        track("Hands");
        if (tab_hands) {
            {  // [Hands] InteractOffset (round 13): the arrows, 5 mm a press
                float go[3];
                holster::interact_offset(go);
                ImGui::TextUnformatted("Where your hands grab (the white dot with the holsters shown), from the wrist (m):");
                if (nudge3("Interaction spot", go, 0.005f)) holster::set_interact_offset(go, true);
            }
            {  // 2026-10-07: how the controller sits in the hand, per controller type ([Controls] GripFit<Type>)
                float fo[3], fa[3];
                const char* nm = controllers::fit_name(1);
                if (nm && controllers::fit(1, fo, fa)) {
                    ImGui::Text("Controller fit (%s): the hands moved and turned from where the controller puts them (m, degrees):", nm);
                    bool ch = nudge3("Controller fit", fo, 0.005f, 0.1f);
                    ch |= nudge1("Controller fit pitch", &fa[0], 2.0f, -45.0f, 45.0f, "%.0f");
                    ch |= nudge1("Controller fit yaw", &fa[1], 2.0f, -45.0f, 45.0f, "%.0f");
                    ch |= nudge1("Controller fit roll", &fa[2], 2.0f, -45.0f, 45.0f, "%.0f");
                    if (ch) controllers::set_fit(1, fo, fa, true);
                } else {
                    ImGui::TextDisabled("Controller fit: no controller seen yet");
                }
            }
            body::HandCfg hc = body::hand_cfg();
            bool changed = false;
            if (ImGui::Checkbox("Arms follow the controllers", &hc.ik)) {
                changed = true;
                config::set("Hands", "ArmIK", hc.ik ? "1" : "0");
            }
            track("Arms follow the controllers");
            {  // 2026-10-07: the reticle where the shot lands
                bool rt = aim::reticle_on();
                if (ImGui::Checkbox("A reticle where the shot will land (while aiming)", &rt)) aim::set_reticle_on(rt);
                track("A reticle where the shot will land (while aiming)");
            }
            struct Slider {
                const char* label;
                const char* key;
                float* v;
                float lo, hi;
                const char* fmt;
            };
            if (ImGui::Checkbox("Arms stretch to reach the controllers", &hc.stretch)) {
                changed = true;
                config::set("Hands", "ArmStretch", hc.stretch ? "1" : "0");
            }
            track("Arms stretch to reach the controllers");
            const Slider sl[] = {{"Wrist offset (m)", "WristOffset", &hc.wrist_offset, -0.1f, 0.2f, "%.3f"},
                                 {"Hand pitch (deg)", "HandPitch", &hc.pitch, -90.0f, 90.0f, "%.0f"},
                                 {"Hand yaw (deg)", "HandYaw", &hc.yaw, -90.0f, 90.0f, "%.0f"},
                                 {"Hand roll (deg)", "HandRoll", &hc.roll, -180.0f, 180.0f, "%.0f"},
                                 {"Longest arm (times John's)", "ArmStretchMax", &hc.stretch_max, 1.0f, 1.6f, "%.2f"}};
            for (const Slider& x : sl) {
                if (ImGui::SliderFloat(x.label, x.v, x.lo, x.hi, x.fmt)) changed = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    char v[16];
                    std::snprintf(v, sizeof(v), x.fmt, *x.v);
                    config::set("Hands", x.key, v);
                }
                track(x.label);
            }
            int show = hc.show;
            ImGui::TextUnformatted("Body shown in first person:");
            if (ImGui::RadioButton("Whole body", show == 0)) show = 0;
            track("Whole body");
            ImGui::SameLine();
            if (ImGui::RadioButton("Forearms and hands", show == 1)) show = 1;
            track("Forearms and hands");
            ImGui::SameLine();
            if (ImGui::RadioButton("Hands only", show == 2)) show = 2;
            track("Hands only");
            if (show != hc.show) {
                hc.show = show;
                changed = true;
                config::set("Body", "Show", show == 1 ? "arms" : show == 2 ? "hands" : "full");
            }
            int hg = body::hidden_geometry();  // [Body] HiddenGeometry: how the hidden parts are drawn (the sliver)
            ImGui::TextUnformatted("Hidden parts:");
            ImGui::SameLine();
            if (ImGui::RadioButton("Collapsed", hg == 0)) body::set_hidden_geometry(0);
            track("Collapsed");
            ImGui::SameLine();
            if (ImGui::RadioButton("Skipped", hg == 1)) body::set_hidden_geometry(1);
            track("Skipped");
            ImGui::SameLine();
            if (ImGui::RadioButton("Filtered (no line between the elbows)", hg == 2)) body::set_hidden_geometry(2);
            track("Filtered (no line between the elbows)");
            bool as = body::auto_shows();
            if (ImGui::Checkbox("Forearms and hands when crouching, in cover or riding", &as)) {
                body::set_auto_shows(as);
                config::set("Body", "AutoShow", as ? "1" : "0");
            }
            track("Forearms and hands when crouching, in cover or riding");
            bool lt = body::locks_torso();
            if (ImGui::Checkbox("Keep the torso over the hips", &lt)) {
                body::set_locks_torso(lt);
                config::set("Body", "LockTorso", lt ? "1" : "0");
            }
            track("Keep the torso over the hips");
            bool dw = dual::enabled();
            if (ImGui::Checkbox("Dual wield (the free hand takes another holster's gun)", &dw)) dual::set_enabled(dw);
            track("Dual wield (the free hand takes another holster's gun)");
            bool dc = dual::copy_enabled();
            if (ImGui::Checkbox("Dual wield the same sidearm (the free hand at the other hip)", &dc)) dual::set_copy_enabled(dc);
            track("Dual wield the same sidearm (the free hand at the other hip)");
            bool om = dual::own_model();
            if (ImGui::Checkbox("  ... showing your other sidearm's model, when you own a different one", &om)) dual::set_own_model(om);
            track("  ... showing your other sidearm's model, when you own a different one");
            bool sh = dual::same_at_its_holster();  // round 13 item 10
            if (ImGui::Checkbox("  ... and a second of the gun in hand at its own holster", &sh)) dual::set_same_at_its_holster(sh);
            track("  ... and a second of the gun in hand at its own holster");
            bool kd = holster::keeps_drawn();
            if (ImGui::Checkbox("Keep the gun drawn (no automatic holster)", &kd)) {
                holster::set_keeps_drawn(kd);
                config::set("Hands", "KeepGunDrawn", kd ? "1" : "0");
            }
            track("Keep the gun drawn (no automatic holster)");
            bool be = aim::block_executions();  // round 13
            if (ImGui::Checkbox("No executions up close (the trigger always fires)", &be)) aim::set_block_executions(be);
            track("No executions up close (the trigger always fires)");
            if (changed) body::set_hand_cfg(hc);
            ImGui::EndTabItem();
        }
        bool tab_holsters = ImGui::BeginTabItem("Holsters");
        track("Holsters");  // the tab's own rect, selected or not
        if (tab_holsters) {
            bool on = holster::holsters_enabled();
            if (ImGui::Checkbox("Body holsters (grip at a holster to draw, grip and let go there to put away)", &on))
                holster::set_holsters_enabled(on);
            track("Body holsters (grip at a holster to draw, grip and let go there to put away)");
            bool rings = holster::show_zones();
            if (ImGui::Checkbox("Show the holsters (rings: green a hand in it, amber gripped, blue on the gun)", &rings))
                holster::set_show_zones(rings);
            track("Show the holsters (rings: green a hand in it, amber gripped, blue on the gun)");
            bool fists = holster::unarmed_after_holster();
            if (ImGui::Checkbox("Putting a gun away selects the fists", &fists)) holster::set_unarmed_after_holster(fists);
            track("Putting a gun away selects the fists");
            bool sg = holster::show_guns();
            if (ImGui::Checkbox("Show the guns at their holsters (a hip gun barrel down, a long gun across the back)", &sg)) holster::set_show_guns(sg);
            track("Show the guns at their holsters (a hip gun barrel down, a long gun across the back)");
            bool sb = holster::show_back_guns();  // round 13 item 14
            if (ImGui::Checkbox("  ... the long guns on the back too (off: the hips' only)", &sb)) holster::set_show_back_guns(sb);
            track("  ... the long guns on the back too (off: the hips' only)");
            {  // run 7 item 1d
                int an = holster::anchor();
                ImGui::TextUnformatted("The holsters follow:");
                ImGui::SameLine();
                if (ImGui::RadioButton("John's body", an == 0)) holster::set_anchor(0);
                track("John's body");
                ImGui::SameLine();
                if (ImGui::RadioButton("Your headset (they stay with you as you move in the room)", an == 1)) holster::set_anchor(1);
                track("Your headset (they stay with you as you move in the room)");
            }
            ImGui::TextUnformatted("Each holster: offset right / up / forward from the drawn holster (m), and its size. Each arrow press: 5 mm.");
            for (int z = 0; z < holster::zone_count(); ++z) {
                float off[3], radius = 0;
                holster::zone(z, off, &radius);
                ImGui::PushID(z);
                if (holster::zone_takes_weapons(z)) {
                    bool en = holster::zone_enabled(z);
                    if (ImGui::Checkbox("##on", &en)) holster::set_zone_enabled(z, en, true);
                    track((std::string(holster::zone_label(z)) + " on").c_str());
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(holster::zone_label(z));
                // round 13 item 9: arrows, a press one increment (saved at once)
                bool zc = nudge3((std::string(holster::zone_label(z)) + " offset").c_str(), off, 0.005f, 0.4f);
                zc |= nudge1((std::string(holster::zone_label(z)) + " size").c_str(), &radius, 0.005f, 0.05f, 0.4f);
                if (zc) holster::set_zone(z, off, radius, true);
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        // round 13 item 8: the gun in hand's own foregrip, foregrip ring and loading ring (else the Reloading tab's)
        bool tab_gun = ImGui::BeginTabItem("Gun in hand");
        track("Gun in hand");
        if (tab_gun) {
            const int w = holster::gun_in_hand();
            if (w < 0) {
                ImGui::TextUnformatted("Hold a gun to adjust its own foregrip and rings here (else the Reloading tab's, for every gun).");
            } else {
                ImGui::Text("%s: its own settings (else the Reloading tab's, for every gun). Each arrow press: 5 mm.", holster::weapon_label(w));
                static const char* const kName[3] = {"The foregrip: where John's front hand sits on it (right / up / forward, m)",
                                                     "The foregrip ring: where your front hand takes hold, and its size",
                                                     "The loading ring: where a round goes in, and its size"};
                static const char* const kTrack[3] = {"Gun foregrip", "Gun foregrip ring", "Gun loading ring"};
                for (int what = 0; what < 3; ++what) {
                    float off[3], r = 0.0f;
                    bool own = false;
                    holster::gun_adjust(w, what, off, &r, &own);
                    ImGui::Separator();
                    ImGui::Text("%s%s", kName[what], own ? "" : " (now every gun's)");
                    bool ch = nudge3(kTrack[what], off, 0.005f);
                    if (what > 0) {
                        char sl[64];
                        std::snprintf(sl, sizeof(sl), "%s size", kTrack[what]);
                        ch |= nudge1(sl, &r, 0.005f, 0.03f, 0.5f);
                    }
                    if (ch) holster::set_gun_adjust(w, what, off, r, true);
                    if (own) {
                        char bl[80];
                        std::snprintf(bl, sizeof(bl), "Use every gun's##%d", what);
                        if (ImGui::Button(bl)) holster::clear_gun_adjust(w, what);
                        std::snprintf(bl, sizeof(bl), "%s: every gun's", kTrack[what]);
                        track(bl);
                    }
                }
            }
            ImGui::EndTabItem();
        }
        bool tab_weapons = ImGui::BeginTabItem("Weapons");
        track("Weapons");
        if (tab_weapons) {
            bool wc = holster::weapon_choice();
            if (ImGui::Checkbox("Choose the weapon in each holster", &wc)) holster::set_weapon_choice(wc);
            track("Choose the weapon in each holster");
            bool any = holster::any_weapon();
            if (ImGui::Checkbox("List every weapon you own for every holster", &any)) holster::set_any_weapon(any);
            track("List every weapon you own for every holster");
            holster::Arsenal a{};
            const bool have = holster::arsenal(&a);
            if (!have) ImGui::TextDisabled("(your weapons are read in gameplay)");
            for (int z = 0; z < holster::zone_count(); ++z) {
                if (!holster::zone_takes_weapons(z)) continue;
                const int chosen = holster::zone_weapon(z);
                const std::string label = holster::zone_label(z);
                std::string preview = chosen < 0 ? std::string("Automatic (the game's weapon for it)") : holster::weapon_label(chosen);
                if (chosen >= 0 && have && !(a.owned >> chosen & 1)) preview += " (not owned: automatic)";
                ImGui::SetNextItemWidth(380);
                const bool open = ImGui::BeginCombo(label.c_str(), preview.c_str());
                if (!open) track(label.c_str());
                if (open) {
                    if (ImGui::Selectable("Automatic (the game's weapon for it)", chosen < 0)) holster::set_zone_weapon(z, -1, true);
                    track((label + ": Automatic").c_str());
                    for (int w = 0; have && w < holster::kWeapons; ++w) {
                        if (!(a.owned >> w & 1) || (!any && !holster::zone_fits(z, a.equip_slot[w]))) continue;
                        ImGui::PushID(w);
                        if (ImGui::Selectable(holster::weapon_label(w), chosen == w)) holster::set_zone_weapon(z, w, true);
                        ImGui::PopID();
                        track((label + ": " + holster::weapon_label(w)).c_str());
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::TextWrapped("A holster draws its weapon, and the game keeps it in that slot (its weapon wheel follows). With every weapon "
                               "listed, a gun still holsters where the game carries it (a rifle on the back).");
            ImGui::EndTabItem();
        }
        bool tab_reload = ImGui::BeginTabItem("Reloading");
        track("Reloading");  // the tab's own rect, selected or not
        if (tab_reload) {
            bool v = reload::hand_reload();
            if (ImGui::Checkbox("Reload by hand (grip a round at the chest, bring it to the gun)", &v)) reload::set_hand_reload(v);
            track("Reload by hand (grip a round at the chest, bring it to the gun)");
            bool rh = round_draw::enabled();
            if (ImGui::Checkbox("A round seen in the hand that holds it", &rh)) round_draw::set_enabled(rh);
            track("A round seen in the hand that holds it");
            {  // run 6 item 5: where the round sits in the hand
                round_draw::Place pl = round_draw::place();
                ImGui::TextUnformatted("  The round in the hand: moved from the pinch (m, the hand's axes; a press 2 mm), its direction, its brightness:");
                bool ch = nudge3("Round offset", pl.offset, 0.002f, 0.08f);  // round 13 item 9: arrows
                bool done = ch;
                const char* const kAxis[3] = {"Along the fingers", "Out of the hand", "Custom"};
                for (int i = 0; i < 3; ++i) {
                    if (i) ImGui::SameLine();
                    if (ImGui::RadioButton(kAxis[i], pl.axis_mode == i)) {
                        pl.axis_mode = i;
                        ch = done = true;
                    }
                    track(kAxis[i]);
                }
                {  // round 13 item 12: its direction as two angles from along the fingers, a press 5 degrees (Custom)
                    constexpr float kDeg = 57.29578f;
                    float turn = 0.0f, tilt = 0.0f;
                    if (pl.axis_mode != 1) {
                        turn = std::round(std::atan2(pl.axis[1], pl.axis[0]) * kDeg);
                        tilt = std::round(std::asin(std::fmin(1.0f, std::fmax(-1.0f, pl.axis[2]))) * kDeg);
                    }
                    bool ac = nudge1("Round turn (degrees)", &turn, 5.0f, -180.0f, 180.0f, "%.0f");
                    ac |= nudge1("Round tilt (degrees)", &tilt, 5.0f, -90.0f, 90.0f, "%.0f");
                    if (ac) {
                        pl.axis_mode = 2;
                        pl.axis[0] = std::cos(tilt / kDeg) * std::cos(turn / kDeg);
                        pl.axis[1] = std::cos(tilt / kDeg) * std::sin(turn / kDeg);
                        pl.axis[2] = std::sin(tilt / kDeg);
                        ch = done = true;
                    }
                }
                const bool bc = nudge1("Round brightness", &pl.bright, 0.25f, 0.25f, 3.0f);
                ch |= bc;
                done |= bc;
                if (ch) round_draw::set_place(pl, false);
                if (done) round_draw::set_place(pl, true);
            }
            bool sa_ = round_draw::show_ammo();
            if (ImGui::Checkbox("Show the rounds at the chest while a gun is open for them", &sa_)) round_draw::set_show_ammo(sa_);
            track("Show the rounds at the chest while a gun is open for them");
            bool ma = actions::enabled();
            if (ImGui::Checkbox("Work each gun's action by hand (revolver flick open/closed, bolt, lever flick, pump)", &ma)) actions::set_enabled(ma);
            track("Work each gun's action by hand (revolver flick open/closed, bolt, lever flick, pump)");
            bool oc = actions::open_cylinder();
            if (ImGui::Checkbox("  ... the open revolver drawn open (the Schofield and LeMat tip down, the Double-action swings out, the Cattleman's gate opens)", &oc))
                actions::set_open_cylinder(oc);
            track("  ... the open revolver drawn open (the Schofield and LeMat tip down, the Double-action swings out, the Cattleman's gate opens)");
            bool lp = actions::lever_parts();
            if (ImGui::Checkbox("  ... the lever guns' levers worked by your flick (down opens, up closes; the gun stays still)", &lp))
                actions::set_lever_parts(lp);
            track("  ... the lever guns' levers worked by your flick (down opens, up closes; the gun stays still)");
            bool pp = actions::pump_parts();
            if (ImGui::Checkbox("  ... the Pump-action's fore-end in your front hand (pull it back and push it forward after a shot; the gun's own pumping removed)", &pp))
                actions::set_pump_parts(pp);
            track("  ... the Pump-action's fore-end in your front hand (pull it back and push it forward after a shot; the gun's own pumping removed)");
            bool bo = actions::bolt_parts();
            if (ImGui::Checkbox("  ... the bolt actions' bolts in your other hand (lift the handle, pull back, push forward, turn it down)", &bo))
                actions::set_bolt_parts(bo);
            track("  ... the bolt actions' bolts in your other hand (lift the handle, pull back, push forward, turn it down)");
            bool br = actions::breech_parts();
            if (ImGui::Checkbox("  ... the single-shot rifles opened by hand (the Springfield's trapdoor and the Rolling Block's block by your other hand, the Buffalo's by a flick down and up)", &br))
                actions::set_breech_parts(br);
            track("  ... the single-shot rifles opened by hand (the Springfield's trapdoor and the Rolling Block's block by your other hand, the Buffalo's by a flick down and up)");
            bool sa = actions::semi_auto_parts();
            if (ImGui::Checkbox("  ... the Semi-Auto Shotgun's bolt racked by your other hand after loading it from empty", &sa))
                actions::set_semi_auto_parts(sa);
            track("  ... the Semi-Auto Shotgun's bolt racked by your other hand after loading it from empty");
            bool mb = actions::manual_break();
            if (ImGui::Checkbox("  ... the Double-barrel and the Sawed-off broken open by the stick's flick (shut: flick it up, or swing the barrels up with the other hand)", &mb))
                actions::set_manual_break(mb);
            track("  ... the Double-barrel and the Sawed-off broken open by the stick's flick (shut: flick it up, or swing the barrels up with the other hand)");
            bool bs = actions::barrel_hand_snap();
            if (ImGui::Checkbox("      ... the other hand drawn on the barrels while it swings them up", &bs)) actions::set_barrel_hand_snap(bs);
            track("      ... the other hand drawn on the barrels while it swings them up");
            bool ps = actions::part_hand_snap();
            if (ImGui::Checkbox("  ... the other hand drawn on the bolt, the breech's part or the semi-auto's handle it grips", &ps))
                actions::set_part_hand_snap(ps);
            track("  ... the other hand drawn on the bolt, the breech's part or the semi-auto's handle it grips");
            bool hc = actions::held_click();  // round 13 item 13
            if (ImGui::Checkbox("  ... the gun clicks when fired before its action is worked", &hc)) actions::set_held_click(hc);
            track("  ... the gun clicks when fired before its action is worked");
            {
                int sq = reload::chest_squeeze();
                ImGui::TextUnformatted("The gun hand squeezed at the chest holster:");
                const char* const kSq[3] = {"Does nothing", "Loads one round", "Reloads fully"};
                for (int i = 0; i < 3; ++i) {
                    ImGui::SameLine();
                    if (ImGui::RadioButton(kSq[i], sq == i)) reload::set_chest_squeeze(i);
                    track(kSq[i]);
                }
            }
            v = reload::automatic();
            if (ImGui::Checkbox("The game's automatic reloads", &v)) reload::set_automatic(v);
            track("The game's automatic reloads");
            v = reload::button();
            if (ImGui::Checkbox("The game's reload button", &v)) reload::set_button(v);
            track("The game's reload button");
            v = reload::two_handed();
            if (ImGui::Checkbox("Two-handed long guns (grip the foregrip with the front hand)", &v)) reload::set_two_handed(v);
            track("Two-handed long guns (grip the foregrip with the front hand)");
            v = holster::foregrip_snap();
            if (ImGui::Checkbox("The front hand snaps onto the gun (the game's grip)", &v)) holster::set_foregrip_snap(v, true);
            track("The front hand snaps onto the gun (the game's grip)");
            {
                float fo[3], fr = 0;
                holster::foregrip(fo, &fr);
                ImGui::TextUnformatted("Foregrip: where John's front hand sits on the gun, right / up / forward from the game's grip (m; a press 5 mm):");
                if (nudge3("Foregrip offset", fo, 0.005f, 0.3f)) holster::set_foregrip(fo, fr, true);  // round 13 item 9: arrows
                // run 6 item 4: the ring where your front hand takes the gun, moved and sized apart from the grip
                float zo[3], zr = 0;
                holster::foregrip_zone(zo, &zr);
                ImGui::TextUnformatted("The foregrip ring: where your front hand takes hold, moved from the grip (m), and its size (Holsters tab: show it):");
                bool zch = nudge3("Foregrip ring offset", zo, 0.005f, 0.3f);
                zch |= nudge1("Foregrip ring size", &zr, 0.005f, 0.03f, 0.4f);
                if (zch) holster::set_foregrip_zone(zo, zr, true);
            }
            {  // run 6 item 3: the loading point
                float lo[3], lr = 0;
                bool touch = false;
                holster::load_point(lo, &lr, &touch);
                ImGui::TextUnformatted("Where a round goes in: right / up / forward from the gun's loading point (m), and the ring's size:");
                bool ch = nudge3("Loading point offset", lo, 0.005f, 0.3f);  // round 13 item 9: arrows
                ch |= nudge1("Loading point size", &lr, 0.005f, 0.03f, 0.4f);
                ch |= ImGui::Checkbox("A round goes in as soon as it touches the ring (the round, not the hand)", &touch);
                track("A round goes in as soon as it touches the ring (the round, not the hand)");
                if (ch) holster::set_load_point(lo, lr, touch, true);
            }
            {
                ImGui::TextUnformatted("An empty gun:");
                const int em = static_cast<int>(audio::empty_click_mode());
                const char* const names[4] = {"Clicks", "Buzzes", "Clicks and buzzes", "Does nothing"};
                for (int i = 0; i < 4; ++i) {
                    ImGui::SameLine();
                    if (ImGui::RadioButton(names[i], em == i)) audio::set_empty_click_mode(static_cast<audio::ClickMode>(i));
                    track(names[i]);
                }
                float vol = audio::click_volume();
                ImGui::SetNextItemWidth(260);
                if (ImGui::SliderFloat("Click volume", &vol, 0.0f, 1.0f, "%.2f")) audio::set_click_volume(vol, false);
                if (ImGui::IsItemDeactivatedAfterEdit()) {  // saved, and one click at the new level: the gun in hand's
                    audio::set_click_volume(vol, true);
                    RdrvrActorState st{};
                    audio::preview(api::actor_state(&st) ? st.weapon : -1);
                }
                track("Click volume");
            }
            ImGui::EndTabItem();
        }
        bool tab_gestures = ImGui::BeginTabItem("Gestures");
        track("Gestures");  // the tab's own rect, selected or not
        if (tab_gestures) {
            bool hc = physics::enabled();
            if (ImGui::Checkbox("Hands push loose objects (bottles, crates, chairs)", &hc)) physics::set_enabled(hc);
            track("Hands push loose objects (bottles, crates, chairs)");
            bool gb = physics::grab_enabled();
            if (ImGui::Checkbox("Grab loose objects with an empty hand (let go to throw)", &gb)) physics::set_grab_enabled(gb);
            track("Grab loose objects with an empty hand (let go to throw)");
            bool v = gestures::throw_by_hand();
            if (ImGui::Checkbox("Throw by hand (dynamite, fire bottles, knives leave with your hand's speed)", &v))
                gestures::set_throw_by_hand(v);
            track("Throw by hand (dynamite, fire bottles, knives leave with your hand's speed)");
            v = gestures::melee_by_swing();
            if (ImGui::Checkbox("Melee by swing (a fast swing punches, or stabs with the knife)", &v)) gestures::set_melee_by_swing(v);
            track("Melee by swing (a fast swing punches, or stabs with the knife)");
            v = gestures::lasso_by_hand();
            if (ImGui::Checkbox("Lasso by hand (swing forward to throw, yank back to pull)", &v)) gestures::set_lasso_by_hand(v);
            track("Lasso by hand (swing forward to throw, yank back to pull)");
            float sw = gestures::swing_speed(), tg = gestures::throw_gain();
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderFloat("Swing speed (m/s) for a punch or the lasso", &sw, 1.0f, 8.0f, "%.1f")) gestures::set_tuning(sw, tg, false);
            if (ImGui::IsItemDeactivatedAfterEdit()) gestures::set_tuning(sw, tg, true);
            track("Swing speed (m/s) for a punch or the lasso");
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderFloat("Throw strength (times your hand's speed)", &tg, 0.5f, 4.0f, "%.2f")) gestures::set_tuning(sw, tg, false);
            if (ImGui::IsItemDeactivatedAfterEdit()) gestures::set_tuning(sw, tg, true);
            track("Throw strength (times your hand's speed)");
            ImGui::EndTabItem();
        }
        bool tab_controls = ImGui::BeginTabItem("Controls");
        track("Controls");  // the tab's own rect, selected or not
        if (tab_controls) {
            bool lh = controls::left_handed();
            if (ImGui::Checkbox("Left-handed (swap the controllers)", &lh)) controls::set_left_handed(lh);
            track("Left-handed (swap the controllers)");
            bool gih = body::gun_in_gun_hand();
            if (ImGui::Checkbox("Left-handed: the gun in John's left hand (each arm on its own controller)", &gih)) body::set_gun_in_gun_hand(gih);
            track("Left-handed: the gun in John's left hand (each arm on its own controller)");
            bool ta = controls::trigger_aims();
            if (ImGui::Checkbox("The trigger alone fires (no left trigger needed)", &ta)) controls::set_trigger_aims(ta);
            track("The trigger alone fires (no left trigger needed)");
            bool ar = controls::aim_when_raised();
            if (ImGui::Checkbox("Aim while the gun is raised (left trigger held for you)", &ar)) controls::set_aim_when_raised(ar);
            track("Aim while the gun is raised (left trigger held for you)");
            bool sd = controls::sprint_drops_aim();
            if (ImGui::Checkbox("Sprinting lowers the raised gun (on foot)", &sd)) controls::set_sprint_drops_aim(sd);
            track("Sprinting lowers the raised gun (on foot)");
            if (aim::spawn_hooked()) {
                bool pa = aim::perfect_accuracy();
                if (ImGui::Checkbox("Perfect accuracy (every shot leaves along the barrel)", &pa)) aim::set_perfect_accuracy(pa);
                track("Perfect accuracy (every shot leaves along the barrel)");
                bool sp = aim::shotgun_pattern();
                if (ImGui::Checkbox("Shotgun pellets spread (off: every pellet on one line)", &sp)) aim::set_shotgun_pattern(sp);
                track("Shotgun pellets spread (off: every pellet on one line)");
            } else {
                ImGui::TextUnformatted("Perfect accuracy: unavailable (the spawn hook is not installed)");
            }
            bool dg = controls::draw_to_grabbing_hand();
            if (ImGui::Checkbox("Draw into the hand that grabs the holster", &dg)) controls::set_draw_to_grabbing_hand(dg);
            track("Draw into the hand that grabs the holster");
            ImGui::TextUnformatted("Each button drives this game button:");
            const char* names[controls::kTargets];
            for (int g = 0; g < controls::kTargets; ++g) names[g] = controls::target_name(g);
            for (int s = 0; s < controls::kSources; ++s) {
                int cur = controls::mapping(s);
                ImGui::SetNextItemWidth(220);
                if (ImGui::Combo(controls::source_label(s), &cur, names, controls::kTargets)) controls::set_mapping(s, cur);
                track(controls::source_label(s));
            }
            if (ImGui::Button("Reset to the game's scheme")) controls::reset_mapping();
            track("Reset to the game's scheme");
            ImGui::TextUnformatted("Menu button: tap = pause, hold = this menu. Without one (Index): both B buttons.");
            ImGui::EndTabItem();
        }
        bool tab_screen = ImGui::BeginTabItem("Screen");
        track("Screen");  // the tab's own rect, selected or not
        if (tab_screen) {
            int m = g_set.cut3d ? 1 : 0;
            if (ImGui::RadioButton("Cutscenes on the screen", m == 0)) m = 0;
            track("Cutscenes on the screen");
            if (ImGui::RadioButton("Cutscenes in 3D", m == 1)) m = 1;
            track("Cutscenes in 3D");
            if ((m == 1) != g_set.cut3d) {
                g_set.cut3d = m == 1;
                vr_mode::set_cutscene_3d(g_set.cut3d);
                config::set("Screen", "CutsceneMode", g_set.cut3d ? "3D" : "Screen");
            }
            if (ImGui::SliderFloat("3D cutscene separation", &g_set.separation, 0.0f, 1.0f, "%.2f"))
                vr_mode::set_separation(g_set.separation);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char v[16];
                std::snprintf(v, sizeof(v), "%.2f", g_set.separation);
                config::set("Screen", "Cutscene3DSeparation", v);
            }
            track("3D cutscene separation");
            ImGui::EndTabItem();
        }
        bool tab_debug = ImGui::BeginTabItem("Debug");
        track("Debug");  // the tab's own rect, selected or not
        if (tab_debug) {
            char line[400];
            xr::perf_status(line, sizeof(line));
            ImGui::TextWrapped("%s", line);
            vr_mode::status_text(line, sizeof(line));
            ImGui::TextWrapped("%s", line);
            ImGui::Separator();
            for (int i = 0; i < 13; ++i) {
                char label[48];
                std::snprintf(label, sizeof(label), "Split %s", kSplits[i]);
                if (ImGui::Checkbox(label, &g_set.split[i])) {
                    dual_pass::set_split(kSplits[i], g_set.split[i]);
                    char key[32];
                    std::snprintf(key, sizeof(key), "Split_%s", kSplits[i]);
                    config::set("Stereo", key, g_set.split[i] ? "1" : "0");
                }
                track(label);
                if (i % 3 != 2) ImGui::SameLine(static_cast<float>((i % 3 + 1) * kW / 3));
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

bool render(ID3D12Resource* dst) {
    ID3D12CommandQueue* q = state::present_queue.load();
    ID3D12Device* dev = state::device.load();
    int s = g_slot;
    if (g_fence->GetCompletedValue() < g_slot_value[s]) {
        g_fence->SetEventOnCompletion(g_slot_value[s], g_event);
        if (WaitForSingleObject(g_event, 50) != WAIT_OBJECT_0) return false;
    }
    if (FAILED(g_alloc[s]->Reset()) || FAILED(g_list->Reset(g_alloc[s], nullptr))) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE rt = g_rtv_heap->GetCPUDescriptorHandleForHeapStart(), img = rt;
    img.ptr += g_rtv_step;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = g_quad_fmt;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(dst, &rv, img);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    const float clear[4] = {0, 0, 0, 0};
    g_list->ClearRenderTargetView(rt, clear, 0, nullptr);
    g_list->OMSetRenderTargets(1, &rt, FALSE, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srv_heap);
    {
        d3d::InternalSubmitScope internal;  // the backend may upload its textures on the queue
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->SetPipelineState(g_pso);
    g_list->SetGraphicsRootSignature(g_root);
    g_list->SetDescriptorHeaps(1, &g_srv_heap);
    g_list->SetGraphicsRootDescriptorTable(0, g_rt_srv);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp{0, 0, static_cast<float>(kW), static_cast<float>(kH), 0, 1};
    D3D12_RECT sc{0, 0, kW, kH};
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);
    g_list->OMSetRenderTargets(1, &img, FALSE, nullptr);
    g_list->DrawInstanced(3, 1, 0, 0);
    if (FAILED(g_list->Close())) return false;
    d3d::submit_internal(q, g_list);
    g_slot_value[s] = ++g_fence_value;
    q->Signal(g_fence, g_fence_value);
    g_slot = (s + 1) % kSlots;
    return true;
}

}  // namespace

void init() {}

bool visible() { return g_visible.load(); }

void set_visible(bool on) {
    if (on && !g_visible.load()) g_place = true;
    g_visible = on;
    log::info("[menu] %s", on ? "open" : "closed");
}

bool frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* quad) {
    if (controls::take_menu_toggle()) set_visible(!g_visible.load());  // the menu button held (controls.cpp)
    {  // F7 while the game is in front: the menu toggled (as Scroll Lock recentres: polled, the game still sees the key)
        static bool f7_was = false;
        bool f7 = false;
        DWORD pid = 0;
        if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !f7_was) {
            log::info("[menu] F7");
            set_visible(!g_visible.load());
        }
        f7_was = f7;
    }
    if (!g_visible.load() || !ensure(session)) return false;
    if (g_place.exchange(false)) place(views);
    ImGuiIO& io = ImGui::GetIO();
    float px = -FLT_MAX, py = -FLT_MAX;
    // the right hand points, or the left when the right misses; that hand's trigger clicks
    int hand = 1;
    bool hit = laser(1, &px, &py);
    if (!hit && laser(0, &px, &py)) {
        hit = true;
        hand = 0;
    }
    bool pressed = hit && hands::get(hand).trigger > 0.5f;
    io.AddMousePosEvent(hit ? px : -FLT_MAX, hit ? py : -FLT_MAX);
    io.AddMouseButtonEvent(0, pressed);
    {
        d3d::InternalSubmitScope internal;
        ImGui_ImplDX12_NewFrame();
    }
    ImGui::NewFrame();
    draw();
    // the pointer (headset round 4: without it the menu was hard to aim at): a ring with a dot, filled while pressed
    if (hit) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImVec2 at(px, py);
        ImU32 col = pressed ? IM_COL32(255, 200, 60, 255) : IM_COL32(255, 255, 255, 235);
        fg->AddCircle(at, 20.0f, IM_COL32(0, 0, 0, 200), 40, 8.0f);
        fg->AddCircle(at, 20.0f, col, 40, 4.0f);
        fg->AddCircleFilled(at, pressed ? 13.0f : 6.0f, col, 24);
    }
    ImGui::Render();
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    if (XR_FAILED(xrAcquireSwapchainImage(g_sc, &ai, &index))) return false;
    bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(g_sc, &wi)) && index < g_images.size() && render(g_images[index]);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_sc, &ri);
    if (!ok) return false;
    *quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad->space = space;
    quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad->subImage.swapchain = g_sc;
    quad->subImage.imageRect = {{0, 0}, {kW, kH}};
    quad->pose = g_pose;
    quad->size = {kQuadW, kQuadH};
    g_frames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void scroll(float y) { g_scroll_req.store(y < 0.0f ? 0.0f : y); }

std::string find(const std::string& label) {
    std::lock_guard lock(g_find_mutex);
    auto it = g_rects.find(label);
    if (it == g_rects.end()) return "ERROR no widget \"" + label + "\" drawn (is the menu open on that tab?)";
    float cx = 0.5f * (it->second.x0 + it->second.x1), cy = 0.5f * (it->second.y0 + it->second.y1);
    float local[3] = {(cx / kW - 0.5f) * kQuadW, (0.5f - cy / kH) * kQuadH, 0}, w[3];
    rotate(g_pose_shared.orientation, local, w);
    char out[200];
    // then its row in the window (pixels from the top, of kH) and the window's scroll: a row past the bottom is not seen
    std::snprintf(out, sizeof(out), "%.4f %.4f %.4f row %.0f of %d scroll %.0f", g_pose_shared.position.x + w[0], g_pose_shared.position.y + w[1],
                  g_pose_shared.position.z + w[2], cy, kH, g_scroll_now.load(std::memory_order_relaxed));
    return out;
}

void status_text(char* out, size_t len) {
    std::snprintf(out, len, "menu %s, %s, frames %llu, clicks %llu", g_visible.load() ? "open" : "closed",
                  g_ready ? "ready" : "not made", static_cast<unsigned long long>(g_frames.load()),
                  static_cast<unsigned long long>(g_clicks.load()));
}

}  // namespace rdrvr::menu
