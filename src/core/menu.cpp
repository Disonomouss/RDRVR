#include "core/menu.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/actions.h"
#include "core/aim.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/body.h"
#include "core/xr_blit.h"
#include "core/holster.h"
#include "core/reload.h"
#include "core/gestures.h"
#include "core/gun_melee.h"
#include "core/offhand.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/deadeye.h"
#include "core/dual.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/d3d_util.h"
#include "core/dual_pass.h"
#include "core/playback.h"
#include "core/eye_shape.h"
#include "core/hands.h"
#include "core/launcher_start.h"
#include "core/log.h"
#include "core/pgo.h"
#include "core/physics.h"
#include "core/pose.h"
#include "core/render_settings.h"
#include "core/state.h"
#include "core/ui_layer.h"
#include "core/vr_mode.h"
#include "core/wheel.h"
#include "core/whistle.h"
#include "core/xr.h"
#include "core/round_draw.h"
#include "core/render_res.h"
#include "core/pso.h"
#include "core/scope.h"
#include "core/vi_auto.h"

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
// run 7 item 5: the gun hand's stick scrolls the window under the laser (presenting thread): px wanted this frame, the
// fraction carried to the next (ImGui rounds a scroll to whole pixels), and the pixels scrolled so (the status line)
constexpr float kStickDead = 0.2f, kScrollSpeed = 500.0f;  // the stick's deadzone; px/s at full tilt
float g_stick_px = 0.0f, g_stick_rem = 0.0f;
std::atomic<uint64_t> g_stick_scrolled{0};
struct Rect {
    float x0, y0, x1, y1;
    float v0, v1;  // the rows of its window that are seen (the page scrolls under the tabs)
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

using d3d_util::fence_reached;  // (d3d_util.h: the event reset before it is armed, the value checked after the wait)

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
        const pso::OwnCreation own_pso;  // (run 19: [Stereo] InstancedShadersAuto leaves the mod's own pipelines alone)
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
    if (XR_FAILED(pso::as_own([&] { return xrCreateSwapchain(session, &ci, &g_sc); }))) {
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
    // run 7 item 5: a scrollbar the laser can take and drag (22 px was 2 cm on the 1 m quad)
    ImGui::GetStyle().ScrollbarSize = 40.0f;
    ImGui::GetStyle().GrabMinSize = 40.0f;
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
        const pso::OwnCreation own_pso;  // (ImGui's backend makes its pipelines here)
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
    const float v0 = ImGui::GetWindowPos().y, v1 = v0 + ImGui::GetWindowHeight();
    {
        std::lock_guard lock(g_find_mutex);
        g_rects[label] = {a.x, a.y, b.x, b.y, v0, v1};
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

// run 7 item 5: the tabs as buttons, each its page in a child window that scrolls under them, and a help line under
// the page: the explanation of the row under the laser. 2026-10-13 (the menu's clean up): nine tabs, one row.
// 2026-10-29: a tenth, Gunfights (Ped Gunfight Overhaul's settings, pgo.h), shown only while that mod is installed; the
// tabs shown in one row while they fit, else in two
enum Page : int { kDisplay, kComfort, kBody, kHolsters, kGuns, kReloading, kGestures, kControls, kAdvanced, kGunfights, kPages };
const char* const kPageName[kPages] = {"Display", "Comfort", "Body", "Holsters", "Guns", "Reloading", "Gestures", "Controls", "Advanced",
                                       "Gunfights"};
bool page_shown(int p) { return p != kGunfights || pgo::installed(); }
constexpr float kTabH = 44.0f;    // a tab button's height (px)
constexpr float kLabelW = 210.0f; // nudge1: its arrows from here (the label before), the value kValueW wide
constexpr float kValueW = 150.0f;
int g_page = kDisplay;            // presenting thread
bool g_page_changed = false;      // a new page opens at its top
std::string g_help;               // this frame's help line (the row under the laser)
std::atomic<float> g_scroll_max{0.0f};
std::atomic<int> g_page_shared{kDisplay};
const char kIdleHelp[] = "Point at a setting to read about it here. Your gun hand's stick scrolls the page under the pointer.";

// A label's part shown (before "##") and its name for track() and the tests (after "##", else the whole label), as
// ImGui's own "label##id": a short label keeps a test's name
std::string shown_part(const char* label) {
    const char* h = std::strstr(label, "##");
    return h ? std::string(label, h) : std::string(label);
}
const char* track_name(const char* label) {
    const char* h = std::strstr(label, "##");
    return h ? h + 2 : label;
}

// The help line's text while the laser is on the last item (a disabled one too)
void help(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) g_help = text;
}

void heading(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

void note(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

bool check(const char* label, bool* v, const char* tip) {
    const bool changed = ImGui::Checkbox(label, v);
    track(track_name(label));
    help(tip);
    return changed;
}

bool radio(const char* label, bool active, const char* tip) {
    const bool pressed = ImGui::RadioButton(label, active);
    track(track_name(label));
    help(tip);
    return pressed;
}

// A parent's sub-options: indented under it and disabled while it is off
void sub_begin(bool parent_on) {
    ImGui::Indent();
    ImGui::BeginDisabled(!parent_on);
}
void sub_end() {
    ImGui::EndDisabled();
    ImGui::Unindent();
}

// A group's reset (a gun's "Use every gun's", the buttons' scheme): its last row, at the right, on every tab
bool reset_button(const char* label, bool enabled, const char* tip) {
    const std::string shown = shown_part(label);
    const float w = ImGui::CalcTextSize(shown.c_str()).x + 2.0f * ImGui::GetStyle().FramePadding.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    track(track_name(label));
    help(tip);
    return pressed;
}

// 2026-10-13: a button that acts on a second click within 5 s (a reset): the first turns its label to "Click again to
// confirm"; tracked by `name` either way
bool confirm_button(const char* label, const char* name, const char* tip) {
    static std::string armed;
    static double armed_ms = 0.0;
    const double now = log::now_ms();
    const bool is_armed = armed == name && now - armed_ms < 5000.0;
    const std::string shown = (is_armed ? std::string("Click again to confirm") : std::string(label)) + "###" + name;  // (### : one ID for both labels)
    bool done = false;
    if (ImGui::Button(shown.c_str())) {
        if (is_armed) {
            armed.clear();
            done = true;
        } else {
            armed = name;
            armed_ms = now;
        }
    }
    track(name);
    help(tip);
    return done;
}

// round 13: an adjustment by arrows (sliders were hard to set): for right, up and forward a left and a right arrow,
// one step a press, the value between; true when a press changed it. Each arrow tracked as "<name> <axis> minus" /
// "plus", the name the label's part after "##" (else the label). Run 7 item 5: the row's help line, every
// adjustment by arrows (no sliders left)
bool nudge3(const char* label, float v[3], float step, float lim = 1.0f, const char* tip = nullptr, const char* const* axes = nullptr,
            const char* vfmt = "%s %+.3f") {
    static const char* const kPlace[3] = {"right", "up", "forward"};
    const char* const* kAxis = axes ? axes : kPlace;
    const char* nm = track_name(label);
    bool changed = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(shown_part(label).c_str());
    for (int i = 0; i < 3; ++i) {
        char name[160];
        ImGui::PushID(i);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##dn", ImGuiDir_Left)) v[i] -= step, changed = true;
        std::snprintf(name, sizeof(name), "%s %s minus", nm, kAxis[i]);
        track(name);
        ImGui::SameLine();
        ImGui::Text(vfmt, kAxis[i], v[i]);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Right)) v[i] += step, changed = true;
        std::snprintf(name, sizeof(name), "%s %s plus", nm, kAxis[i]);
        track(name);
        ImGui::PopID();
        if (changed) v[i] = std::fmin(lim, std::fmax(-lim, std::round(v[i] / step) * step));
    }
    ImGui::EndGroup();
    help(tip);
    ImGui::PopID();
    return changed;
}

// one value by arrows, as nudge3; the arrows and the value in columns
bool nudge1(const char* label, float* v, float step, float lo, float hi, const char* fmt = "%.3f", const char* tip = nullptr) {
    const char* nm = track_name(label);
    bool changed = false;
    char name[160];
    ImGui::PushID(label);
    ImGui::BeginGroup();
    const std::string shown = shown_part(label);
    ImGui::TextUnformatted(shown.c_str());
    // a label longer than the column moves its arrows past it (round 37: "The world's speed meanwhile" ran under them)
    const float col = std::fmax(kLabelW, ImGui::CalcTextSize(shown.c_str()).x + ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine(col);
    if (ImGui::ArrowButton("##dn", ImGuiDir_Left)) *v -= step, changed = true;
    std::snprintf(name, sizeof(name), "%s minus", nm);
    track(name);
    ImGui::SameLine();
    ImGui::Text(fmt, *v);
    ImGui::SameLine(col + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + kValueW);
    if (ImGui::ArrowButton("##up", ImGuiDir_Right)) *v += step, changed = true;
    std::snprintf(name, sizeof(name), "%s plus", nm);
    track(name);
    ImGui::EndGroup();
    help(tip);
    ImGui::PopID();
    if (changed) *v = std::fmin(hi, std::fmax(lo, std::round(*v / step) * step));
    return changed;
}

// 2026-10-29 (the Gunfights page): as nudge1, its arrows also held, as Ped Gunfight Overhaul's own keys: a press one
// `step`; held past 0.5 s, five steps a second, the step ten times larger after each 2 s held (1, 10, 100 ...; 0.01,
// 0.1 ... for a decimal)
bool nudge_held(const char* label, double* v, double step, double lo, double hi, const char* fmt, const char* tip) {
    static ImGuiID held = 0;
    static double held_ms = 0.0, next_ms = 0.0;
    const char* nm = track_name(label);
    bool changed = false;
    char name[160];
    const double now = log::now_ms();
    auto arrow = [&](const char* id, ImGuiDir dir, double sign) {
        ImGui::ArrowButton(id, dir);
        const ImGuiID me = ImGui::GetItemID();
        if (ImGui::IsItemActivated()) {
            held = me;
            held_ms = now;
            next_ms = now + 500.0;
            *v += sign * step;
            changed = true;
        } else if (ImGui::IsItemActive() && held == me && now >= next_ms) {
            next_ms = now + 200.0;
            *v += sign * step * std::pow(10.0, std::floor((now - held_ms) / 2000.0));
            changed = true;
        }
    };
    ImGui::PushID(label);
    ImGui::BeginGroup();
    const std::string shown = shown_part(label);
    ImGui::TextUnformatted(shown.c_str());
    const float col = std::fmax(kLabelW, ImGui::CalcTextSize(shown.c_str()).x + ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine(col);
    arrow("##dn", ImGuiDir_Left, -1.0);
    std::snprintf(name, sizeof(name), "%s minus", nm);
    track(name);
    ImGui::SameLine();
    ImGui::Text(fmt, *v);
    ImGui::SameLine(col + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + kValueW);
    arrow("##up", ImGuiDir_Right, 1.0);
    std::snprintf(name, sizeof(name), "%s plus", nm);
    track(name);
    ImGui::EndGroup();
    help(tip);
    ImGui::PopID();
    if (changed) *v = std::fmin(hi, std::fmax(lo, std::round(*v / step) * step));
    return changed;
}

// The stick's scroll of the current window while the laser is on it (ImGui's hover: the pointer's last position); the
// whole pixels now, the fraction kept
void stick_scroll() {
    if (g_stick_px == 0.0f || !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        g_stick_rem = 0.0f;
        return;
    }
    g_stick_rem += g_stick_px;
    const float whole = std::trunc(g_stick_rem);
    if (whole == 0.0f) return;
    g_stick_rem -= whole;
    const float was = ImGui::GetScrollY(), to = std::fmin(ImGui::GetScrollMaxY(), std::fmax(0.0f, was + whole));
    if (to == was) return;
    ImGui::SetScrollY(to);
    g_stick_scrolled.fetch_add(static_cast<uint64_t>(std::fabs(to - was)), std::memory_order_relaxed);
}

void page_buttons() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (!page_shown(g_page)) {  // (the Gunfights page's mod removed meanwhile)
        g_page = kDisplay;
        g_page_changed = true;
        g_page_shared.store(g_page, std::memory_order_relaxed);
    }
    int shown[kPages], ns = 0;
    for (int p = 0; p < kPages; ++p)
        if (page_shown(p)) shown[ns++] = p;
    // (2026-10-13: each tab as wide as its name, the room left shared out: "Reloading" was cut short at an even width)
    float text_w[kPages], all_text = 0.0f;
    for (int k = 0; k < ns; ++k) all_text += text_w[k] = ImGui::CalcTextSize(kPageName[shown[k]]).x + 2.0f * st.FramePadding.x;
    const int row1_n = all_text + static_cast<float>(ns - 1) * st.ItemSpacing.x <= avail ? ns : (ns + 1) / 2;
    float row_text[2] = {0.0f, 0.0f};
    for (int k = 0; k < ns; ++k) row_text[k < row1_n ? 0 : 1] += text_w[k];
    for (int k = 0; k < ns; ++k) {
        const int p = shown[k];
        const bool row1 = k < row1_n;
        const int n = row1 ? row1_n : ns - row1_n, i = row1 ? k : k - row1_n;
        const float spare = std::fmax(0.0f, avail - static_cast<float>(n - 1) * st.ItemSpacing.x - row_text[row1 ? 0 : 1]);
        const float w = std::floor(text_w[k] + spare / static_cast<float>(n));
        if (i > 0) ImGui::SameLine();
        const bool sel = g_page == p;
        ImGui::PushStyleColor(ImGuiCol_Button, st.Colors[sel ? ImGuiCol_TabSelected : ImGuiCol_Tab]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, st.Colors[ImGuiCol_TabHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, st.Colors[ImGuiCol_TabSelected]);
        if (ImGui::Button(kPageName[p], ImVec2(w, kTabH)) && !sel) {
            g_page = p;
            g_page_changed = true;
            g_page_shared.store(p, std::memory_order_relaxed);
        }
        ImGui::PopStyleColor(3);
        track(kPageName[p]);  // the tab's own rect, selected or not
    }
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

void save_float(const char* section, const char* key, const char* fmt, float v) {
    char b[32];
    std::snprintf(b, sizeof(b), fmt, v);
    config::set(section, key, b);
}

// 2026-10-13 (the user: "The mod menu needs a clean up", then "make all the changes you have proposed"): nine tabs in
// one row, each setting where a player looks for it; the rows whose off side has no value to a player (test aids, the
// rendering fixes, the stereo splits) are kept in the ini only; a row that needs a restart says so in its label, an
// untried one is marked experimental; the tips in plain words, short enough for the help line
const char kRestart[] = " (restart)";

void page_display() {
    heading("Picture");
    // DLSS is chosen for the next start: switching to or from it while the game runs would re-create its swapchain
    const char* aa[] = {"Off", "FXAA", "Native TAA", "DLSS (restart)"};
    int a = g_set.aa < 0 ? 1 : g_set.aa > 3 ? 3 : g_set.aa;
    ImGui::SetNextItemWidth(380);
    if (ImGui::Combo("Anti-aliasing", &a, aa, 4)) {
        g_set.aa = a;
        if (a <= 2) render_settings::set_aa(a);  // refused (kept for the next start) while DLSS runs
        config::set("Render", "ForceAntiAliasing", std::to_string(a));
    }
    track("Anti-aliasing");
    help("How edges are smoothed. FXAA is the default; Native TAA is the game's own. DLSS (NVIDIA RTX cards) is the "
         "sharpest; switching to or from it needs a restart.");
    // (GOAL-single-pass-default.md item 3) single pass or two-pass at the next start ([Stereo] SinglePass), and this
    // session's: config::single_pass_state() and the converter's progress
    {
        static int pick = -1;
        if (pick < 0) pick = config::get_string("Stereo", "SinglePass", "1") == "0" ? 1 : 0;
        const char* rm[] = {"Single pass (faster)", "Two-pass"};
        ImGui::SetNextItemWidth(380);
        if (ImGui::Combo("Rendering (restart)##Rendering", &pick, rm, 2)) config::set("Stereo", "SinglePass", pick == 0 ? "1" : "0");
        track("Rendering");
        help("Single pass draws both eyes from one walk of the scene, about a millisecond a frame faster; the shaders it needs "
             "are converted from your own game files in the background at the first start. Two-pass draws each eye on its own.");
        char line[300];
        const int st = config::single_pass_state();
        const vi_auto::Progress pr = vi_auto::progress();
        const bool folder = !config::get_string("Stereo", "InstancedShaders", "").empty();
        if (st == 2) {
            std::snprintf(line, sizeof(line), "Now: two-pass (the shader converter is missing: install RDRVR again).");
        } else if (st == 3) {
            std::snprintf(line, sizeof(line), "Now: two-pass (your settings file sets %s=0: tick Single pass in the launcher to write the "
                          "single-pass keys again).", config::single_pass_off_key().c_str());
        } else if (st == 0 || config::get_int("Stereo", "PrepassViews", 0) != 2) {
            std::snprintf(line, sizeof(line), "Now: two-pass.");
        } else if (folder) {
            std::snprintf(line, sizeof(line), "Now: single pass (the shaders from your folder).");
        } else if (!pr.on) {
            std::snprintf(line, sizeof(line), "Now: two-pass in effect (the shader conversion could not start: see the log).");
        } else if (!pr.converter) {
            std::snprintf(line, sizeof(line), "Now: single pass, but the converter stopped (%s): new shaders are drawn as two passes.",
                          pr.why.empty() ? "see the log" : pr.why.c_str());
        } else {
            std::snprintf(line, sizeof(line), "Now: single pass; %zu shaders ready%s%s, %llu not convertible (drawn as two passes)%s.", pr.ready,
                          pr.queue ? ", converting " : "", pr.queue ? std::to_string(pr.queue).c_str() : "",
                          static_cast<unsigned long long>(pr.refused), pr.failed ? ", some failed (see the log)" : "");
        }
        note(line);
    }
    if (g_set.aa == 3) {  // the game's quality index: what DLSS renders before it upscales to the game's resolution
        sub_begin(true);
        static const char* const kQ[6] = {"DLAA (native resolution)", "Dynamic (DLAA here)", "Ultra performance (1/3 per axis)",
                                          "Performance (1/2 per axis)", "Balanced (0.58 per axis)", "Quality (2/3 per axis)"};
        int q = g_set.dlss_q < 0 ? 0 : g_set.dlss_q > 5 ? 5 : g_set.dlss_q;
        ImGui::SetNextItemWidth(380);
        if (ImGui::Combo("DLSS quality (restart)##DLSS quality", &q, kQ, 6)) {
            g_set.dlss_q = q;
            config::set("Render", "DlssQuality", std::to_string(q));
        }
        track("DLSS quality");
        help("How much DLSS renders before it upscales. DLAA (native) is the sharpest and costs the most; the others are faster "
             "and softer.");
        sub_end();
    }
    {  // [XR] Sharpness (run 14): AMD CAS in the eye blit
        float sh = xr_blit::sharpness() * 100.0f;
        if (nudge1("Sharpening", &sh, 5.0f, 0.0f, 100.0f, "%.0f%%",
                   "Sharpens each eye's image in the headset (AMD CAS), bringing back detail DLSS and TAA soften. 0 = off."))
            xr_blit::set_sharpness(sh / 100.0f, true);
    }
    const int running = render_settings::forced_aa();
    if (running >= 0 && ((running == 3) != (g_set.aa == 3) || (running == 3 && g_set.dlss_q != render_settings::dlss_quality())))
        note("Restart the game to apply.");
    // [XR] EyeShape (run 7 item 4): with FXAA only (the mode stands aside for the other techniques)
    sub_begin(g_set.aa == 1);
    bool eye = eye_shape::enabled();
    if (check("Eyes in the headset's shape", &eye,
              "Each eye drawn in your headset's own shape instead of a stretched 16:9 frame: fewer pixels for the same "
              "sharpness. With FXAA only. Turned on now, it fully applies after a restart.")) {
        if (!eye_shape::set_enabled(eye, true)) note("Not available on this game build (see RDRVR.log).");
    }
    sub_end();
    // [XR] EyeShapeDlss (2026-10-10): the same under DLSS, from the next start
    sub_begin(g_set.aa == 3);
    bool eyed = eye_shape::dlss_saved();
    if (check("Eyes in the headset's shape with DLSS (restart)##Eyes in the headset's shape with DLSS", &eyed,
              "The same with DLSS: each eye drawn smaller in the headset's shape, then upscaled by DLSS. About a quarter less GPU "
              "time for the same sharpness."))
        eye_shape::set_dlss_next_start(eyed);
    if (g_set.aa == 3 && render_settings::forced_aa() == 3 && eye_shape::dlss_saved() != eye_shape::dlss_running())
        note("Restart the game to apply.");
    sub_end();
    bool mb = !render_settings::motion_blur_off();  // run 10 items 8c and 8d
    if (check("Motion blur", &mb,
              "The game's motion blur. It smears the view as you turn your head, so it is off by default. On: the game's own "
              "graphics setting decides."))
        render_settings::set_motion_blur_off(!mb, true);
    bool haze = render_settings::heat_haze_on();
    if (check("Heat haze", &haze, "The desert's heat shimmer. Some find it uncomfortable in VR."))
        render_settings::set_heat_haze(haze, true);

    // [Render] RenderResolution: the game's frame at a headset's height, apart from the monitor (from the next start)
    heading("Resolution (restart)");
    {  // 2026-10-09: the headset first, then its size at 100%, 150% or 200% of its pixels
        const int cur = render_res::choice(), sc = render_res::scale();
        char why[192];
        const std::string preview = cur >= 0 ? render_res::headset_label(cur) : std::string("Custom (the ini's RenderResolution)");
        ImGui::TextUnformatted("Render resolution: headset");
        ImGui::SetNextItemWidth(560);
        const bool open = ImGui::BeginCombo("##Render headset", preview.c_str(), ImGuiComboFlags_HeightLargest);  // all its rows seen
        if (!open) track("Render resolution: headset");
        help("The size the game renders at, matched to your headset. Automatic uses the size your VR runtime (SteamVR, Pimax "
             "Play, Virtual Desktop...) asks for. Windowed mode keeps the window's size.");
        if (open) {
            stick_scroll();  // the list, when it is longer than the window (the page under it is not hovered)
            for (int i = 0; i < render_res::headset_count(); ++i) {
                // a headset is chosen at the scale already picked, else at 100% when that one does not fit
                int s = i == 0 ? 0 : (sc >= 0 ? sc : 0);
                if (i > 0 && !render_res::choice_allowed(i, s, why, sizeof(why))) s = 0;
                const bool ok = render_res::choice_allowed(i, s, why, sizeof(why));
                ImGui::PushID(i);
                if (ImGui::Selectable(render_res::headset_label(i), cur == i, ok ? 0 : ImGuiSelectableFlags_Disabled) && ok)
                    render_res::set_choice(i, s);
                ImGui::PopID();
                track(render_res::headset_label(i));
                help(why[0] ? why : nullptr);
            }
            ImGui::EndCombo();
        }
        sub_begin(cur > 0);  // the size: not for the game's own or a custom size
        {
            const std::string sp = cur > 0 ? render_res::scale_label(cur, sc) : std::string("-");
            ImGui::TextUnformatted("Render resolution: size");
            ImGui::SetNextItemWidth(560);
            const bool sopen = cur > 0 && ImGui::BeginCombo("##Render size", sp.c_str());
            if (!sopen) track("Render resolution: size");
            help("100% is your headset's own resolution; 150% and 200% render more pixels (each side about 1.22 and 1.41 times) "
                 "for a sharper picture at a higher cost.");
            if (sopen) {
                for (int s = 0; s < render_res::scale_count(); ++s) {
                    const bool ok = render_res::choice_allowed(cur, s, why, sizeof(why));
                    const std::string lab = render_res::scale_label(cur, s);  // (one static buffer a scale: copied)
                    ImGui::PushID(100 + s);
                    if (ImGui::Selectable(lab.c_str(), sc == s, ok ? 0 : ImGuiSelectableFlags_Disabled) && ok) render_res::set_choice(cur, s);
                    ImGui::PopID();
                    track(lab.c_str());
                    help(why[0] ? why : nullptr);
                }
                ImGui::EndCombo();
            }
        }
        sub_end();
        char st[256];
        render_res::status_text(st, sizeof(st));
        note((std::string("Now: ") + st).c_str());
        if (cur >= 0 && render_res::choice_allowed(cur, sc, why, sizeof(why)) && why[0]) note(why);
        // Automatic: the runtime's own size for its lenses; a note, not the list's help (the open list covers the help line)
        if (cur == 1)
            note("Automatic uses the size your runtime asks for. Keep the runtime's own resolution setting at its default "
                 "(SteamVR: 100%), or the two multiply.");
        uint32_t want_w = 0, want_h = 0, run_w = 0, run_h = 0;
        const bool want = cur >= 0 && render_res::choice_size(cur, sc, &want_w, &want_h);
        const bool run = render_res::active(&run_w, &run_h);
        if (xr::frame_resized()) note("The game's frame changed size while running (its Graphics menu): restart the game for the VR view.");
        else if (want != run || (want && (want_w != run_w || want_h != run_h))) note("Restart the game to apply.");
    }

    heading("Performance");
    {  // the performance presets (2026-10-11): each sets the rows above and the resolution's cap; most apply from the next start
        static int chosen = -1;
        const char* const kP[3] = {"Quality", "Balanced", "Performance"};
        const char* const kTip[3] = {
            "The sharpest: eyes in the headset's shape, DLSS at DLAA, the runtime's full size and refresh rate. For a fast PC.",
            "Eyes in the headset's shape, DLSS Quality, at most 2880 pixels tall. For most PCs.",
            "Eyes in the headset's shape, DLSS Performance (without DLSS: 85% size), at most 2208 tall, 72 Hz where offered. For a "
            "slower PC."};
        ImGui::TextUnformatted("Preset:");
        for (int p = 0; p < 3; ++p) {
            ImGui::SameLine();
            if (ImGui::Button(kP[p])) {
                chosen = p;
                eye_shape::set_enabled(true, true);
                eye_shape::set_dlss_next_start(true);
                dual_pass::set_split("shadows", true);
                config::set("Stereo", "Split_shadows", "1");
                g_set.dlss_q = p == 0 ? 0 : p == 1 ? 5 : 3;  // DLAA, Quality, Performance
                config::set("Render", "DlssQuality", std::to_string(g_set.dlss_q));
                config::set("Render", "AutoMaxHeight", p == 0 ? "0" : p == 1 ? "2880" : "2208");
                config::set("XR", "EyeScale", p == 2 && g_set.aa != 3 ? "0.85" : "1.0");
                float rates[16];
                const int n = xr::refresh_rates(rates, 16);
                float hz = 0.0f;
                for (int i = 0; p == 2 && i < n; ++i)
                    if (std::fabs(rates[i] - 72.0f) < 0.5f) hz = rates[i];
                if (p < 2 || hz > 0.0f) xr::set_refresh_rate_want(hz);
                log::info("[menu] the %s preset chosen", kP[p]);
            }
            track((std::string("Preset ") + kP[p]).c_str());
            help(kTip[p]);
        }
        if (chosen >= 0) note("Set. The eye shape, DLSS and the resolution apply from the next start; the refresh rate now.");
        float mp = 0.0f, hz = 0.0f;
        if (xr::struggling(&mp, &hz))
            note(("This PC is missing frames here (" + std::to_string(static_cast<int>(mp + 0.5f)) + "% of them, " +
                  std::to_string(static_cast<int>(hz + 0.5f)) + " frames a second): the Balanced or Performance preset may help.").c_str());
    }
    {  // [XR] RefreshRate: the headset's refresh rate, where the runtime lets the mod choose it
        float rates[16];
        const int n = xr::refresh_rates(rates, 16);
        if (n > 0) {
            const float want = xr::refresh_rate_want();
            ImGui::TextUnformatted("Headset refresh rate:");
            const char* const tip = "Lower rates are easier on your PC. A frame that misses the rate halves it (90 to 45), so 72 or "
                                    "80 can feel smoother on a slower PC. The runtime's: as your headset is set.";
            ImGui::SameLine();
            if (radio("The runtime's", want <= 0.0f, tip)) xr::set_refresh_rate_want(0.0f);
            for (int i = 0; i < n; ++i) {
                char lab[24];
                std::snprintf(lab, sizeof(lab), "%.0f Hz##rr%d", rates[i], i);
                ImGui::SameLine();
                if (radio(lab, want > 0.0f && std::fabs(want - rates[i]) < 0.5f, tip)) xr::set_refresh_rate_want(rates[i]);
            }
            char now[64];
            std::snprintf(now, sizeof(now), "Now: %.0f Hz", xr::refresh_rate_now());
            note(now);
        } else if (n == 0) {
            note("Headset refresh rate: the runtime offered no rates.");
        } else {
            note("Headset refresh rate: set it in your headset's or runtime's own settings.");
        }
    }

    heading("HUD");
    bool wrist = xr::hud_on_wrist();
    if (check("HUD on the left wrist", &wrist,
              "The radar and the ammo counter on the back of your left hand (the right when left-handed), shown when you look at "
              "it like a watch. Prompts stay on the floating screen."))
        xr::set_hud_on_wrist(wrist);
    sub_begin(wrist);  // 2026-10-09: [Controls] WristSatchel
    {  // 2026-11-04: [XR] WristHudShape (a player's piece of the radar's rim floating above the wrist: rect to compare)
        const bool rd = xr::hud_shape_radar();
        const char* kShapeTip = "What goes to the wrist. The radar and ammo count only: anything else in that corner, such as a subtitle, "
                                "stays on the floating screen. The whole corner: the screen's lower-left corner as a rectangle.";
        if (radio("The radar and ammo count##wrist shape radar", rd, kShapeTip) && !rd) xr::set_hud_shape_radar(true);
        ImGui::SameLine();
        if (radio("The whole corner##wrist shape rect", !rd, kShapeTip) && rd) xr::set_hud_shape_radar(false);
    }
    bool ws = controls::wrist_satchel();
    if (check("A double tap of Y at the wrist opens the satchel", &ws,
              "With the wrist HUD in view, two quick presses of Y open John's satchel; B closes it. A single press still reaches the "
              "game, a third of a second late. Holding Y opens this menu either way."))
        controls::set_wrist_satchel(ws);
    sub_end();

    heading("Cutscenes");
    int m = g_set.cut3d ? 1 : 0;
    if (radio("On the cinema screen", m == 0, "Cutscenes on a floating cinema screen, as the game frames them.")) m = 0;
    ImGui::SameLine();
    if (radio("In 3D", m == 1, "Cutscenes all around you in 3D, seen from the cutscene's camera; your head only turns the view.")) m = 1;
    if ((m == 1) != g_set.cut3d) {
        g_set.cut3d = m == 1;
        vr_mode::set_cutscene_3d(g_set.cut3d);
        config::set("Screen", "CutsceneMode", g_set.cut3d ? "3D" : "Screen");
    }
    sub_begin(g_set.cut3d);
    if (nudge1("3D separation", &g_set.separation, 0.05f, 0.0f, 1.0f, "%.2f",
               "How strong the 3D is: 1.00 matches your own eyes, lower is flatter (a press 0.05).")) {
        vr_mode::set_separation(g_set.separation);
        save_float("Screen", "Cutscene3DSeparation", "%.2f", g_set.separation);
    }
    sub_end();
    bool fpm = camera_lever::first_person_moments();  // 2026-10-12: [Body] FirstPersonMoments
    if (check("Short scenes in first person", &fpm,
              "Short scripted moments (looting, skinning, doors, chests, beds, a mission's quick shots) stay at John's eyes "
              "instead of cutting to third person. Full cutscenes still follow the choice above. Needs First person."))  {
        camera_lever::set_first_person_moments(fpm);
        config::set("Body", "FirstPersonMoments", fpm ? "1" : "0");
    }

    heading("Sound");  // 2026-10-13: [XR] AudioFollowsHead, AudioRearFold (0.8.8, ini only until now)
    bool ah = camera_lever::audio_follows_head();
    if (check("Sound follows your head", &ah,
              "Sounds are placed around your head as you turn it, so you can tell where they come from. Off: around the way John "
              "faces."))  {
        camera_lever::set_audio_follows_head(ah);
        config::set("XR", "AudioFollowsHead", ah ? "1" : "0");
    }
    bool rf = camera_lever::audio_rear_fold();
    if (check("Sounds behind you at full volume", &rf,
              "On stereo headphones the game plays sounds behind you much quieter (about 10 dB), so people behind you seem far "
              "away. On: they are as loud as in front. Surround (5.1 / 7.1) devices keep the game's mix."))  {
        camera_lever::set_audio_rear_fold(rf);
        config::set("XR", "AudioRearFold", rf ? "1" : "0");
    }
}

void page_comfort() {
    heading("The view");
    if (ImGui::Button("Recentre")) camera_lever::recentre();
    track("Recentre");
    help("Puts the view, John's facing, the cinema screen and the HUD in front of you again. Also: your headset's own recentre, "
         "or both stick clicks held a second.");
    bool fp = pose::anchor_enabled();
    if (check("First person", &fp,
              "The view from John's eyes, moved by your head. Off: the game's own camera, turned by your head.")) {
        pose::set_anchor(fp);
        config::set("Body", "CameraAnchor", fp ? "1" : "0");
    }
    float lift = pose::eye_lift();
    if (nudge1("Eye height", &lift, 0.01f, -0.2f, 0.3f, "%+.2f m", "Raises or lowers your view above John's eyes (a press 1 cm).")) {
        pose::set_eye_lift(lift);
        save_float("Body", "SeatedLift", "%.2f", lift);
    }
    float ws = camera_lever::world_scale();  // 2026-10-13: [Comfort] WorldScale
    if (nudge1("World scale", &ws, 0.05f, 0.5f, 2.0f, "%.2fx",
               "How big the world looks: above 1 bigger (you feel smaller), below 1 smaller. 1.00 is true scale. Only your view and "
               "hands change: the game's world, aiming and holsters stay the same (a press 0.05)."))
        camera_lever::set_world_scale(ws, true);

    heading("Walking in your room");  // 2026-10-27 ([XR] RoomScale; the user: "Implement roomscale and physical crouch")
    bool rs = pose::room_scale();
    if (check("Room-scale walking", &rs,
              "Step in your room and John walks under you, so your view moves only with your head. He turns to face your step, "
              "walks (never runs) and is put right under you at the end; your own stick always wins. Where he cannot follow (a wall, a fence, someone in the way) he stops and your view fades "
              "to black if you step on. On foot only: riding and cutscenes are as before. Mind ledges: a step toward one walks him "
              "off it."))
        pose::set_room_scale(rs);
    // (2026-11-03, round 73 check 8, the user: "Move 'body moves with the headset' to roomscale settings and it will be
    // useable") [Body] BodyFollowsHead, from the body's rows; its own switch, room-scale on or off
    bool follow = body::follows_head();
    if (check("Body moves with the headset", &follow,
              "John's body moves with you when you lean or step in your room, so it stays under your view; with room-scale "
              "walking, until he walks after you. Off: it stays where John stands.")) {
        body::set_follows_head(follow);
        config::set("Body", "BodyFollowsHead", follow ? "1" : "0");
    }
    sub_begin(rs);
    {  // (2026-11-02, GOAL-older-items item 1) [XR] RoomScaleDirect
        bool rd = pose::room_direct();
        if (check("Small steps at once", &rd,
                  "A small step (up to 20 cm) puts John right under you at once, without turning or walking, where nothing is in the "
                  "way; longer steps, or with a wall between, he walks as before."))
            pose::set_room_direct(rd, true);
    }
    float rdead = 0.15f, rstop = 0.06f, rmin = 0.38f, rmax = 0.45f, roff = 0.65f;  // (the defaults; room_tuning fills them)
    pose::room_tuning(&rdead, &rstop, &rmin, &rmax, &roff);
    if (nudge1("Step before he walks", &rdead, 0.05f, 0.05f, 0.40f, "%.2f m",
               "How far you can move from John before he walks after you; inside it your view simply follows your head. A step "
               "already moving away starts him a little sooner (a press 5 cm)."))
        pose::set_room_tuning(rdead, rstop, rmin, rmax, roff, true);
    float rspeed = rmax * 100.0f;
    if (nudge1("His walking pace", &rspeed, 5.0f, 40.0f, 60.0f, "%.0f%% of the stick",
               "How hard the walk pushes the stick at most: 45% is the game's walk (a press 5%). Higher catches up sooner."))
        pose::set_room_tuning(rdead, rstop, rmin, rspeed / 100.0f, roff, true);
    bool rfade = pose::room_fade();
    if (check("Fade out when he is blocked", &rfade,
              "Your view fades to black as you step past where John stopped (a wall, a fence, someone in the way), instead of "
              "passing through it. Off: the view is held 65 cm from him."))
        pose::set_room_fade(rfade);
    sub_end();
    // 2026-10-27 ([XR] BodyFollowsTurn; the user: "Turning physically will turn johns body right?", "Add it after room
    // scale works.")
    bool bt = pose::body_follows_turn();
    if (check("John turns when you turn", &bt,
              "Turn your body in your room and John turns with you, smoothly, so he faces where you face; your view does not move "
              "with it. A look to the side does not turn him: only a turn past the angle below, held for the time below. Not "
              "while riding, in cover, in a menu or the wheel, or with a long gun held in both hands."))
        pose::set_body_follows_turn(bt);
    // 2026-10-28 (round 37 check 7, the user: the turn "lags behind"): its three keys in the menu (were ini only)
    sub_begin(bt);
    float bdead = 35.0f, bhold = 100.0f, bspeed = 300.0f;  // (the defaults; body_turn_tuning fills them)
    pose::body_turn_tuning(&bdead, &bhold, &bspeed);
    if (nudge1("Turn past", &bdead, 5.0f, 20.0f, 90.0f, "%.0f deg",
               "How far your head may turn from John's body before he turns after you: a look to the side inside it never turns "
               "him (a press 5 degrees)."))
        pose::set_body_turn_tuning(bdead, bhold, bspeed, true);
    if (nudge1("Hold the turn", &bhold, 50.0f, 0.0f, 1000.0f, "%.0f ms",
               "How long your head must stay turned past that angle before he follows: longer tells a glance aside from a turn "
               "more surely, shorter follows sooner (a press 50 ms)."))
        pose::set_body_turn_tuning(bdead, bhold, bspeed, true);
    if (nudge1("His turn speed", &bspeed, 30.0f, 60.0f, 360.0f, "%.0f deg/s",
               "How fast he turns at most; he slows as he comes round to face you (a press 30 degrees a second)."))
        pose::set_body_turn_tuning(bdead, bhold, bspeed, true);
    sub_end();

    heading("Crouching");  // 2026-10-27 ([XR] PhysicalCrouch; the user: "Implement roomscale and physical crouch")
    bool pc = pose::physical_crouch();
    if (check("Physical crouch", &pc,
              "Crouch for real and John crouches; stand up and he stands. Your view follows your own head while you crouch. A "
              "look at the ground or a bend is not a crouch. Not while riding, in cover or in a menu. Sitting down counts as a "
              "crouch: when you sit, stand John up first (L3), then press \"Set my standing height\" seated (a recentre keeps "
              "the standing one). The left stick click still crouches too, and both "
              "stick clicks (the recentre) no longer crouch John on foot."))
        pose::set_physical_crouch(pc);
    sub_begin(pc);
    float depth = pose::crouch_share() * 100.0f;
    if (nudge1("Crouch depth", &depth, 5.0f, 15.0f, 50.0f, "%.0f%% of your height",
               "How far your head must drop to crouch John, as a share of your standing height (a press 5%). You stand him up "
               "again a little above it."))
        pose::set_crouch_share(depth / 100.0f);
    // 2026-10-28 (round 37 check 1, the user: the crouch "lags behind player"): the hold in the menu (was ini only)
    float chold = pose::crouch_hold_ms();
    if (nudge1("Crouch hold", &chold, 25.0f, 50.0f, 600.0f, "%.0f ms",
               "How long your head must stay past the depth before John crouches (or back up before he stands): a quick dip or a "
               "nod does nothing. The further past the depth, the sooner: a quick, deep duck crouches him (a press 25 ms)."))
        pose::set_crouch_hold(chold);
    const double st_in = pose::standing_calibrate_in();
    if (st_in > 0.0) {
        char sb[96];
        std::snprintf(sb, sizeof(sb), "Stand straight and look ahead: %.0f s", st_in / 1000.0 + 0.5);
        note(sb);
    } else if (st_in == 0.0) {
        note("Stand straight and look ahead: waiting for your headset's tracking...");
    } else {
        if (const char* m = pose::standing_calibrate_message()) note(m);
        if (ImGui::Button("Set my standing height")) pose::calibrate_standing(3000.0);
        track("Set my standing height");
        help("Press, then stand straight and look ahead for 3 seconds: your crouch is measured from there (seated: sit and "
             "look ahead). Not while John is crouched: stand him up (L3) first. A recentre while standing sets it too.");
        bool floor = false;
        const float sh = pose::standing_height(&floor);
        // (2026-10-28: the user's floor gave 1.21-1.24 m: a standing eye height under 1.40 m from the floor said as low)
        char hb[320];
        std::snprintf(hb, sizeof(hb), "Your standing eye height: %.2f m (%s)%s", sh, floor ? "from the headset's floor" : "no floor from the headset",
                      floor && sh < 1.40f ? ". That is low for standing straight: if your headset's floor is set too high, set it again "
                                            "in the headset; the crouch depth is a share of this height" : "");
        note(hb);
    }
    sub_end();

    heading("Turning on the right stick");
    int turn = pose::snap_turning() ? 1 : 0;
    if (radio("Smooth turning", turn == 0, "The right stick turns you smoothly, at the turn speed.")) turn = 0;
    ImGui::SameLine();
    if (radio("Snap turning", turn == 1, "The right stick turns you in steps of the snap angle.")) turn = 1;
    if ((turn == 1) != pose::snap_turning()) {
        pose::set_turning(turn == 1, pose::turn_speed(), pose::snap_angle());
        config::set("Comfort", "TurnMode", turn ? "snap" : "smooth");
    }
    float speed = pose::turn_speed(), angle = pose::snap_angle();
    sub_begin(turn == 0);
    if (nudge1("Turn speed", &speed, 10.0f, 30.0f, 360.0f, "%.0f deg/s", "How fast smooth turning turns you at full tilt (a press 10).")) {
        pose::set_turning(pose::snap_turning(), speed, angle);
        config::set("Comfort", "TurnSpeed", std::to_string(static_cast<int>(speed)));
    }
    sub_end();
    sub_begin(turn == 1);
    if (nudge1("Snap angle", &angle, 5.0f, 10.0f, 90.0f, "%.0f deg", "How far each snap turns you (a press 5 degrees).")) {
        pose::set_turning(pose::snap_turning(), speed, angle);
        config::set("Comfort", "SnapAngle", std::to_string(static_cast<int>(angle)));
    }
    sub_end();

    heading("Riding");
    int steer = pose::steer_by_head() ? 1 : 0;
    if (radio("Steer with the stick", steer == 0, "The view faces the horse and the left stick steers it; you can look around freely."))
        steer = 0;
    ImGui::SameLine();
    if (radio("Steer with the head", steer == 1, "As on foot: the left stick rides where you look.")) steer = 1;
    if ((steer == 1) != pose::steer_by_head()) pose::set_steer_by_head(steer == 1);
    sub_begin(steer == 0);
    bool rturn = pose::stick_turn_riding();
    if (check("The right stick turns the view while riding", &rturn,
              "The right stick turns your view as on foot while the left stick steers. Mounting or a recentre faces the horse "
              "again."))
        pose::set_stick_turn_riding(rturn);
    sub_end();
    {  // (2026-11-02, GOAL-older-items item 25) [Horse] MountRecentre
        bool mr = xr::mount_recentre();
        if (check("Recentre on getting on", &mr, "Getting on a horse, onto a wagon or into a vehicle recentres your view (as holding both stick clicks): you face the mount's forward."))
            xr::set_mount_recentre(mr, true);
    }
    bool saddle = pose::saddle_anchor();
    float stau = pose::saddle_smoothing();
    if (check("View from the saddle", &saddle,
              "A steady view above the saddle instead of John's bouncing head."))
        pose::set_saddle(saddle, stau, true);
    sub_begin(saddle);
    bool climb = pose::saddle_climb();
    if (check("Keep its height on climbs", &climb, "On a steep climb the view keeps its height instead of sinking toward the saddle."))
        pose::set_saddle_climb(climb, true);
    if (nudge1("Saddle smoothing", &stau, 0.05f, 0.0f, 0.6f, "%.2f s", "How much the view's height is smoothed (0 = none; a press 0.05 s)."))
        pose::set_saddle(saddle, stau, true);
    sub_end();
    bool cb = controls::click_brake();
    if (check("Left stick click brakes", &cb,
              "On a horse or a wagon, hold the left stick click to slow down (with the stick pulled back for a hard stop). On "
              "foot it is still crouch."))
        controls::set_click_brake(cb);
}

void page_body() {
    heading("John's body");
    {
        body::HandCfg hc = body::hand_cfg();
        int show = hc.show;
        ImGui::TextUnformatted("Shown:");
        const char* const kShowTip = "How much of John you see in first person.";
        ImGui::SameLine();
        if (radio("Whole body", show == 0, kShowTip)) show = 0;
        ImGui::SameLine();
        if (radio("Forearms and hands", show == 1, kShowTip)) show = 1;
        ImGui::SameLine();
        if (radio("Hands only", show == 2, kShowTip)) show = 2;
        if (show != hc.show) {
            hc.show = show;
            body::set_hand_cfg(hc);
            config::set("Body", "Show", show == 1 ? "arms" : show == 2 ? "hands" : "full");
        }
    }
    bool as = body::auto_shows();
    if (check("Forearms and hands when crouching, in cover or riding", &as,
              "Only the forearms and hands are drawn while crouching, in cover, riding or driving, where the body gets in the way.")) {
        body::set_auto_shows(as);
        config::set("Body", "AutoShow", as ? "1" : "0");
    }
    bool hide = body::hide_enabled();
    if (check("Hide head and hat", &hide, "John's head and hat hidden in first person (cutscenes still show them).")) {
        body::set_hide_enabled(hide);
        config::set("Body", "HideHead", hide ? "1" : "0");
    }
    int st = body::stance();
    {
        const char* const kStanceTip = "John's torso: upright over the hips, held still under your view (only the arms and legs "
                                       "move), only its lean removed, or as the game animates it.";
        ImGui::TextUnformatted("Torso:");
        ImGui::SameLine();
        if (radio("Upper body upright", st == 1, kStanceTip)) st = 1;
        ImGui::SameLine();
        if (radio("Still", st == 3, kStanceTip)) st = 3;
        ImGui::SameLine();
        if (radio("Remove lean only", st == 2, kStanceTip)) st = 2;
        ImGui::SameLine();
        if (radio("Game animation", st == 0, kStanceTip)) st = 0;
        if (st != body::stance()) {
            body::set_stance(st);
            config::set("Body", "Stance", st == 0 ? "game" : st == 2 ? "lean" : st == 3 ? "still" : "upright");
        }
    }
    sub_begin(st == 1 || st == 3);
    float pitch = body::torso_pitch();
    if (nudge1("Torso pitch", &pitch, 1.0f, -20.0f, 20.0f, "%+.0f deg", "Leans the upright torso forward (positive) or back.")) {
        body::set_torso_pitch(pitch);
        config::set("Body", "TorsoPitch", std::to_string(static_cast<int>(pitch)));
    }
    sub_end();
    float back = body::body_back();
    if (nudge1("Body back", &back, 0.01f, 0.0f, 0.4f, "%.2f m",
               "Draws the body further behind your view, so looking down shows the chest rather than the inside (a press 1 cm).")) {
        body::set_body_back(back);
        save_float("Body", "BodyBack", "%.2f", back);
    }
    bool lt = body::locks_torso();
    if (check("Keep the torso over the hips", &lt, "Running no longer pushes John's upper body forward into your view.")) {
        body::set_locks_torso(lt);
        config::set("Body", "LockTorso", lt ? "1" : "0");
    }
    bool lockf = body::locks_facing();
    if (check("Lock body facing", &lockf, "The body, legs too, faces where you face: walking sideways no longer turns his shoulder into view.")) {
        body::set_locks_facing(lockf);
        config::set("Body", "LockFacing", lockf ? "1" : "0");
    }

    heading("John's arms");
    body::HandCfg hc = body::hand_cfg();
    bool changed = false;
    ImGui::BeginDisabled(!hc.ik);  // (its switch, Arms follow the controllers, in the Advanced tab)
    if (check("Arms stretch to reach the controllers", &hc.stretch,
              "Past John's reach his arm lengthens so his hand stays on your controller.")) {
        changed = true;
        config::set("Hands", "ArmStretch", hc.stretch ? "1" : "0");
    }
    sub_begin(hc.stretch);
    if (nudge1("Longest arm", &hc.stretch_max, 0.05f, 1.0f, 1.6f, "%.2fx John's",
               "How far an arm may stretch, times its length; past it the hand stops short of the controller.")) {
        changed = true;
        save_float("Hands", "ArmStretchMax", "%.2f", hc.stretch_max);
    }
    sub_end();
    ImGui::EndDisabled();
    if (!hc.ik) note("Off: Arms follow the controllers (the Advanced tab).");
    if (nudge1("Wrist offset", &hc.wrist_offset, 0.005f, -0.1f, 0.2f, "%.3f m",
               "How far John's wrist sits behind your controller's grip (a press 5 mm).")) {
        changed = true;
        save_float("Hands", "WristOffset", "%.3f", hc.wrist_offset);
    }
    {
        const char* const kTurnTip = "Turns John's hand against your controller until it sits as yours does (degrees, a press 2; "
                                     "the left hand mirrored).";
        if (nudge1("Hand pitch", &hc.pitch, 2.0f, -90.0f, 90.0f, "%+.0f deg", kTurnTip)) {
            changed = true;
            save_float("Hands", "HandPitch", "%.0f", hc.pitch);
        }
        if (nudge1("Hand yaw", &hc.yaw, 2.0f, -90.0f, 90.0f, "%+.0f deg", kTurnTip)) {
            changed = true;
            save_float("Hands", "HandYaw", "%.0f", hc.yaw);
        }
        if (nudge1("Hand roll", &hc.roll, 2.0f, -180.0f, 180.0f, "%+.0f deg", kTurnTip)) {
            changed = true;
            save_float("Hands", "HandRoll", "%.0f", hc.roll);
        }
    }
    if (changed) body::set_hand_cfg(hc);

    {  // 2026-10-07: how the controller sits in the hand, per controller type ([Controls] GripFit<Type>)
        float fo[3], fa[3];
        const char* nm = controllers::fit_name(1);
        if (nm && controllers::fit(1, fo, fa)) {
            char hd[128];
            std::snprintf(hd, sizeof(hd), "Controller fit: %s (a press 5 mm or 2 deg)", nm);
            heading(hd);
            const char* const kFitTip = "How this kind of controller sits in your hand: John's hands moved and turned from where the "
                                        "controller puts them (the left mirrored).";
            bool ch = nudge3("Offset##Controller fit", fo, 0.005f, 0.1f, kFitTip);
            ch |= nudge1("Pitch##Controller fit pitch", &fa[0], 2.0f, -45.0f, 45.0f, "%+.0f", kFitTip);
            ch |= nudge1("Yaw##Controller fit yaw", &fa[1], 2.0f, -45.0f, 45.0f, "%+.0f", kFitTip);
            ch |= nudge1("Roll##Controller fit roll", &fa[2], 2.0f, -45.0f, 45.0f, "%+.0f", kFitTip);
            if (ch) controllers::set_fit(1, fo, fa, true);
        } else {
            heading("Controller fit");
            note("No controller seen yet.");
        }
    }

    heading("Where your hands grab (a press 5 mm)");
    {  // [Hands] InteractOffset (round 13)
        float go[3];
        holster::interact_offset(go);
        if (nudge3("Interaction spot", go, 0.005f, 1.0f,
                   "The point of your hand that grabs holsters, rounds, rings and a gun's parts (the dot Show the hand dots draws), "
                   "moved from the wrist."))
            holster::set_interact_offset(go, true);
    }
}

void page_holsters() {
    heading("The holsters");
    bool on = holster::holsters_enabled();
    if (check("Body holsters", &on,
              "Reach to a holster and grip to draw its weapon; grip and let go there with the weapon to put it away. A buzz tells "
              "you a hand is at one."))
        holster::set_holsters_enabled(on);
    sub_begin(on);
    {  // run 8 item 5: [Holsters] Mode
        const bool wm = wheel::wheel_mode();
        const char* const kModeTip = "Draw by reaching to the holsters, or hold a grip to open the game's weapon wheel, move toward "
                                     "a weapon and let go (the holsters then draw nothing; the chest still gives rounds).";
        ImGui::TextUnformatted("Weapons by:");
        ImGui::SameLine();
        if (radio("The holsters", !wm, kModeTip)) wheel::set_wheel_mode(false, true);
        ImGui::SameLine();
        if (radio("The weapon wheel", wm, kModeTip)) wheel::set_wheel_mode(true, true);
        if (wm) {  // [Holsters] WheelStickDpad (2026-10-26)
            ImGui::Indent();
            bool sd = wheel::stick_dpad();
            if (check("The wheel's hand: its stick is the D-pad", &sd,
                      "While a grip holds the wheel open, push that hand's stick left or right to pick another gun of the highlighted "
                      "kind (the game's D-pad); the stick neither walks nor turns meanwhile."))
                wheel::set_stick_dpad(sd, true);
            // [Holsters] WheelPause, WheelPauseScale (2026-10-27, the user: "Weapon wheel pausing game until you pick a
            // weapon"): off by default
            bool wp = wheel::wheel_pause();
            if (check("The world stands still while the wheel is open", &wp,
                      "While a grip holds the weapon wheel open, the world nearly stops (the game's own time warp, as Dead Eye's) "
                      "until you let go; it runs on at once after. Not while Dead Eye is on or a hand holds an object."))
                wheel::set_wheel_pause(wp, true);
            sub_begin(wp);
            float ps = wheel::wheel_pause_scale();
            if (nudge1("The world's speed meanwhile", &ps, 0.01f, 0.01f, 0.08f, "%.2fx",
                       "How fast the world runs while the wheel is open: 0.02 is nearly still, 0.08 slow motion (a press 0.01; never 0)."))
                wheel::set_wheel_pause_scale(ps, true);
            sub_end();
            ImGui::Unindent();
        }
    }
    bool fists = holster::unarmed_after_holster();
    if (check("Putting a gun away selects the fists", &fists,
              "As the game's weapon wheel does: the left trigger then raises your fists instead of drawing the last gun."))
        holster::set_unarmed_after_holster(fists);
    bool hk = holster::hold_to_keep();  // [Holsters] HoldToKeep (2026-10-30)
    ImGui::BeginDisabled(wheel::wheel_mode());  // (the weapon wheel's mode keeps its own grips)
    if (check("Hold the grip to keep the gun", &hk,
              "Grip at a holster to draw and keep holding: the gun stays in your hand only while you hold the grip. Let go anywhere "
              "and it goes back in its holster; with your other hand on a long gun's foregrip, letting go leaves the gun carried "
              "by that hand (Carry a long gun by its foregrip). A second gun (dual wield) too. With a gun in hand your gun hand's "
              "grip is not the game's button (RB: the cover), except in Dead Eye (its paint). Not with the weapon wheel."))
        holster::set_hold_to_keep(hk, true);
    ImGui::EndDisabled();
    bool dg = controls::draw_to_grabbing_hand();
    if (check("Draw into the hand that grabs the holster", &dg,
              "The gun goes into whichever hand grabbed the holster, and that hand's trigger fires."))
        controls::set_draw_to_grabbing_hand(dg);
    {  // run 7 item 1d
        const char* const kAnchorTip = "Whether the holsters stay with John's body or move with you when you step around your room. "
                                       "Riding and cover always use the body.";
        int an = holster::anchor();
        ImGui::TextUnformatted("They follow:");
        ImGui::SameLine();
        if (radio("John's body", an == 0, kAnchorTip)) holster::set_anchor(0);
        ImGui::SameLine();
        if (radio("Your headset", an == 1, kAnchorTip)) holster::set_anchor(1);
        sub_begin(an == 1);  // 2026-10-09: [Holsters] TurnWithHead, TurnDeadZone
        bool tw = holster::turn_with_head();
        if (check("Turn with the headset", &tw, "The holsters turn around you as you turn your head. Off: they face the way John faces."))
            holster::set_turn_with_head(tw);
        sub_begin(tw);
        bool th = holster::turn_by_hands();
        if (check("Turn by your head and hands", &th,
                  "While both hands aim in front of you, the holsters turn halfway toward your hands, so looking aside swings them less."))
            holster::set_turn_by_hands(th);
        float dz = holster::turn_dead_zone();
        if (nudge1("Turn dead zone", &dz, 5.0f, 0.0f, 90.0f, "%.0f deg",
                   "How far you can look aside before the holsters follow: 0 follows every look; 30 to 45 lets you glance aside."))
            holster::set_turn_dead_zone(dz);
        sub_end();
        bool ls = holster::lean_steady();
        if (check("Steady when you lean", &ls, "Leaning or bending over leaves the holsters at your hips; crouching and stepping still move them."))
            holster::set_lean_steady(ls);
        sub_end();
    }
    sub_end();

    // 2026-10-13 (the menu's clean up): the two placing modes one choice (each holds the draws while on)
    heading("Placing by hand");
    {
        const int pm = holster::placing() ? 1 : holster::placing_pieces() ? 2 : 0;
        const char* const kPlaceTip = "Rings: close the menu, grip a holster's ring and move it. Belt pieces and guns: grip a piece of "
                                      "John's belt or a holstered gun, then move and turn it. Let go to leave it (saved). Nothing is "
                                      "drawn meanwhile: set Off when done.";
        if (radio("Off##Placing off", pm == 0, kPlaceTip)) holster::set_placing(false), holster::set_placing_pieces(false);
        ImGui::SameLine();
        if (radio("Rings##Placing rings", pm == 1, kPlaceTip)) holster::set_placing_pieces(false), holster::set_placing(true);
        ImGui::SameLine();
        if (radio("Belt pieces and guns##Placing pieces", pm == 2, kPlaceTip)) holster::set_placing(false), holster::set_placing_pieces(true);
    }
    bool rf = holster::rings_follow();  // 2026-10-13 (the user: "Two options should be selectable")
    if (check("Rings stay on their holsters", &rf,
              "A holster's ring and its piece of John's belt keep their places to each other: moving one moves the other the same "
              "way. Off: each moves on its own. The ammo ring is always on its own."))
        holster::set_rings_follow(rf, true);

    heading("Shown");
    {  // [Holsters] InteractionHighlights (2026-10-29, GOAL-weapons-handling.md item 5)
        const int hm = holster::interaction_highlights();
        const char* const kHlTip = "Rings: the rings below, as their switches say. Highlights: instead of the rings, a soft glow over "
                                   "the place itself (the gun's part, or the holster), only while using it would do something now (a "
                                   "holster with a gun to draw or room to put one away, the loading gate with room in the gun, the "
                                   "hammer when it can be cocked): what the gun needs at any distance, brighter within reach, the "
                                   "foregrip and a holster while a hand is near; never over your sights. Both: the rings and the highlights. Glowing "
                                   "parts: as highlights, but the gun's parts (the hammer, the lever, the bolt, the slide, the loading "
                                   "gate, the foregrip) glow on the gun itself, as if their own texture glowed, hidden where your hand "
                                   "covers them; the holsters have no glow.";
        ImGui::TextUnformatted("Interaction points:");
        ImGui::SameLine();
        if (radio("Rings##interaction points", hm == 0, kHlTip)) holster::set_interaction_highlights(0);
        ImGui::SameLine();
        if (radio("Highlights##interaction points", hm == 1, kHlTip)) holster::set_interaction_highlights(1);
        ImGui::SameLine();
        if (radio("Both##interaction points", hm == 2, kHlTip)) holster::set_interaction_highlights(2);
        ImGui::SameLine();
        // (run 29 item 2, the user 2026-10-29: "I want the weapon parts to look like they have a glowing texture on them")
        if (radio("Glowing parts##interaction points", hm == 3, kHlTip)) holster::set_interaction_highlights(3);
        sub_begin(hm == 3);  // [Holsters] PartGlowColor, PartGlowStrength
        float col[3];
        holster::part_glow_color(col);
        static const char* const kRgb[3] = {"red", "green", "blue"};
        if (nudge3("The glow's colour##part glow colour", col, 0.05f, 1.0f,
                   "The glowing parts' colour (red, green, blue, each 0 to 1; a press 0.05). Round 45's warm gold is 1.00 0.84 0.52.", kRgb, "%s %.2f")) {
            for (float& c : col) c = std::fmax(0.0f, c);
            holster::set_part_glow_color(col);
        }
        float gs = holster::part_glow_strength();
        if (nudge1("The glow's strength##part glow strength", &gs, 0.1f, 0.0f, 8.0f, "%.1f",
                   "How bright the glowing parts are against their own texture (0 to 8; 1 the default; a press 0.1)."))
            holster::set_part_glow_strength(gs);
        sub_end();
    }
    bool rings = holster::show_zones();
    if (check("Show the holsters", &rings, "Each holster as a ring, to place and size them: white, green with a hand in it, amber gripped."))
        holster::set_show_zones(rings);
    sub_begin(rings);  // 2026-10-09: [Holsters] ZonesNear
    bool znear = holster::zones_near();
    if (check("Only near a hand", &znear, "A ring shows, faintly, only while a hand comes near it."))
        holster::set_zones_near(znear);
    sub_end();
    bool hdots = holster::show_hand_dots();  // run 8 item 5b
    if (check("Show the hand dots", &hdots, "A dot at each hand's grab point, green while it is in a holster or on the foregrip."))
        holster::set_show_hand_dots(hdots, true);
    bool wpts = holster::show_weapon_points();
    if (check("Show the weapon points", &wpts,
              "Where a gun takes your other hand: the foregrip ring, where a round goes in, and hints for working the action."))
        holster::set_show_weapon_points(wpts, true);
    bool sg = holster::show_guns();
    if (check("Show the guns at their holsters", &sg,
              "Each holstered gun drawn at its holster, hidden while it is in a hand. Set each one's angle and place below."))
        holster::set_show_guns(sg);
    sub_begin(sg);
    bool sb = holster::show_back_guns();  // round 13 item 14
    if (check("The long guns on the back too", &sb, "Off: only the hips' guns are shown.")) holster::set_show_back_guns(sb);
    sub_end();

    heading("Weapons");
    bool wc = holster::weapon_choice();
    if (check("Choose the weapon in each holster", &wc,
              "Each holster draws the weapon you pick for it below (the game's weapon wheel follows). Automatic: the game's weapon "
              "for that slot."))
        holster::set_weapon_choice(wc);
    sub_begin(wc);
    bool any = holster::any_weapon();
    // 2026-10-13 (the user: "change the option to have any weapon in any holster to only apply to the gun holsters")
    if (check("Any weapon in the gun holsters", &any,
              "The hip and back holsters take any weapon you own, such as a pistol on the back. Put one away at a holster and it is "
              "drawn from there again. The knife, satchel and lasso holsters keep their own."))
        holster::set_any_weapon(any);
    sub_end();
    holster::Arsenal ars{};
    const bool have = holster::arsenal(&ars);

    // run 12 items 2-4: John's own belt's pieces, each moved and turned
    heading("John's belt");
    const bool own = body::own_pieces();
    {
        bool cb = own;
        // (2026-10-13, the user: "Is there a box to tick that turns all of this off and just uses the standard belt?")
        if (check("Customize John's belt", &cb,
                  "Move and turn John's holster, knife sheath, satchel, lasso and rifle sling (each holster below), and add a mirrored "
                  "holster at the left hip. Off: the game's own belt; your places are kept."))
            holster::set_own_pieces_linked(cb, true);
        sub_begin(cb);
        bool lh = body::left_holster();
        if (check("A mirrored holster at the left hip", &lh, "John's holster drawn again at the left hip for the secondary sidearm."))
            body::set_left_holster(lh, true);
        sub_end();
    }
    // 2026-10-13 (the user: "Can you add an option to reset the belt to game default positions?"): always clickable (also
    // with Customize John's belt off), each on a second click
    if (confirm_button("Reset the belt to the game's positions", "Reset the belt",
                       "Every piece of John's belt and the left holster back where the game hangs them (with Rings stay on their "
                       "holsters, their rings move back with them). Click twice."))
    {
        for (int i = 0; i < body::kPieces; ++i) holster::set_piece_linked(i, body::Piece{}, true);
        holster::set_piece_linked(body::kPieces, body::Piece{}, true);
    }
    ImGui::SameLine();
    if (confirm_button("Reset the rings to their defaults", "Reset the rings",
                       "Every holster's ring back to its default place and size (the belt pieces stay). Click twice."))
        holster::reset_rings();

    // each holster, in the user's order: the hips, the backs, the knife, the satchel, the lasso, the ammo
    heading("Each holster (a press 5 mm or 2.5 deg)");
    note("Ring: its place (right / up / forward) and size. Place and Angle: its piece of John's belt (with Customize John's belt). "
         "Gun's angle and place: its holstered gun (with Show the guns).");
    static const int kOrder[] = {0, 5, 1, 6, 3, 2, 7, 4};
    static const char* const kAxis[3] = {"tilt", "turn", "lean"};
    for (int z : kOrder) {
        if (z >= holster::zone_count()) continue;
        float off[3], radius = 0;
        holster::zone(z, off, &radius);
        const std::string zl = holster::zone_label(z);
        ImGui::PushID(z);
        ImGui::Separator();
        bool en = true;
        if (holster::zone_takes_weapons(z)) {
            en = holster::zone_enabled(z);
            if (check((zl + "##" + zl + " on").c_str(), &en, "This holster on or off.")) holster::set_zone_enabled(z, en, true);
        } else {
            ImGui::TextUnformatted(zl.c_str());
        }
        sub_begin(en);
        if (wc && holster::zone_takes_weapons(z)) {  // its weapon (was the Weapons tab)
            const int chosen = holster::zone_weapon(z);
            const bool any_z = any && holster::zone_any(z);
            // (a choice of another kind, an older AnyWeapon's, draws automatically: shown so)
            const bool kind_ok = chosen >= 0 && chosen < holster::kRounds && (any_z || !have || holster::zone_fits_weapon(z, chosen, ars.equip_slot[chosen]));
            std::string preview = !kind_ok ? std::string("Automatic (the game's weapon for it)") : holster::weapon_label(chosen);
            if (kind_ok && have && !(ars.owned >> chosen & 1)) preview += " (not owned: automatic)";
            ImGui::TextUnformatted("Weapon");
            ImGui::SameLine(kLabelW);
            ImGui::SetNextItemWidth(460);
            const bool open = ImGui::BeginCombo(("##" + zl).c_str(), preview.c_str());
            if (!open) track(zl.c_str());
            help(have ? "The weapon this holster draws." : "The weapon this holster draws (your weapons are read in gameplay).");
            if (open) {
                if (ImGui::Selectable("Automatic (the game's weapon for it)", chosen < 0)) holster::set_zone_weapon(z, -1, true);
                track((zl + ": Automatic").c_str());
                for (int w = 0; have && w < holster::kWeapons; ++w) {
                    if (!(ars.owned >> w & 1) || (!any_z && !holster::zone_fits_weapon(z, w, ars.equip_slot[w]))) continue;
                    ImGui::PushID(w);
                    if (ImGui::Selectable(holster::weapon_label(w), chosen == w)) holster::set_zone_weapon(z, w, true);
                    ImGui::PopID();
                    track((zl + ": " + holster::weapon_label(w)).c_str());
                }
                ImGui::EndCombo();
            }
        }
        // round 13 item 9: arrows, a press one increment (saved at once)
        bool zc = nudge3(("Ring##" + zl + " offset").c_str(), off, 0.005f, 0.4f, "Its ring moved right, up or forward (m).");
        zc |= nudge1(("Size##" + zl + " size").c_str(), &radius, 0.005f, 0.05f, 0.4f, "%.3f", "Its ring's size (radius, m).");
        if (zc) holster::set_zone_linked(z, off, radius, true);
        const int pc = holster::zone_piece(z);
        if (own && pc == body::kPieces) {  // the mirrored left holster
            body::Piece p = body::left_holster_piece();
            bool ch = nudge3("Place##LeftHolster place", p.d, 0.005f, 0.6f, "The left holster moved right, up or forward (m).");
            ch |= nudge3("Angle##LeftHolster angle", p.turn, 2.5f, 180.0f, "The left holster turned: tilt, turn, lean (degrees).", kAxis, "%s %+.1f");
            if (ch) holster::set_piece_linked(body::kPieces, p, true);
        } else if (own && pc >= 0 && pc < body::kPieces && z != 6) {  // (the left back's sling: the right back's rows)
            body::Piece p = body::piece(pc);
            const std::string k = body::piece_key(pc);
            bool ch = nudge3(("Place##" + k + " place").c_str(), p.d, 0.005f, 0.6f,
                             z == 1 ? "John's rifle sling (both back holsters hang on it) moved right, up or forward (m)."
                                    : "Its piece of John's belt moved right, up or forward from where the game hangs it (m).");
            ch |= nudge3(("Angle##" + k + " angle").c_str(), p.turn, 2.5f, 180.0f, "Its piece turned: tilt, turn, lean (degrees).", kAxis,
                         "%s %+.1f");
            if (ch) holster::set_piece_linked(pc, p, true);
        } else if (own && z == 6) {
            note("On John's rifle sling: the right back's Place and Angle move it.");
        }
        if (sg && holster::zone_shows_gun(z)) {
            // 2026-10-12: the shown gun's angle and place at its holster ([Holsters] <Key>GunTurn, <Key>GunOffset)
            float tn[3], go[3];
            holster::zone_gun(z, tn, go);
            ImGui::PushID(100 + z);
            bool ch = nudge3(("Gun's angle##" + zl + " gun angle").c_str(), tn, 2.5f, 180.0f,
                             "The holstered gun turned about its grip: tilt (barrel forward or back), turn, lean (degrees). The left "
                             "holsters mirror the right's until set.",
                             kAxis, "%s %+.1f");
            ch |= nudge3(("Gun's place##" + zl + " gun place").c_str(), go, 0.005f, 0.3f, "The holstered gun moved right, up or forward (m).");
            if (ch) holster::set_zone_gun(z, tn, go, true);
            ImGui::PopID();
        }
        sub_end();
        ImGui::PopID();
    }
}

void page_guns() {
    heading("The gun in hand");
    {  // 2026-10-07: the reticle where the shot lands
        bool rt = aim::reticle_on();
        if (check("Reticle where the shot lands", &rt,
                  "With a gun in hand, a reticle where the bullet will hit; red while you aim at a person or an animal."))
            aim::set_reticle_on(rt);
        sub_begin(rt);  // [Hands] ReticleStyle
        const bool dot = aim::reticle_dot();
        const char* const kStyleTip = "The reticle's look: a ring with a dot, or a smaller dot only.";
        if (radio("Ring and dot", !dot, kStyleTip)) aim::set_reticle_dot(false);
        ImGui::SameLine();
        if (radio("Dot only", dot, kStyleTip)) aim::set_reticle_dot(true);
        sub_end();
    }
    {  // (r55 item 11) [Scope] Enabled, Magnification, OtherEye
        bool sc = scope::enabled();
        if (check("Look through a rifle's scope", &sc,
                  "Bring a scoped rifle's eyepiece to an eye (the Rolling Block, the Carcano; the Buffalo by its sights): that eye sees "
                  "through the scope, magnified, in a round picture at the eyepiece, fading in and out as it comes and goes. Either "
                  "eye; one hand is enough."))
            scope::set_enabled(sc, true);
        if (sc && scope::picture_broken())
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "The scope's picture could not be made on this PC (see RDRVR.log): off until ticked again.");
        sub_begin(sc);
        const bool gz = scope::game_zoom();
        const char* const kMagTip = "Realistic: a period scope, about 4x. The game's zoom: the game's own sniper view, about 7.75x.";
        ImGui::TextUnformatted("Magnification:");
        ImGui::SameLine();
        if (radio("Realistic (4x)", !gz, kMagTip)) scope::set_game_zoom(false, true);
        ImGui::SameLine();
        if (radio("The game's zoom", gz, kMagTip)) scope::set_game_zoom(true, true);
        const bool dk = scope::other_dark();
        const char* const kEyeTip = "Your other eye while you look through the scope: open, as with a real scope, or dark.";
        ImGui::TextUnformatted("The other eye:");
        ImGui::SameLine();
        if (radio("Open, as with a real scope", !dk, kEyeTip)) scope::set_other_dark(false, true);
        ImGui::SameLine();
        if (radio("Dark while scoped", dk, kEyeTip)) scope::set_other_dark(true, true);
        sub_end();
    }
    bool kd = holster::keeps_drawn();
    if (check("Keep the gun drawn", &kd, "A drawn gun stays in your hand until you put it away (the game holsters it 3 s after your last shot).")) {
        holster::set_keeps_drawn(kd);
        config::set("Hands", "KeepGunDrawn", kd ? "1" : "0");
    }
    bool ex = !aim::block_executions();  // round 13
    if (check("Executions up close", &ex,
              "Pulling the trigger close to someone can start the game's third-person execution, pistol whip or butt strike. Off "
              "(the default): it just fires."))
        aim::set_block_executions(!ex);
    bool pab = aim::shoot_past_arm_block();  // run 7 item 1e: [Hands] ShootPastArmBlock
    if (check("Shoot past the arm block", &pab,
              "Lets you fire when the game thinks John's shoulder-to-gun line hits a wall (your gun can be over a wall his shoulder "
              "is behind). A friendly in the way still blocks the shot."))
        aim::set_shoot_past_arm_block(pab, true);
    if (aim::spawn_hooked()) {
        bool pa = aim::perfect_accuracy();
        if (check("Perfect accuracy", &pa,
                  "Every shot goes exactly where the barrel points: no random spread. Off: the game's own spread."))
            aim::set_perfect_accuracy(pa);
        bool sp = aim::shotgun_pattern();
        if (check("Shotgun pellets spread", &sp, "The pellets spread in the game's cone. Off: every pellet goes where the barrel points."))
            aim::set_shotgun_pattern(sp);
    } else {
        note("Perfect accuracy: not available on this game build.");
    }
    {  // [Hands] GunKick (2026-10-27; the user: "Look into the recoil optimumbox added"; after optimumbox's recoil, 0.8.11.10)
        // (the review: the kick's shots are counted in the spawn hook; without it the row says so, as Perfect accuracy's)
        if (aim::spawn_hooked()) {
            float rk = body::gun_kick() * 100.0f;
            if (nudge1("Gun kick", &rk, 25.0f, 0.0f, 200.0f, "%.0f%%",
                       "Each shot kicks the gun in your hand muzzle up and back for a moment, the most on revolvers, rifles and shotguns; "
                       "a long gun held with both hands slides back instead. The second gun and the copy kick on their own shots. "
                       "Looks only: your shots still go where the barrel points. 0% = off (a press 25%)."))
                body::set_gun_kick(rk / 100.0f, true);
        } else {
            note("Gun kick: not available on this game build.");
        }
    }
    {  // [Hands] HandSmoothing (2026-11-02, GOAL-older-items item 22; after optimumbox's)
        bool hs = controllers::hand_smoothing();
        if (check("Hand smoothing", &hs,
                  "Steadies your hands and the aim while they are still (a gentle filter that lets go as they move); off: the "
                  "controllers exactly as tracked."))
            controllers::set_hand_smoothing(hs, true);
    }
    bool gcol = body::gun_collision();  // [Hands] GunCollision (2026-10-29, GOAL-weapons-handling item 4)
    if (check("Guns touch", &gcol,
              "Your two guns (dual wield, the copy, a carried long gun) and your hands and arms don't pass through each other: a gun "
              "that touches the other gun, your other hand or an arm is drawn held against it, with a buzz. With manual cocking, the "
              "other gun swept back across a single action's hammer cocks it. Looks only: your shots still go where the barrel "
              "points. A hand holding a round, a magazine, the foregrip or a part is never pushed off it."))
        body::set_gun_collision(gcol, true);
    bool ohi = offhand::enabled();  // [Hands] OffHandItems (2026-10-29: the knife, the throwables and the torch with a gun)
    if (check("Items in the free hand", &ohi,
              "With a gun in one hand, grip at the knife's sheath or the satchel with the other hand to draw the knife, the torch "
              "or a throwable into it: the gun stays in hand and keeps aiming, shooting and reloading. A fast stab or slash into "
              "someone hits as the game's knife; a fast swing of the torch knocks them and sets them alight; a slow move does "
              "neither. Let go of a throwable during a throw and it flies with your hand's speed; let go slowly, or of the knife "
              "or the torch, and it goes back where it came from."))
        offhand::set_enabled(ohi, true);
    bool ohl = offhand::torch_lit_enabled();  // [Hands] OffHandTorchLit (2026-10-29, Roo Code's; off)
    if (check("The free hand's torch lit (experimental)##The free hand's torch lit", &ohl,
              "Undead Nightmare: the torch drawn into your free hand burns with the game's own flame and light, at the torch in "
              "your hand. It needs the torch to be one of your weapons the game has ready (choosing the torch on the weapon wheel "
              "may do it: not yet tried); otherwise it stays unlit. Needs \"Items in the free hand\" (and the launcher's \"Flames "
              "follow the item in your hand\", on unless you turned it off)."))
        offhand::set_torch_lit(ohl, true);
    bool mfp = pose::mounted_first_person();  // 2026-10-13: [Hands] MountedGunFirstPerson
    if (check("Mounted guns in first person (experimental)##Mounted guns in first person", &mfp,
              "The Gatling, the Browning and the cannon: first person kept, John's hands on the gun, and the gun turned toward where your "
              "gun hand points (it learns the gun's barrel from your first shot). Off: the game's own camera and aim while you use one."))
        pose::set_mounted_first_person(mfp, true);

    heading("Dead Eye");
    {  // [DeadEye] Level, Sepia (2026-10-26)
        const int lv = deadeye::level(), un = deadeye::unlocked();
        const char* const kLvTip = "Which Dead Eye you use, up to the level the story has unlocked: slow motion only; the marks painted as "
                                   "you sweep over targets; or the marks painted with the button. A change applies at the next Dead Eye.";
        char lb[96];
        std::snprintf(lb, sizeof(lb), "Dead Eye level (unlocked: %s):", un ? (un == 1 ? "1" : un == 2 ? "2" : "3") : "not read yet");
        note(lb);
        if (radio("The game's##deadeye 0", lv == 0, kLvTip)) deadeye::set_level(0);
        ImGui::SameLine();
        if (radio("1: slow motion##deadeye 1", lv == 1, kLvTip)) deadeye::set_level(1);
        if (radio("2: marks as you sweep##deadeye 2", lv == 2, kLvTip)) deadeye::set_level(2);
        ImGui::SameLine();
        if (radio("3: marks by the button##deadeye 3", lv == 3, kLvTip)) deadeye::set_level(3);
        bool sp = deadeye::sepia();
        if (check("Dead Eye's sepia colour", &sp, "Dead Eye's sepia screen colour. Off: the colours stay as they are; the slow motion and the rest stay."))
            deadeye::set_sepia(sp);
    }
    {  // [Hands] DeadEyeMarks, DeadEyeMarkDegrees, DeadEyeBarrelPaint (run 28 item 1c; after optimumbox's fork, v0.8.11.9,
       // commit ac942427, MIT; his key names)
        // each feature's hooks are installed only when it is on at the start: turned on while not installed, the row
        // says "(restart)" and the change applies at the next start
        if (deadeye::marks_supported()) {
            const bool live = deadeye::marks_available();
            bool mk = deadeye::marks_on();
            if (check(live ? "Dead Eye's X on each mark##Dead Eye's X on each mark" : "Dead Eye's X on each mark (restart)##Dead Eye's X on each mark", &mk,
                      "The game's X on each target you mark in Dead Eye, drawn on the target in the headset (it showed nowhere "
                      "before). While marks show, a small part of the floating HUD (where the X wait to be drawn) is left out. "
                      "Turned on while the game runs, it applies after a restart."))
                deadeye::set_marks_on(mk, true);
            if (mk && !live) note("Restart the game to apply.");
            sub_begin(mk);
            float dg = deadeye::mark_degrees();
            if (nudge1("Its size##Dead Eye X size", &dg, 0.5f, 0.5f, 15.0f, "%.1f degrees", "How big each X looks in the headset."))
                deadeye::set_mark_degrees(dg, true);
            sub_end();
        } else {
            note("Dead Eye's X on each mark: not available on this game build.");
        }
        if (deadeye::barrel_paint_supported()) {
            const bool live = deadeye::barrel_paint_available();
            bool bp = deadeye::barrel_paint();
            if (check(live ? "Dead Eye marks where the barrel points##Dead Eye marks where the barrel points"
                           : "Dead Eye marks where the barrel points (restart)##Dead Eye marks where the barrel points",
                      &bp,
                      "A mark painted with the button lands on whoever your barrel points at. Off: the game picks the target "
                      "itself from its flat camera's view, which in the headset often is not the one you aim at. Turned on "
                      "after the game started with it off, it applies after a restart."))
                deadeye::set_barrel_paint(bp, true);
            if (bp && !live) note("Restart the game to apply.");
        } else {
            note("Dead Eye marks where the barrel points: not available on this game build.");
        }
    }
    heading("Dual wield");
    bool dw = dual::enabled();
    if (check("Dual wield", &dw,
              "With a gun in one hand, grip another gun holster with the free hand to take a second gun, with its own trigger and "
              "ammo."))
        dual::set_enabled(dw);
    sub_begin(dw);
    bool bt = dual::beside_torch_enabled();  // [Hands] DualWieldBesideTorch (2026-10-29)
    if (check("A gun beside the torch", &bt,
              "Undead Nightmare: with the torch in one hand (lit, as the game's weapon), grip a gun holster with the free hand to "
              "take a second gun, as beside a gun: its own trigger, reticle and ammo. Press it to your chest for one round at a time "
              "(with the chest squeeze off it can't be reloaded beside the torch; the torch hand takes no rounds for it). Put the "
              "torch away and the second gun becomes the gun in hand."))
        dual::set_beside_torch(bt, true);
    bool ks = holster::knife_swap();  // [Hands] KnifeSwap (2026-11-01, GOAL-offhand-changes C2)
    if (check("The knife beside a gun is the game's knife", &ks,
              "Your other hand draws the knife while a gun is in hand: the knife becomes the game's own weapon in that hand (its "
              "stab hits as the game's knife) and the gun the second gun in your gun hand. Off: the knife as a copy beside the gun."))
        holster::set_knife_swap(ks);
    sub_end();
    sub_begin(dw || dual::copy_enabled());  // (the copy too: it works with DualWield off)
    bool xr = dual::cross_reload();  // [Hands] DualWieldCrossReload (2026-10-27)
    if (check("Each hand reloads the other gun", &xr,
              "With a second gun or the copy out, grip at the chest with either hand to take rounds for the gun in your OTHER hand, "
              "then bring them to that gun's loading ring. Rounds per grab and one-at-a-time apply, as do its action's gates (a "
              "revolver in hand must be open; a second gun that opens to load takes none with the manual actions on, unless opened "
              "by its stick (below); the copy has no action of its own and always takes them, or with that option only while "
              "open). The hand holding rounds does not fire its own gun until they are in and its trigger is let go. The chest "
              "squeeze is off for the two guns while this is on."))
        dual::set_cross_reload(xr, true);
    bool dop = actions::dual_open();  // [Hands] DualWieldOpen (2026-10-28, round 37 check 4)
    if (check("Open the second gun by its stick", &dop,
              "With each gun's action by hand on and a revolver as the second gun or the copy: hold that hand's stick down and "
              "click it to open the gun (its rounds then go in, with the option above), and flick that hand (a roll or a turn) "
              "to shut it, as the gun in your gun hand. That click never crouches or brakes. While such a gun is out, a quick "
              "flick of that stick back does not walk you back (on foot; riding, the stick slows the horse at once as before) "
              "and a quick click does nothing: hold the stick back, or the click in, a moment longer (the two waits below) to "
              "walk back, crouch, or brake on a horse. While it is open its trigger does nothing; Dead Eye shuts it. With revolvers "
              "cocked by a flick, a single action there needs that stick's quick flick down after each shot, as the gun in hand does."))
        actions::set_dual_open(dop, true);
    sub_begin(dop);  // (2026-10-29) the walk back's and the click's waits while it is armed
    {
        float wm = actions::dual_open_wait_ms(), cm = actions::dual_click_hold_ms();
        bool ch = nudge1("The walk back waits##DualWieldOpenWaitMs", &wm, 25.0f, 0.0f, 600.0f, "%.0f ms",
                         "While the second gun can be opened by its stick, on foot: how long that stick pulled back waits before you walk "
                         "back, so a quick flick down to open the gun does not move you. Riding, there is no wait.");
        ch |= nudge1("The click held in for##DualWieldClickHoldMs", &cm, 25.0f, 0.0f, 600.0f, "%.0f ms",
                     "While the second gun can be opened by its stick: how long that stick's click must be held in before it crouches "
                     "you (or brakes on a horse). A quicker click does nothing.");
        if (ch) actions::set_dual_open_waits(wm, cm, true);
    }
    sub_end();
    sub_end();
    bool dc = dual::copy_enabled();
    if (check("The same sidearm in both hands", &dc,
              "With a sidearm in hand, grip a hip holster with the free hand to take a copy of it; let go at a hip to put it back."))
        dual::set_copy_enabled(dc);
    sub_begin(dc);
    bool cap = dual::copy_as_prop();  // run 7 item 1: [Hands] CopyAsProp
    if (check("The copy drawn as a prop", &cap, "Draws the copy as a model of the gun, so you see it in the free hand."))
        dual::set_copy_as_prop(cap, true);
    bool om = dual::own_model();
    if (check("Your other sidearm's model", &om,
              "If you own a different sidearm, the copy is that gun (the hip holster's), and its shots leave its muzzle."))
        dual::set_own_model(om);
    sub_begin(om);
    bool sh = dual::same_at_its_holster();  // round 13 item 10
    if (check("A second of the gun at its own holster", &sh,
              "Gripping the holster the gun was drawn from gives a second of that same gun; the other hip still gives your other "
              "sidearm."))
        dual::set_same_at_its_holster(sh);
    sub_end();
    sub_end();

    heading("Two-handed long guns");
    bool v = reload::two_handed();
    if (check("Two-handed long guns", &v, "Grip a long gun's foregrip with your other hand, ahead of the gun hand, to aim it with both hands."))
        reload::set_two_handed(v);
    sub_begin(v);
    bool snap = holster::foregrip_snap();
    if (check("The front hand snaps onto the gun", &snap,
              "While you hold the foregrip, John's front hand sits on the gun as the game holds it, instead of following your controller."))
        holster::set_foregrip_snap(snap, true);
    bool steer = body::two_handed_steer();  // [Reload] TwoHandedSteer (2026-10-15)
    if (check("The front hand turns the gun", &steer,
              "On: the gun points from your gun hand toward your front hand. Off: John's front hand is drawn holding the gun, but "
              "only your gun hand moves it."))
        body::set_two_handed_steer(steer);
    bool stk = holster::physical_stock();  // [Reload] PhysicalStock (run 14)
    if (check("Physical rifle stock", &stk,
              "For a stock that holds both controllers: your front hand's trigger at the foregrip takes hold of the gun as its "
              "grip does (the trigger still aims), and only your gun hand aims the gun."))
        holster::set_physical_stock(stk);
    sub_begin(stk);
    const double cal_in = holster::stock_calibrate_in();
    if (cal_in >= 0.0) {
        char cb[120];
        std::snprintf(cb, sizeof(cb), "Hold a long gun on the stock with both hands, aiming down the sights: %.0f s", cal_in / 1000.0 + 0.5);
        note(cb);
    } else {
        if (ImGui::Button("Calibrate the stock")) {  // (the menu closed: the game aims again during the countdown; the review)
            holster::stock_calibrate(5000.0);
            set_visible(false);
        }
        track("Calibrate the stock");
        help("Press (the menu closes), then within 5 seconds hold any long gun on your stock with both hands, aiming down the sights: "
             "every long gun is set straight along the stock with its top up.");
        ImGui::SameLine();
        if (ImGui::Button("Clear the stock calibration")) {
            const float z[3] = {0.0f, 0.0f, 0.0f};
            holster::set_stock_turn(z);
        }
        track("Clear the stock calibration");
        help("The long guns back to the game's own angle in your gun hand.");
    }
    sub_end();
    bool carry = dual::carry_enabled();  // [Hands] CarryByForegrip (2026-10-26)
    if (check("Carry a long gun by its foregrip", &carry,
              "Holding a long gun with both hands, hold your gun hand's grip half a second away from the holsters, then let go (a "
              "quick tap stays the game's cover): the front hand "
              "keeps the gun by its foregrip (carried, it does not fire) and the gun hand is free to draw another weapon. Grip near "
              "the gun's grip with the empty hand to take it back; let go with the front hand to put it back in its holster."))
        dual::set_carry_enabled(carry, true);
    sub_end();

    // round 13 item 8: the gun in hand's own foregrip, foregrip ring and loading ring (else the Reloading tab's)
    const int w = holster::gun_in_hand();
    if (w < 0) {
        heading("This gun");
        note("Hold a gun to give it its own foregrip, rings and front-hand hold here; a gun without its own uses every gun's "
             "(the Reloading tab).");
        return;
    }
    char hd[160];
    std::snprintf(hd, sizeof(hd), "This gun: %s (a press 5 mm)", holster::weapon_label(w));
    heading(hd);
    note("Its own settings; without its own it uses every gun's (the Reloading tab). Offsets right / up / forward along the gun.");
    // (2026-11-01, GOAL-offhand-changes C35, C38) every gun (the Blunderbuss, the Explosive Rifle too): the foregrip's two for
    // the long guns, the loading ring for all, the magazine's place for the magazine pistols
    // (C36; the user: the rings of the bolts and the other interaction spots adjustable) the action's spot, every gun
    static const char* const kName[5] = {"The foregrip", "The foregrip ring", "The loading ring", "The magazine",
                                         "The action's spot (bolt, breech, slide, hammer)"};
    static const char* const kTip[5] = {"Where John's front hand sits on this gun.",
                                        "Where your front hand takes hold of this gun, and the ring's size.",
                                        "Where a round goes into this gun, and the ring's size.",
                                        "Where this pistol's magazine goes in (its well under the grip).",
                                        "Where your other hand takes this gun's action: the bolt's handle, the breech, the slide, the "
                                        "fanned hammer or the muzzle's ramrod."};
    static const char* const kTrack[5] = {"Gun foregrip", "Gun foregrip ring", "Gun loading ring", "Gun magazine", "Gun action spot"};
    // (2026-11-01, GOAL-offhand-changes C8) each hand its own: which hand's set the arrows change
    static int s_ahand = 1;
    if (radio("Right hand##Gun adjust hand", s_ahand == 1, "Set this gun's places for when your right hand holds it.")) s_ahand = 1;
    ImGui::SameLine();
    if (radio("Left hand##Gun adjust hand", s_ahand == 0, "Set this gun's places for when your left hand holds it (until you change "
                                                          "them, it uses the right hand's).")) s_ahand = 0;
    for (int what = 0; what < holster::kAdjCount; ++what) {
        if (what < holster::kAdjLoadRing && !reload::is_long_gun(w)) continue;
        if (what == holster::kAdjMag && !actions::mag_gun(w)) continue;
        float off[3], r = 0.0f;
        bool own_adj = false;
        holster::gun_adjust(w, what, off, &r, &own_adj, s_ahand, true);
        if (s_ahand == 0 && !own_adj) holster::gun_adjust(w, what, off, &r, nullptr, 0);  // (the left's start: what it uses now)
        std::snprintf(hd, sizeof(hd), "%s (%s)", kName[what], own_adj                       ? "its own"
                                                              : what >= holster::kAdjMag   ? (s_ahand == 0 ? "now the right hand's, or as made" : "as made")
                                                              : s_ahand == 0               ? "now the right hand's or every gun's"
                                                                                           : "now every gun's");
        ImGui::TextUnformatted(hd);
        char lb[96];
        std::snprintf(lb, sizeof(lb), "Offset##%s", kTrack[what]);
        bool ch = nudge3(lb, off, 0.005f, 1.0f, kTip[what]);
        if (what == holster::kAdjForeRing || what == holster::kAdjLoadRing) {
            std::snprintf(lb, sizeof(lb), "Size##%s size", kTrack[what]);
            ch |= nudge1(lb, &r, 0.005f, 0.03f, 0.5f, "%.3f", kTip[what]);
        }
        if (ch) holster::set_gun_adjust(w, what, off, r, true, s_ahand, true);  // (C8b: the other hand stays)
        std::snprintf(lb, sizeof(lb), "Use every gun's##%s: every gun's", kTrack[what]);
        if (reset_button(lb, own_adj, "Clears this gun's own setting for this hand: it uses every gun's (the Reloading tab) again "
                                      "(the left hand: the right hand's)."))
            holster::clear_gun_adjust(w, what, s_ahand, true);  // (C8b: the other hand stays)
    }
    if (reload::is_long_gun(w)) {  // [Weapon.<Gun>] FrontHandPose: the long guns' front hand (round 67: the Blunderbuss and Explosive Rifle too)
        const int fp = body::front_pose(w), seen = body::front_pose_seen(w);
        ImGui::TextUnformatted("The front hand's hold:");
        const char* const kHoldTip = "Which of the game's holds John's front hand uses on this gun: the one for its stance "
                                     "(Automatic), or always the lowered or the aiming hold.";
        if (radio("Automatic##front pose", fp == 0, kHoldTip)) body::set_front_pose(w, 0);
        char lb[96];
        std::snprintf(lb, sizeof(lb), "Lowered%s##front pose", seen & 1 ? "" : " (hold it lowered once first)");
        ImGui::SameLine();
        if (radio(lb, fp == 1, kHoldTip)) body::set_front_pose(w, 1);
        std::snprintf(lb, sizeof(lb), "Aiming%s##front pose", seen & 2 ? "" : " (aim with it once first)");
        ImGui::SameLine();
        if (radio(lb, fp == 2, kHoldTip)) body::set_front_pose(w, 2);
    }
}

void page_reloading() {
    heading("Reload by hand");
    bool v = reload::hand_reload();
    if (check("Reload by hand", &v,
              "Grip at your chest with the hand that has no gun to take a round (a buzz), and bring it to the open gun to load it. "
              "Not in Dead Eye."))
        reload::set_hand_reload(v);
    sub_begin(v);
    bool ttr = holster::trigger_takes_round();  // [Reload] TriggerTakesRound (run 14; under Reload by hand: the review)
    if (check("Your empty hand's trigger takes a round", &ttr,
              "At the chest, your empty hand's trigger takes a round as its grip does; keep it pulled while you bring the round to the "
              "gun. The trigger still aims as usual."))
        holster::set_trigger_takes_round(ttr);
    {  // [Reload] RoundsPerGrab, RoundsGoIn (2026-10-26)
        float pg = static_cast<float>(holster::rounds_per_grab());
        if (nudge1("Rounds taken at once##Rounds per grab", &pg, 1.0f, 1.0f, 8.0f, "%.0f",
                   "How many rounds a grip at your chest takes, as many as the gun has room for; all of them drawn in your hand."))
            holster::set_rounds_per_grab(static_cast<int>(pg + 0.5f));
        const bool all = holster::rounds_all_at_once();
        const char* const kInTip = "One at a time: held at the gun's loading ring, the rounds go in one after another. All at once: they "
                                   "all go in as they reach it. Let go at the gun, the rest go in.";
        ImGui::TextUnformatted("The rounds held go in:");
        ImGui::SameLine();
        if (radio("One at a time##rounds in", !all, kInTip)) holster::set_rounds_all_at_once(false);
        ImGui::SameLine();
        if (radio("All at once##rounds in", all, kInTip)) holster::set_rounds_all_at_once(true);
    }
    {  // [Reload] RoundSource (2026-10-28)
        const int rs = holster::round_source();
        const char* const kSrcTip = "Your chest: grip at your chest to take rounds. Grip near the gun: also grip with your empty hand "
                                    "near the gun's loading ring (within a hand's width of it), and the rounds appear in your hand; push "
                                    "them into the ring (gripped inside the ring, they go straight in). Palm up: also turn your empty hand "
                                    "palm up, away from the gun, and grip. When dual wielding, either hand takes rounds for the other gun. "
                                    "Gripping a part of the gun (a bolt, the barrels, the foregrip) still works that part.";
        ImGui::TextUnformatted("Rounds come from:");
        ImGui::SameLine();
        if (radio("Your chest##round source", rs == 0, kSrcTip)) holster::set_round_source(0);
        ImGui::SameLine();
        if (radio("Grip near the gun##round source", rs == 1, kSrcTip)) holster::set_round_source(1);
        ImGui::SameLine();
        if (radio("Palm up##round source", rs == 2, kSrcTip)) holster::set_round_source(2);
    }
    sub_end();
    bool sa_ = round_draw::show_ammo();
    if (check("Show the rounds at the chest", &sa_, "While the gun can take a round, a row of its rounds at your chest (up to six)."))
        round_draw::set_show_ammo(sa_);
    {
        int sq = reload::chest_squeeze();
        const char* const kSqTip = "Gripping at your chest with the gun hand: nothing, one round in, or a full reload.";
        ImGui::TextUnformatted("The gun hand squeezed at the chest:");
        ImGui::Indent();
        const char* const kSq[3] = {"Does nothing##Squeeze does nothing", "Loads one round", "Reloads fully"};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            if (radio(kSq[i], sq == i, kSqTip)) reload::set_chest_squeeze(i);
        }
        ImGui::Unindent();
    }
    v = reload::automatic();
    if (check("The game's automatic reloads", &v,
              "The game's own reloads as well: after the last round, when drawing an empty gun, or on an empty trigger pull."))
        reload::set_automatic(v);
    v = reload::button();
    if (check("The game's reload button", &v, "The game's reload button works as well as the hand reload.")) reload::set_button(v);

    heading("The round in the hand");
    {  // [Reload] RoundInHand as its three modes (2026-10-27: the game's own shells were ini-only, a 2 kept by the tick)
        const int rm = round_draw::mode();
        const char* const kRhTip = "Draws the rounds you took at the chest between your fingers (and the row at the chest). The game's "
                                   "shotgun shells: a shotgun's shells are the game's own model, in the hand and at the chest, lit "
                                   "by the game.";
        ImGui::TextUnformatted("Rounds seen in the hand:");
        ImGui::Indent();
        const char* const kRh[3] = {"Off##Round in hand off", "The mod's rounds", "The game's shotgun shells too"};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            if (radio(kRh[i], rm == i, kRhTip)) round_draw::set_mode(i);
        }
        ImGui::Unindent();
    }
    bool rmo = round_draw::models();
    if (check("Each gun's own round (the new models)", &rmo,
              "Each gun's round at its real size, textured and lit as the game's own props. Off: the old simple rounds."))
        round_draw::set_models(rmo, true);
    {  // run 6 item 5: where the round sits in the hand (2026-10-31, R13: each hand its own)
        static int s_rhand = 0;
        ImGui::TextUnformatted("The round in your:");
        ImGui::SameLine();
        if (radio("Left hand##Round hand", s_rhand == 0, "Set where the round sits in your left hand.")) s_rhand = 0;
        ImGui::SameLine();
        if (radio("Right hand##Round hand", s_rhand == 1, "Set where the round sits in your right hand (until you change it, it uses the "
                                                         "left hand's place).")) s_rhand = 1;
        round_draw::Place pl = round_draw::place(s_rhand);
        // (2026-11-01, GOAL-offhand-changes C9; round 62 check 11, the user: the round cannot be moved far enough to set the angle
        // for the left gun: the limits opened up) 20 cm each way (was 8)
        bool ch = nudge3("Offset##Round offset", pl.offset, 0.002f, 0.20f, "The round moved from between your thumb and index (a press 2 mm).");
        bool done = ch;
        const char* const kAxis[3] = {"Along the fingers", "Out of the hand", "Custom"};
        const char* const kAxisTip = "Which way the round points: along the fingers, out of the hand, or your own by the two angles.";
        ImGui::TextUnformatted("Direction:");
        for (int i = 0; i < 3; ++i) {
            ImGui::SameLine();
            if (radio(kAxis[i], pl.axis_mode == i, kAxisTip)) {
                pl.axis_mode = i;
                ch = done = true;
            }
        }
        {  // round 13 item 12: its direction as two angles from along the fingers (Custom)
            // (2026-11-01, GOAL-offhand-changes C9b; round 65 check 12, the user: the turn and tilt limits unlocked too) both
            // go all the way round: each wraps past 180 to -175 (a tilt past 90 is over the top, down the far side); the angles
            // shown are kept per hand while the direction is the one they made (the axis alone reads back within +-90)
            constexpr float kDeg = 57.29578f;
            static float s_turn[2] = {0, 0}, s_tilt[2] = {0, 0}, s_made[2][3] = {{0, 0, 0}, {0, 0, 0}};
            static bool s_kept[2] = {false, false};
            float turn = 0.0f, tilt = 0.0f;
            if (pl.axis_mode != 1) {
                const float* k = s_made[s_rhand];
                if (s_kept[s_rhand] && std::fabs(k[0] - pl.axis[0]) + std::fabs(k[1] - pl.axis[1]) + std::fabs(k[2] - pl.axis[2]) < 1e-4f) {
                    turn = s_turn[s_rhand], tilt = s_tilt[s_rhand];
                } else {
                    turn = std::round(std::atan2(pl.axis[1], pl.axis[0]) * kDeg);
                    tilt = std::round(std::asin(std::fmin(1.0f, std::fmax(-1.0f, pl.axis[2]))) * kDeg);
                }
            }
            const char* const kAngleTip = "The round's direction as two angles from along the fingers (a press 5 degrees; each goes "
                                          "all the way round); either makes it Custom.";
            bool ac = nudge1("Turn##Round turn (degrees)", &turn, 5.0f, -1000.0f, 1000.0f, "%+.0f", kAngleTip);
            ac |= nudge1("Tilt##Round tilt (degrees)", &tilt, 5.0f, -1000.0f, 1000.0f, "%+.0f", kAngleTip);
            if (ac) {
                while (turn > 180.0f) turn -= 360.0f;
                while (turn <= -180.0f) turn += 360.0f;
                while (tilt > 180.0f) tilt -= 360.0f;
                while (tilt <= -180.0f) tilt += 360.0f;
                pl.axis_mode = 2;
                pl.axis[0] = std::cos(tilt / kDeg) * std::cos(turn / kDeg);
                pl.axis[1] = std::cos(tilt / kDeg) * std::sin(turn / kDeg);
                pl.axis[2] = std::sin(tilt / kDeg);
                s_turn[s_rhand] = turn, s_tilt[s_rhand] = tilt, s_kept[s_rhand] = true;
                std::memcpy(s_made[s_rhand], pl.axis, sizeof(pl.axis));
                ch = done = true;
            }
        }
        const bool bc = nudge1("Brightness##Round brightness", &pl.bright, 0.25f, 0.25f, 3.0f, "%.2f",
                               "How bright the drawn rounds are, in the hand and at the chest (a press 0.25).");
        ch |= bc;
        done |= bc;
        if (ch) round_draw::set_place(pl, false, s_rhand);
        if (done) round_draw::set_place(pl, true, s_rhand);
    }

    heading("Every gun's rings (a press 5 mm)");
    note("A gun's own (the Guns tab, This gun) replace these. Offsets right / up / forward along the gun.");
    {
        float fo[3], fr = 0;
        holster::foregrip(fo, &fr);
        ImGui::TextUnformatted("The foregrip: where John's front hand sits on the gun");
        if (nudge3("Offset##Foregrip offset", fo, 0.005f, 0.3f, "Where John's front hand sits on the gun."))
            holster::set_foregrip(fo, fr, true);
        float zo[3], zr = 0;
        holster::foregrip_zone(zo, &zr);
        ImGui::TextUnformatted("The foregrip ring: where your front hand takes hold");
        const char* const kZoneTip = "Where your front hand takes hold of the gun, and the ring's size (Show the weapon points "
                                     "draws it); John's hand still sits at the foregrip.";
        bool zch = nudge3("Offset##Foregrip ring offset", zo, 0.005f, 0.3f, kZoneTip);
        zch |= nudge1("Size##Foregrip ring size", &zr, 0.005f, 0.03f, 0.4f, "%.3f", kZoneTip);
        if (zch) holster::set_foregrip_zone(zo, zr, true);
    }
    {  // run 6 item 3: the loading point
        float lo[3], lr = 0;
        bool touch = false;
        holster::load_point(lo, &lr, &touch);
        ImGui::TextUnformatted("The loading ring: where a round goes in");
        const char* const kLoadTip = "Where a round goes into the gun in hand, and the ring's size: a round brought inside it goes in.";
        bool ch = nudge3("Offset##Loading point offset", lo, 0.005f, 0.3f, kLoadTip);
        ch |= nudge1("Size##Loading point size", &lr, 0.005f, 0.03f, 0.4f, "%.3f", kLoadTip);
        ch |= check("A round goes in at a touch", &touch,
                    "The round itself touching the ring loads it. Off: your wrist has to be inside the ring.");
        if (ch) holster::set_load_point(lo, lr, touch, true);
    }

    heading("Each gun's action by hand");
    bool ma = actions::enabled();
    if (check("Work each gun's action by hand", &ma,
              "Open a revolver with the right stick (as chosen under Revolvers) and close it with a flick of the gun; work a bolt, "
              "lever or pump by hand after each shot. The trigger waits until you do."))
        actions::set_enabled(ma);
    sub_begin(ma);
    bool oc = actions::open_cylinder();
    if (check("Revolvers drawn open", &oc,
              "An open revolver is drawn open: the Schofield and LeMat tip down, the Double-action's cylinder swings out, the "
              "Cattleman's gate opens."))
        actions::set_open_cylinder(oc);
    {  // [Reload] RevolverControl (2026-10-26)
        const int rc = actions::revolver_control();
        const char* const kRevTip =
            "Opened by a flick: a flick of the right stick down opens a revolver. Cocked by a flick: after each shot, a flick of the "
            "right stick down cocks the Cattleman's, the Schofield's and the LeMat's hammer (the Double-action needs none); hold the "
            "stick down and click it to open a revolver. Either way a flick of the gun shuts it. A revolver drawn into your other "
            "hand is worked by that hand's stick (on foot).";
        note("Revolvers:");
        if (radio("Opened by a flick of the stick##revolver", rc == 0, kRevTip)) actions::set_revolver_control(0);
        ImGui::SameLine();
        if (radio("Cocked by a flick, opened by holding it and clicking##revolver", rc == 1, kRevTip)) actions::set_revolver_control(1);
        sub_begin(rc == 1);
        bool tc = actions::thumb_cock();  // [Reload] ThumbCock (2026-10-28)
        if (check("John's thumb cocks the hammer", &tc,
                  "On the stick's flick, John's thumb reaches up to the hammer and draws it back, then returns to the grip. The shot "
                  "is ready at the flick either way."))
            actions::set_thumb_cock(tc);
        bool fh = actions::fan_hammer();  // [Reload] FanHammer (2026-10-26)
        if (check("Fan the hammer with your other hand", &fh,
                  "Sweep your other hand's palm back across the hammer to cock it; keep the trigger pulled and each sweep fires."))
            actions::set_fan_hammer(fh);
        sub_end();
    }
    bool lp = actions::lever_parts();
    if (check("Lever guns: the lever drawn as you work it", &lp, "The lever drawn opening and closing as you work it, with the game's lever sounds."))
        actions::set_lever_parts(lp);
    {  // the lever: by the right stick held, the gun down then up (2026-11-02, GOAL-older-items item 3: the only way now)
        note("Lever guns: hold the right stick down, move the gun down to open the lever and up to shut it; the gun stays put meanwhile.");
        bool vs = actions::volcanic_stick();  // [Reload] VolcanicControl (2026-10-26)
        if (check("Volcanic: its lever by a flick of the right stick", &vs,
                  "After each shot, a flick of the right stick down works the Volcanic's lever, whatever the lever guns use."))
            actions::set_volcanic_stick(vs);
    }
    bool pp = actions::pump_parts();
    if (check("Pump-action: the fore-end in your front hand", &pp,
              "After a shot, pull the fore-end back with your front hand (held two-handed) and push it forward."))
        actions::set_pump_parts(pp);
    bool bo = actions::bolt_parts();
    if (check("Bolt actions: the bolt in your other hand", &bo,
              "Bolt Action and Carcano: grip the bolt with your other hand, lift, pull back (rounds go in now), push forward and turn "
              "it down."))
        actions::set_bolt_parts(bo);
    bool cbo = actions::carried_bolt();  // [Reload] CarriedBolt (2026-10-28)
    if (check("Bolt actions: the bolt by your gun hand when carried by the foregrip", &cbo,
              "With a bolt gun carried by its foregrip (Carry a long gun by its foregrip), your free gun hand works the bolt as your "
              "other hand does in hand. A shot not yet cycled goes with the gun and stays so until you work the bolt; grip near the "
              "gun's grip to take it back. The carried gun never fires."))
        actions::set_carried_bolt(cbo);
    {  // [Reload] RamrodAtChest (2026-11-01, GOAL-offhand-changes C15)
        bool rac = actions::ramrod_at_chest();
        if (check("Blunderbuss: the ramrod at your chest", &rac,
                  "After the round is down the barrel, take the ramrod at your chest with your other hand, bring it to the muzzle, "
                  "push it in and pull it out: loaded. Off: your bare hand at the muzzle makes two ram strokes, as before."))
            actions::set_ramrod_at_chest(rac);
        if (rac) {  // (2026-11-02, round 67-68 check 3, the user: the ramrod lower in the hand, adjustable as the round; taken from the gun)
            bool fg = actions::ramrod_from_gun();
            ImGui::TextUnformatted("Take the ramrod:");
            ImGui::SameLine();
            if (radio("From the gun##Ramrod from", fg, "Grip it with your other hand where it lies under the Blunderbuss's barrel.")) fg = true;
            ImGui::SameLine();
            if (radio("At your chest##Ramrod from", !fg, "Grip it at your chest with your other hand.")) fg = false;
            if (fg != actions::ramrod_from_gun()) actions::set_ramrod_from_gun(fg, true);
            if (fg) {
                float go[3];
                actions::ramrod_grab_offset(go);
                if (nudge3("Where it is taken on the gun##Ramrod grab", go, 0.005f, 0.3f,
                           "The ramrod's grab point on the gun: right, up, forward along the gun (a press 5 mm)."))
                    actions::set_ramrod_grab_offset(go, true);
            }
            static int s_rhand = 0;
            ImGui::TextUnformatted("The ramrod in your:");
            ImGui::SameLine();
            if (radio("Left hand##Ramrod hand", s_rhand == 0, "Set where the ramrod sits in your left hand.")) s_rhand = 0;
            ImGui::SameLine();
            if (radio("Right hand##Ramrod hand", s_rhand == 1, "Set where the ramrod sits in your right hand (until you change it, the left's).")) s_rhand = 1;
            actions::RodPlace rp = actions::ramrod_place(s_rhand);
            const char* const kRodTip = "Where the ramrod sits in your hand: right, up, forward along your hand (a press 5 mm), and its "
                                        "direction from where your hand points (a press 5 degrees).";
            bool rc = nudge3("The ramrod in the hand##Ramrod offset", rp.offset, 0.005f, 0.2f, kRodTip);
            rc |= nudge1("Turn##Ramrod turn (degrees)", &rp.turn, 5.0f, -180.0f, 180.0f, "%+.0f", kRodTip);
            rc |= nudge1("Tilt##Ramrod tilt (degrees)", &rp.tilt, 5.0f, -180.0f, 180.0f, "%+.0f", kRodTip);
            if (rc) actions::set_ramrod_place(s_rhand, rp, true);
        }
    }
    {  // [Reload] OffHandBoltTrigger (2026-11-01, GOAL-offhand-changes C5)
        bool obt = actions::offhand_bolt_trigger();
        if (check("Bolt actions: the bolt by the off hand's trigger", &obt,
                  "Work the bolt of the gun in your hand with your other hand's trigger (pull it at the bolt and make the strokes); "
                  "that trigger does not aim while it is at the bolt. Off: by its grip, as before."))
            actions::set_offhand_bolt_trigger(obt);
    }
    {  // [Reload] CarriedBoltTrigger (2026-10-30, round 55)
        bool cbt = actions::carried_bolt_trigger();
        sub_begin(cbo);
        if (check("The carried bolt by the trigger", &cbt,
                  "Work the carried gun's bolt with your gun hand's trigger (pull it at the bolt and make the strokes); your grip then "
                  "always takes the gun back, even beside the bolt. Off: by the grip, as before (a grip nearer the gun's grip than its "
                  "bolt takes it back)."))
            actions::set_carried_bolt_trigger(cbt);
        sub_end();
    }
    bool cpu = actions::carried_pump();  // [Reload] CarriedPump (2026-10-29)
    if (check("Pump-action: pumped by a jerk when carried by the foregrip", &cpu,
              "With the Pump-action carried by its foregrip (Carry a long gun by its foregrip), snap that hand back along the barrel "
              "to pump it back, then forward to close it: a one-handed pump. A shot not yet pumped goes with the gun; grip near the "
              "gun's grip to take it back as you left it. The carried gun never fires."))
        actions::set_carried_pump(cpu);
    bool br = actions::breech_parts();
    if (check("Single-shot rifles opened by hand", &br,
              "Open the Springfield's trapdoor and the Rolling Block's block with your other hand (the Buffalo by a flick down and "
              "up), load one round, shut."))
        actions::set_breech_parts(br);
    bool sa = actions::semi_auto_parts();
    if (check("Semi-Auto Shotgun: the bolt racked by hand", &sa,
              "After loading it from empty, pull back the bolt's handle (right of the receiver) with your other hand and let go."))
        actions::set_semi_auto_parts(sa);
    bool pm = actions::pistol_mags();  // [Reload] PistolMagazines (2026-10-28)
    if (check("Pistols: magazines (the Mauser: a stripper clip)", &pm,
              "Semi-Automatic Pistol and High Power: flick the right stick down to drop the magazine (a round stays in the chamber if "
              "the gun wasn't empty; its rounds go back to your ammo). Take a new one with your other hand (at your chest, or as "
              "\"Rounds come from\" says) and push it up into the bottom of the grip: the gun is loaded. Mauser: pull its bolt back by "
              "the ears with your other hand (it stays open when empty), push a stripper clip down into the top, then pull the bolt "
              "and let go (or flick the stick down) to close it. No single rounds go into these three."))
        actions::set_pistol_mags(pm);
    if (pm) {  // (2026-10-29, round 38: "don't seem to move with the ammo adjustments") the held magazine's own place
        sub_begin(true);
        static int s_mhand = 0;  // (2026-10-31, R13: each hand its own)
        if (radio("Left hand##Magazine hand", s_mhand == 0, "Set the magazine's place in your left hand.")) s_mhand = 0;
        ImGui::SameLine();
        if (radio("Right hand##Magazine hand", s_mhand == 1, "Set the magazine's place in your right hand (until you change it, the left "
                                                            "hand's).")) s_mhand = 1;
        // (2026-11-01, GOAL-offhand-changes C38; the user: each pistol's magazine set separately, and for each hand) a magazine
        // pistol in hand: its own place (until set: the hand's for every pistol); none in hand: every pistol's
        const int gw = holster::gun_in_hand(), mw = gw >= 1 && gw <= 3 ? gw : -1;
        bool mown = false;
        round_draw::MagPlace mp = round_draw::mag_place(s_mhand, mw, &mown);
        char mh[160];
        if (mw >= 0)
            std::snprintf(mh, sizeof(mh), "The %s's magazine in this hand (%s)", holster::weapon_label(mw), mown ? "its own" : "now every pistol's");
        else
            std::snprintf(mh, sizeof(mh), "Every pistol's magazine in this hand (hold a pistol to give it its own)");
        ImGui::TextUnformatted(mh);
        const char* const kMpTip = "Where the magazine (the Mauser's clip) sits in your hand, apart from the rounds' place on the Reloading "
                                   "tab (a press 2 mm, or 5 degrees). Its fit into the gun follows it. With a pistol in hand: that pistol's.";
        bool mc = nudge3("Magazine in the hand##Magazine offset", mp.offset, 0.002f, 0.12f, kMpTip);
        mc |= nudge1("Turn##Magazine turn (degrees)", &mp.turn, 5.0f, -180.0f, 180.0f, "%+.0f", kMpTip);
        mc |= nudge1("Tilt##Magazine tilt (degrees)", &mp.tilt, 5.0f, -90.0f, 90.0f, "%+.0f", kMpTip);
        if (mc) round_draw::set_mag_place(mp, true, s_mhand, mw);
        if (mw >= 0 && reset_button("Use every pistol's##Magazine own", mown, "Clears this pistol's own magazine place for this hand."))
            round_draw::clear_mag_place(s_mhand, mw);
        sub_end();
    }
    bool psl = actions::pistol_slides();  // [Reload] PistolSlides (2026-10-28)
    if (check("Pistols: the slide racked by hand", &psl,
              "Semi-Automatic Pistol (its toggle), High Power and Mauser (its bolt): grip the slide with your other hand, pull it back "
              "and let go to chamber a round. Racking with a round already chambered throws it out (it is lost). After the last shot "
              "the slide stays back: put a new magazine in, then rack it. With nothing chambered the trigger does nothing."))
        actions::set_pistol_slides(psl);
    bool mb = actions::manual_break();
    if (check("Break actions opened by the stick", &mb,
              "Double-barrel and Sawed-off: hold the right stick down and click it to open (with [Reload] BreakOpenClick=0 in the "
              "ini: a flick down); shut with a flick of the gun up, or swing the barrels up with your other hand."))
        actions::set_manual_break(mb);
    sub_begin(mb);
    bool bs = actions::barrel_hand_snap();
    if (check("John's hand on the barrels as they swing up", &bs, "While your other hand holds the open barrels, John's hand is drawn on them."))
        actions::set_barrel_hand_snap(bs);
    sub_end();
    sub_begin(bo || br || sa || psl || pm);  // (PistolSlides, the Mauser's bolt with PistolMagazines: the hand on the slide)
    bool ps = actions::part_hand_snap();
    if (check("John's hand on the part your other hand grips", &ps,
              "While your other hand works a bolt or a breech, John's hand is drawn on it, moving with it."))
        actions::set_part_hand_snap(ps);
    sub_end();
    bool hc = actions::held_click();  // round 13 item 13
    if (check("A click when fired before the action is worked", &hc,
              "A trigger pull before the gun is closed or worked plays the empty click."))
        actions::set_held_click(hc);
    sub_end();

    heading("An empty gun");
    const int em = static_cast<int>(audio::empty_click_mode());
    const char* const names[4] = {"Clicks", "Buzzes", "Clicks and buzzes", "Does nothing"};
    const char* const kClickTip = "What an empty gun's trigger does: a click from the gun's side, a controller buzz, both, or nothing.";
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (radio(names[i], em == i, kClickTip)) audio::set_empty_click_mode(static_cast<audio::ClickMode>(i));
    }
    sub_begin(em == 0 || em == 2);
    float vol = audio::click_volume();
    if (nudge1("Click volume", &vol, 0.05f, 0.0f, 1.0f, "%.2f",
               "The click's loudness, 0 to 1 (the game's volume settings do not change it; the Windows mixer does). Each press plays "
               "it once.")) {
        audio::set_click_volume(vol, true);  // saved, and one click at the new level: the gun in hand's
        RdrvrActorState st{};
        audio::preview(api::actor_state(&st) ? st.weapon : -1);
    }
    sub_end();
}

void page_gestures() {
    heading("By hand");
    float sw = gestures::swing_speed(), tg = gestures::throw_gain();
    bool th = gestures::throw_by_hand();
    if (check("Throw by hand", &th,
              "Dynamite, fire bottles, throwing knives and tomahawks fly with your hand's speed and direction: pull the trigger and "
              "swing."))
        gestures::set_throw_by_hand(th);
    sub_begin(th);
    if (nudge1("Throw strength", &tg, 0.1f, 0.5f, 4.0f, "%.1fx", "How hard throws fly, times your hand's speed (a press 0.1)."))
        gestures::set_tuning(sw, tg, true);
    bool gt = aim::grip_throw();
    if (check("Throw by letting go of the grip", &gt,
              "Hold your gun hand's grip, pull the trigger to light or ready the throwable, then swing and let go of the grip to "
              "throw. A lit one held too long goes off in your hand."))
        aim::set_grip_throw(gt);
    sub_end();
    bool me = gestures::melee_by_swing();
    if (check("Melee by swing", &me, "A fast swing of either hand punches with the fists, stabs with the knife or strikes with the torch."))
        gestures::set_melee_by_swing(me);
    bool la = gestures::lasso_by_hand();
    if (check("Lasso by hand", &la, "Throw the lasso with its trigger or a swing (as chosen below); yank back to pull on the rope."))
        gestures::set_lasso_by_hand(la);
    sub_begin(la);
    {  // 2026-10-12: [Gestures] LassoThrow, LassoAim
        int lt = gestures::lasso_throw(), lt0 = lt;
        ImGui::TextUnformatted("Thrown by:");
        ImGui::SameLine();
        if (radio("Trigger", lt == 1, "The lasso hand's trigger throws it.")) lt = 1;
        ImGui::SameLine();
        if (radio("Swing", lt == 0, "A fast swing toward where you aim throws it.")) lt = 0;
        ImGui::SameLine();
        if (radio("Either", lt == 2, "A swing or the trigger.")) lt = 2;
        ImGui::SameLine();
        if (radio("Lock on", lt == 3,
                  "Hold the left trigger (LT, the game's aim) to lock on to the one you look at and spin the lasso; then the right "
                  "trigger (RT) or a throwing swing throws it (not without the hold), the lasso in either hand. Let go without "
                  "either: nothing thrown."))
            lt = 3;  // 2026-10-30 (item 12): LassoThrow=lock
        if (lt != lt0) gestures::set_lasso_throw(lt, true);
        int am = gestures::lasso_aim(), am0 = am;
        if (lt == 3) note("Lock on aims by your head (the choice below applies to the other ways).");
        ImGui::TextUnformatted("Aimed by:");
        ImGui::SameLine();
        if (radio("Head", am == 1, "Where you look.")) am = 1;
        ImGui::SameLine();
        if (radio("Lasso hand", am == 0, "Where the lasso hand points.")) am = 0;
        ImGui::SameLine();
        if (radio("Other hand", am == 2, "Where your other hand points: point at the target, throw with the lasso hand.")) am = 2;
        if (am != am0) gestures::set_lasso_aim(am, true);
    }
    sub_end();
    sub_begin(me || la);
    if (nudge1("Swing speed", &sw, 0.25f, 1.0f, 8.0f, "%.2f m/s", "How fast a swing must be to punch or throw the lasso (a press 0.25 m/s)."))
        gestures::set_tuning(sw, tg, true);
    sub_end();

    heading("The gun-butt melee");  // run 7 item 2: [Gestures] GunMelee
    bool gm = gun_melee::enabled();
    if (check("Gun-butt melee", &gm,
              "Hit someone by swinging the gun in your hand: the game's own melee blow, a knock-out by default, with its crime and "
              "anger."))
        gun_melee::set_enabled(gm, true);
    sub_begin(gm);
    float hs = gun_melee::config().speed;
    if (nudge1("Hit speed", &hs, 0.25f, 1.0f, 6.0f, "%.2f m/s", "How fast the swing must be to hit (a press 0.25 m/s)."))
        gun_melee::set_speed(hs, true);
    sub_end();

    heading("Signs");  // run 8 item 4, run 10 items 5 and 7: [Gestures] Whistle, TipHat, Guard
    bool wh = whistle::enabled();
    if (check("Whistle for the horse", &wh,
              "A hand at your mouth and its trigger: John whistles for his horse. The gun hand whistles only with no gun in it."))
        whistle::set_enabled(wh, true);
    bool fb = controls::fists_both_triggers();  // 2026-10-14
    if (check("Fists on both triggers", &fb,
              "With nothing in your hands, hold both triggers for the fighting stance and swing a fist to punch while holding them; "
              "raise both fists before your face to block. A trigger alone does nothing. Off: each trigger as the game has it."))
        controls::set_fists_both_triggers(fb, true);
    bool guard = gestures::guard();
    if (check("Guard with both fists", &guard,
              "With your fists out, raise both hands before your face and hold both triggers to block. Fists raised without the "
              "triggers stay lowered for the game (raised fists alarm people)."))
        gestures::set_guard(guard, true);
    bool tiph = whistle::tip_hat_enabled();
    if (check("Tip the hat to greet##Tip the hat to greet", &tiph,
              "Your right hand at your hat's brim and its trigger: John tips his hat and greets (the game's greet button)."))
        whistle::set_tip_hat(tiph, true);

    heading("Loose objects");
    bool hc = physics::enabled();
    if (check("Hands push loose objects (experimental)##Hands push loose objects", &hc,
              "An empty hand moving through a bottle, a crate or a chair pushes it. People are never pushed."))
        physics::set_enabled(hc);
    bool gb = physics::grab_enabled();
    if (check("Grab loose objects (experimental)##Grab loose objects", &gb,
              "Grip a loose prop with an empty hand to hold it; let go to throw it with your hand's speed. People are never grabbed."))
        physics::set_grab_enabled(gb);
}

void page_controls() {
    heading("Left-handed");
    bool lh = controls::left_handed();
    if (check("Left-handed", &lh, "Swaps the controllers: the left trigger fires and the right stick moves."))
        controls::set_left_handed(lh);
    sub_begin(lh);
    bool gih = body::gun_in_gun_hand();
    if (check("The gun in John's left hand", &gih,
              "John holds the gun in his left hand, each arm on its own controller. Off: his right arm reaches to your left "
              "controller (the arms cross)."))
        body::set_gun_in_gun_hand(gih);
    sub_end();

    heading("The trigger and the aim");
    bool ta = controls::trigger_aims();
    if (check("The trigger alone fires", &ta, "The right trigger alone fires; the mod holds the game's aim button for you."))
        controls::set_trigger_aims(ta);
    bool ar = controls::aim_when_raised();
    if (check("Aim while the gun is raised", &ar,
              "Raising the gun in front of you holds the game's aim for you, so every pull fires at once."))
        controls::set_aim_when_raised(ar);
    bool sd = controls::sprint_drops_aim();
    if (check("Sprinting lowers the raised gun", &sd, "The sprint button lowers a raised gun so John can run with it; a pull still fires."))
        controls::set_sprint_drops_aim(sd);
    bool jd = controls::jump_drops_aim();  // run 7 item 1f: [Hands] JumpDropsAim
    if (check("Jumping lowers the raised gun", &jd,
              "The jump button lowers a raised gun first, so John can jump, vault and climb with a gun in hand."))
        controls::set_jump_drops_aim(jd, true);

    heading("Buttons");
    note("Each VR button drives this game button:");
    const char* names[controls::kTargets];
    for (int g = 0; g < controls::kTargets; ++g) names[g] = controls::target_name(g);
    const float col2 = ImGui::GetCursorPosX() + 0.5f * ImGui::GetContentRegionAvail().x;  // two columns
    for (int s = 0; s < controls::kSources; ++s) {
        int cur = controls::mapping(s);
        if (s % 2) ImGui::SameLine(col2);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo(controls::source_label(s), &cur, names, controls::kTargets)) controls::set_mapping(s, cur);
        track(controls::source_label(s));
        help("The game button this VR button drives. A trigger set to a button presses it past halfway; a button set to LT or RT "
             "pulls it fully.");
    }
    if (reset_button("Reset to the game's scheme", true, "Every VR button back to the game button of its own name."))
        controls::reset_mapping();
    note("The menu: hold the menu button (without one: both B buttons), hold Y with the wrist HUD in view, or F7. A tap of the menu "
         "button pauses; held with the right stick it is the D-pad, with A the satchel. The right stick scrolls the menu.");
}

void page_advanced() {
    char line[400];
    xr::perf_status(line, sizeof(line));
    ImGui::TextWrapped("%s", line);
    vr_mode::status_text(line, sizeof(line));
    ImGui::TextWrapped("%s", line);
    note("These are on for a reason. Turn one off only to compare, or if it misbehaves for you.");

    heading("Fixes you can turn off");
    bool fg = body::fixed_gun_grip();  // run 7 item 1c: [Hands] FixedGunGrip
    if (check("Long guns held by their aiming grip", &fg,
              "Each long gun held the way the game holds it when aiming, in every pose. Off: the game's own hold of each moment."))
        body::set_fixed_gun_grip(fg, true);
    bool fs = body::fixed_sidearm_grip();  // 2026-10-09: [Hands] FixedSidearmGrip
    if (check("Sidearms held by their aiming grip", &fs,
              "Each revolver or pistol held the way the game holds it aimed, so it moves with your hand at once. Off: the game's own "
              "hold, which can lag your hand when the game runs slowly."))
        body::set_fixed_sidearm_grip(fs, true);
    {
        body::HandCfg hc = body::hand_cfg();
        if (check("Arms follow the controllers", &hc.ik,
                  "John's arms reach to your controllers and his hands turn with them. Off: the game's own arm animation.")) {
            config::set("Hands", "ArmIK", hc.ik ? "1" : "0");
            body::set_hand_cfg(hc);
        }
    }
    bool shared = dual_pass::split_on("shadows");
    if (check("Shadows drawn once for both eyes", &shared,
              "Shadows drawn once for both eyes instead of once per eye: about 0.9 ms faster a frame; a shadow's edge can shift by a "
              "pixel."))  {
        dual_pass::set_split("shadows", shared);
        config::set("Stereo", "Split_shadows", shared ? "1" : "0");
    }
    bool gst = body::gun_steady();  // (2026-11-02, GOAL-older-items item 21) [Hands] GunSteady
    if (check("Steady gun far out", &gst, "The gun in your hand doesn't tremble far from the map's middle (the game's positions there step by half a millimetre): held where it was while your hand and head stay still."))
        body::set_gun_steady(gst, true);
    bool nfl = body::no_flutter();  // (2026-11-02, GOAL-older-items item 23) [Body] NoFlutter
    if (check("Still clothes", &nfl, "John's clothes don't flutter in the wind (seen from inside the body it reads as a buzz)."))
        body::set_no_flutter(nfl, true);
    bool amv = body::arm_motion_vectors();  // run 12 item 9: [Body] ArmMotionVectors
    if (check("Arm motion vectors (experimental)##Arm motion vectors", &amv,
              "Tells DLSS and TAA how John's arms moved, which may cut the ghosting beside fast-moving arms."))
        body::set_arm_motion_vectors(amv, true);

    // 2026-10-30 (item 10, the user: "Make launcher appear when running the game through steam/rockstar or the exe"):
    // [Launcher] ShowAtStart and StartTimeout, read at the game's start (launcher_start.h)
    heading("The launcher");
    bool las = launcher_start::show_at_start();
    if (check("Show the launcher when the game starts (next start)##Launcher at start", &las,
              "RDRVR_Launcher.exe opens on the desktop each time the game starts, and the game waits for its Play: the settings saved "
              "there apply to that start. Off: the game starts at once, as before."))
        launcher_start::set_show_at_start(las);
    sub_begin(las);
    float lto = static_cast<float>(launcher_start::start_timeout());
    if (nudge1("Goes on by itself after##Launcher countdown", &lto, 5.0f, 0.0f, 300.0f, "%.0f s",
               "If nobody presses Play (you started the game from the headset), the game goes on after this many seconds with the "
               "saved settings. A click in the launcher stops the countdown; 0: no countdown (the game still goes on after 5 "
               "minutes). Applies at the next start."))
        launcher_start::set_start_timeout(static_cast<int>(lto + 0.5f));
    sub_end();
}

// 2026-10-29 (the user: "a mod menu page for it ... only visible if mod is installed ... all adjustments its own menu
// can"): Ped Gunfight Overhaul's settings (pgo.h): its five, its Save and Reload, and its keyboard menu on or off
void page_gunfights() {
    static std::string result;  // the last Save's or Reload's outcome
    const pgo::Link link = pgo::link();
    note(link == pgo::Link::kLive
             ? "Ped Gunfight Overhaul's own settings. A change applies within a second; Save keeps it for the next game."
             : link == pgo::Link::kOtherBuild
                   ? "This Ped Gunfight Overhaul is not version 1.1.5, so changes here go to its ini only: Save, then restart the game "
                     "or press its Reload key."
                   : "Ped Gunfight Overhaul is installed but not running (is RedHook on?), so changes here go to its ini only: Save, "
                     "then restart the game.");
    pgo::Values v = pgo::get();
    bool changed = false;
    heading("NPCs");
    double acc = v.ped_accuracy, ph = v.ped_health, cr = v.cripple_at, pl = v.player_health;
    if (nudge_held("NPC accuracy (%)", &acc, 0.01, 0.0, 200.0, "%.2f",
                   "How well NPCs aim: 100 is the game's own, 50 half as well, 200 twice as well. Hold an arrow to change it faster."))
        v.ped_accuracy = static_cast<float>(acc), changed = true;
    if (nudge_held("NPC health", &ph, 1.0, 0.0, 1000000.0, "%.0f",
                   "How much NPCs can take: 25 is the game's own, 50 twice as much. 0: they cannot be hurt. Hold an arrow to change it "
                   "faster."))
        v.ped_health = static_cast<int>(ph), changed = true;
    if (nudge_held("Cripple at (%)##Cripple", &cr, 1.0, 0.0, 100.0, "%.0f",
                   "NPCs shot may limp, kneel or crawl only once their health is at or below this share. 100 is the game's own (any "
                   "time); 0: never."))
        v.cripple_at = static_cast<int>(cr), changed = true;
    if (check("Headshots always kill", &v.headshot_kills, "An NPC shot in the head dies, whatever their health."))
        changed = true;
    heading("You");
    if (nudge_held("Your health", &pl, 1.0, 0.0, 1000000.0, "%.0f",
                   "How much John can take: 100 is the game's own (35 its hardcore), 200 twice as much. 0: he cannot be hurt. Hold an "
                   "arrow to change it faster."))
        v.player_health = static_cast<int>(pl), changed = true;
    heading("Its settings file");
    if (check("Its keyboard controls", &v.in_game_controls,
              "Ped Gunfight Overhaul's own menu on the keyboard's arrow keys (and its Numpad 5 save, Numpad 0 reload). Saved, it stays "
              "as set next game."))
        changed = true;
    if (changed) {
        pgo::set(v);
        result.clear();
    }
    std::string err;
    if (ImGui::Button("Save##Gunfights save")) result = pgo::save(&err) ? "Saved." : "Not saved: " + err + ".";
    track("Gunfights save");
    help("Write these settings into PedGunfightOverhaul.ini, as its Numpad 5 does, so the next game starts with them.");
    ImGui::SameLine();
    if (ImGui::Button("Reload##Gunfights reload")) result = pgo::reload(&err) ? "Reloaded." : "Not reloaded: " + err + ".";
    track("Gunfights reload");
    help("Take the settings saved in PedGunfightOverhaul.ini back, as its Numpad 0 does: undoes the changes not saved.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", !result.empty() ? result.c_str() : pgo::unsaved() ? "Changes not saved." : "");
}

void draw() {
    if (!g_set.loaded) load_settings();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kW), static_cast<float>(kH)));
    g_help.clear();
    ImGui::Begin("RDRVR", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    page_buttons();
    // the page: all but the help line's three rows
    const float help_h = 3.0f * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##page", ImVec2(0.0f, -help_h), ImGuiChildFlags_Borders);
    if (g_page_changed) {
        ImGui::SetScrollY(0.0f);
        g_page_changed = false;
    }
    if (const float sy = g_scroll_req.exchange(-1.0f); sy >= 0.0f) ImGui::SetScrollY(sy);
    stick_scroll();
    g_scroll_now.store(ImGui::GetScrollY(), std::memory_order_relaxed);
    g_scroll_max.store(ImGui::GetScrollMaxY(), std::memory_order_relaxed);
    switch (g_page) {
        case kDisplay: page_display(); break;
        case kComfort: page_comfort(); break;
        case kBody: page_body(); break;
        case kHolsters: page_holsters(); break;
        case kGuns: page_guns(); break;
        case kReloading: page_reloading(); break;
        case kGestures: page_gestures(); break;
        case kControls: page_controls(); break;
        case kAdvanced: page_advanced(); break;
        case kGunfights: page_gunfights(); break;
        default: break;
    }
    ImGui::EndChild();
    ImGui::TextWrapped("%s", g_help.empty() ? kIdleHelp : g_help.c_str());
    ImGui::End();
}

// "menu grab <path.bmp>" (run 7 item 5, a test aid): the menu's image (its target, the pointer included) copied at its
// next draw into a readback buffer (made at the first grab), written by the test thread as a 24-bit BMP over black
std::atomic<int> g_grab_phase{0};  // 1 armed (the test thread), 2 copied (the presenting thread), 0 idle
ID3D12Resource* g_grab_rb = nullptr;
std::atomic<uint64_t> g_grab_fence{0};
HANDLE g_grab_done = nullptr;
constexpr UINT kGrabPitch = kW * 4;  // R8G8B8A8, a multiple of 256

bool grab_buffer(ID3D12Device* dev) {
    if (g_grab_rb) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(kGrabPitch) * kH;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_grab_rb)))) {
        log::error("[menu] the grab's readback buffer could not be made");
        g_grab_rb = nullptr;
        return false;
    }
    return true;
}

bool render(ID3D12Resource* dst) {
    ID3D12CommandQueue* q = xr::image_queue();  // the menu's quad image: an XR image
    ID3D12Device* dev = state::device.load();
    int s = g_slot;
    if (!fence_reached(g_fence, g_slot_value[s], g_event, 50)) return false;
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
    const bool grab = g_grab_phase.load() == 1 && grab_buffer(dev);
    if (grab) {  // the menu's image copied out before it goes back to being read
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = g_grab_rb;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dl.PlacedFootprint.Offset = 0;
        dl.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(kW), static_cast<UINT>(kH), 1, kGrabPitch};
        sl.pResource = g_rt;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.SubresourceIndex = 0;
        g_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    } else {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    }
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
    if (grab) {
        g_grab_fence.store(g_fence_value);
        g_grab_phase = 2;
        SetEvent(g_grab_done);
    }
    return true;
}

// ---- the desktop (2026-10-29, "Mod menu isn't visible in recordings"; research/run28/desktop_menu.md): the menu target
// (premultiplied sRGB values over transparent black, coverage in alpha: ImGui's own output) drawn over the back buffer in
// display space (ONE, INV_SRC_ALPHA: ImGui's composite on an sRGB-valued window), RGB only (the back buffer's alpha
// kept). The quad's four corners are the vertices' clip positions (x y 0 w, w the eye-space depth), so the texture is
// mapped perspective-correct; a minified menu is box-filtered over each pixel's footprint (up to 3 x 3 bilinear taps).
const char kDeskShader[] = R"(
Texture2D<float4> menu : register(t0);
SamplerState lin : register(s0);
cbuffer K : register(b0) { float4 corner[4]; };  // TL TR BL BR
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.pos = corner[id];
    o.uv = float2(id & 1, id >> 1);
    return o;
}
float4 ps(V i) : SV_Target {
    float2 sz = float2(1024.0, 768.0);
    float2 dx = ddx(i.uv), dy = ddy(i.uv);
    int2 n = clamp(int2(ceil(float2(length(dx * sz), length(dy * sz)) - 0.001)), 1, 3);
    float4 s = 0;
    [loop] for (int y = 0; y < n.y; ++y)
        [loop] for (int x = 0; x < n.x; ++x)
            s += menu.SampleLevel(lin, i.uv + ((x + 0.5) / n.x - 0.5) * dx + ((y + 0.5) / n.y - 0.5) * dy, 0);
    return saturate(s / float(n.x * n.y));
}
)";

std::atomic<bool> g_desk_on{true};  // "menu desktop on|off" (the session)
bool g_desk_made = false, g_desk_failed = false, g_drawn_now = false;
ID3D12RootSignature* g_desk_root = nullptr;
ID3D12PipelineState* g_desk_pso = nullptr;
DXGI_FORMAT g_desk_fmt = DXGI_FORMAT_UNKNOWN;
ID3DBlob *g_desk_vs = nullptr, *g_desk_ps = nullptr;
ID3D12DescriptorHeap* g_desk_rtv = nullptr;
ID3D12CommandAllocator* g_desk_alloc[kSlots] = {};
ID3D12GraphicsCommandList* g_desk_list = nullptr;
ID3D12Fence* g_desk_fence = nullptr;  // signalled on the present queue
HANDLE g_desk_event = nullptr;
uint64_t g_desk_value = 0, g_desk_slot_value[kSlots] = {};
int g_desk_slot = 0;
std::atomic<uint64_t> g_desk_drawn{0}, g_desk_flat{0}, g_desk_behind{0}, g_desk_skipped{0};
std::atomic<int> g_desk_eye{-1};
std::atomic<int> g_desk_box[4] = {};  // the last drawn copy's bounding box in the back buffer (pixels, cut to it)
std::atomic<int> g_desk_corner[8] = {};  // its corners TL TR BL BR (x y pixels, not cut: r53's perspective reference)
std::atomic<uint32_t> g_desk_bw{0}, g_desk_bh{0}, g_desk_bfmt{0};

DXGI_FORMAT desk_view(DXGI_FORMAT f) {  // the back buffer's display-referred SDR view; UNKNOWN: not drawn (HDR, or other)
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        // R10G10B10A2 is left out: it may be HDR10 (ST.2084), and the mod does not know the swapchain's colour space
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool desk_make(ID3D12Device* dev) {
    if (g_desk_made) return true;
    if (g_desk_failed) return false;
    g_desk_failed = true;  // one attempt; the reason is logged
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!fn) {
        log::error("[menu] the desktop copy: %s, so the menu stays in the headset only", dc ? "d3dcompiler_47.dll has no D3DCompile" : "d3dcompiler_47.dll not loaded");
        return false;
    }
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(fn(kDeskShader, sizeof(kDeskShader) - 1, "rdrvr_menu_desktop", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &g_desk_vs, &err));
    if (err) {
        log::error("[menu] desktop vs: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
        err = nullptr;
    }
    ok = ok && SUCCEEDED(fn(kDeskShader, sizeof(kDeskShader) - 1, "rdrvr_menu_desktop", nullptr, nullptr, "ps", "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_desk_ps, &err));
    if (err) {
        log::error("[menu] desktop ps: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    if (ok) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 16};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{2, params, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob* sig = nullptr;
        ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr)) &&
             SUCCEEDED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_desk_root)));
        if (sig) sig->Release();
    }
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ok = ok && SUCCEEDED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_desk_rtv)));
    for (auto& al : g_desk_alloc) ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)));
    ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_desk_alloc[0], nullptr, IID_PPV_ARGS(&g_desk_list))) &&
         SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_desk_fence)));
    if (ok) g_desk_list->Close();
    g_desk_event = ok ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    if (!ok || !g_desk_event) {
        log::error("[menu] the desktop copy's objects were not created: the menu stays in the headset only");
        return false;
    }
    g_desk_made = true;
    g_desk_failed = false;
    return true;
}

DXGI_FORMAT g_desk_bad_fmt = DXGI_FORMAT_UNKNOWN;  // a format whose pipeline was not created (not tried again)

bool desk_pso(ID3D12Device* dev, DXGI_FORMAT f) {
    if (g_desk_pso && f == g_desk_fmt) return true;
    if (f == g_desk_bad_fmt) return false;
    if (g_desk_pso) {  // a new format (the game's frame made again): the old pipeline released once its lists are done
        if (!fence_reached(g_desk_fence, g_desk_value, g_desk_event, 50)) return false;
        g_desk_pso->Release();
    }
    g_desk_pso = nullptr;
    g_desk_fmt = f;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_desk_root;
    pd.VS = {g_desk_vs->GetBufferPointer(), g_desk_vs->GetBufferSize()};
    pd.PS = {g_desk_ps->GetBufferPointer(), g_desk_ps->GetBufferSize()};
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = f;
    pd.SampleDesc.Count = 1;
    const pso::OwnCreation own_pso;  // (run 19: [Stereo] InstancedShadersAuto leaves the mod's own pipelines alone)
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_desk_pso)))) {
        log::limited("menu.desk.pso", 4, "[menu] the desktop copy's pipeline for format %d was not created (not tried again)", static_cast<int>(f));
        g_desk_pso = nullptr;
        g_desk_fmt = DXGI_FORMAT_UNKNOWN;
        g_desk_bad_fmt = f;
        return false;
    }
    return true;
}

// The quad's corners (TL TR BL BR, the menu's u v 0 0 .. 1 1) as clip positions in views[eye], NDC over the eye image;
// false when one is behind the eye (or nearly: 5 cm), or the FOV is degenerate.
bool project(const XrView& v, float out[16]) {
    const float hw = 0.5f * kQuadW, hh = 0.5f * kQuadH;
    const float loc[4][3] = {{-hw, hh, 0}, {hw, hh, 0}, {-hw, -hh, 0}, {hw, -hh, 0}};
    const float L = std::tan(v.fov.angleLeft), R = std::tan(v.fov.angleRight), U = std::tan(v.fov.angleUp), D = std::tan(v.fov.angleDown);
    if (!(R - L > 1e-3f) || !(U - D > 1e-3f)) return false;
    const XrQuaternionf inv{-v.pose.orientation.x, -v.pose.orientation.y, -v.pose.orientation.z, v.pose.orientation.w};
    for (int i = 0; i < 4; ++i) {
        float w[3], e[3];
        rotate(g_pose.orientation, loc[i], w);
        const float rel[3] = {g_pose.position.x + w[0] - v.pose.position.x, g_pose.position.y + w[1] - v.pose.position.y,
                              g_pose.position.z + w[2] - v.pose.position.z};
        rotate(inv, rel, e);
        const float depth = -e[2];  // the eye looks down -z
        if (!(depth > 0.05f)) return false;
        const float tx = e[0] / depth, ty = e[1] / depth;
        out[4 * i + 0] = (2.0f * tx - (R + L)) / (R - L) * depth;
        out[4 * i + 1] = (2.0f * ty - (U + D)) / (U - D) * depth;
        out[4 * i + 2] = 0.0f;
        out[4 * i + 3] = depth;
    }
    return true;
}

}  // namespace

void set_desktop(bool on) {
    g_desk_on = on;
    log::info("[menu] desktop copy %s (the session)", on ? "on" : "off");
}

bool desktop(const XrView* views, int eye, uint32_t content_w, uint32_t content_h, bool flat) {
    const bool drawn = g_drawn_now;
    g_drawn_now = false;
    if (!drawn || !g_ready || !g_desk_on.load(std::memory_order_relaxed)) return false;
    if (!flat && (!views || eye < 0 || eye > 1)) return false;
    // the swapchain this Present presents (inside its frame end); the present queue is the newest swapchain's, so another
    // one being presented is left alone
    IDXGISwapChain* sc = d3d::presenting_swapchain();
    if (!sc) sc = state::swapchain.load();
    if (sc != state::swapchain.load()) {
        log::limited("menu.desk.sc", 2, "[menu] the desktop copy: a swapchain other than the newest is presenting: not drawn");
        g_desk_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ID3D12CommandQueue* q = state::present_queue.load();  // the game's back buffer stays on the present queue (xr.h)
    ID3D12Device* dev = state::device.load();
    if (!sc || !q || !dev || !desk_make(dev)) return false;
    float c[16];
    if (!flat && !project(views[eye], c)) {
        g_desk_behind.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const int s = g_desk_slot;
    if (!fence_reached(g_desk_fence, g_desk_slot_value[s], g_desk_event, 20)) {  // this slot's last copy still on the GPU (never expected)
        g_desk_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return false;
    ID3D12Resource* bb = nullptr;
    const HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) return false;
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    const DXGI_FORMAT f = desk_view(bd.Format);
    const float bw = static_cast<float>(bd.Width), bh = static_cast<float>(bd.Height);
    g_desk_bw.store(static_cast<uint32_t>(bd.Width), std::memory_order_relaxed);
    g_desk_bh.store(bd.Height, std::memory_order_relaxed);
    g_desk_bfmt.store(static_cast<uint32_t>(bd.Format), std::memory_order_relaxed);
    if (f == DXGI_FORMAT_UNKNOWN || !desk_pso(dev, f)) {
        if (f == DXGI_FORMAT_UNKNOWN)
            log::limited("menu.desk.fmt", 1, "[menu] the desktop copy is not drawn into a back buffer of format %d (possibly HDR, or other)",
                         static_cast<int>(bd.Format));
        g_desk_skipped.fetch_add(1, std::memory_order_relaxed);
        bb->Release();
        return false;
    }
    // the eye image's place in the back buffer (the viewport: NDC over it), and the screen box for the status
    float r[4] = {0.0f, 0.0f, bw, bh};
    if (!flat && content_w && content_h) {  // eye_shape's repaint: the eye rect scaled to fit and centred (xr_blit's ps_mirror)
        const float k = std::fmin(bw / content_w, bh / content_h);
        r[0] = 0.5f * (bw - content_w * k);
        r[1] = 0.5f * (bh - content_h * k);
        r[2] = r[0] + content_w * k;
        r[3] = r[1] + content_h * k;
    }
    if (flat) {  // centred, 60% of the height at the menu's aspect (at most 90% of the width)
        float h = 0.6f * bh, w = h * kW / kH;
        if (w > 0.9f * bw) {
            w = 0.9f * bw;
            h = w * kH / kW;
        }
        const float nx = w / bw, ny = h / bh;
        const float fc[16] = {-nx, ny, 0, 1, nx, ny, 0, 1, -nx, -ny, 0, 1, nx, -ny, 0, 1};
        std::memcpy(c, fc, sizeof(c));
    }
    {
        float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
        for (int i = 0; i < 4; ++i) {
            const float x = r[0] + (0.5f + 0.5f * c[4 * i] / c[4 * i + 3]) * (r[2] - r[0]);
            const float y = r[1] + (0.5f - 0.5f * c[4 * i + 1] / c[4 * i + 3]) * (r[3] - r[1]);
            g_desk_corner[2 * i].store(static_cast<int>(std::lround(x)), std::memory_order_relaxed);
            g_desk_corner[2 * i + 1].store(static_cast<int>(std::lround(y)), std::memory_order_relaxed);
            x0 = x < x0 ? x : x0;
            y0 = y < y0 ? y : y0;
            x1 = x > x1 ? x : x1;
            y1 = y > y1 ? y : y1;
        }
        auto cut = [](float v, float lo, float hi) { return static_cast<int>(v < lo ? lo : v > hi ? hi : v); };
        g_desk_box[0].store(cut(x0, r[0], r[2]), std::memory_order_relaxed);
        g_desk_box[1].store(cut(y0, r[1], r[3]), std::memory_order_relaxed);
        g_desk_box[2].store(cut(x1, r[0], r[2]), std::memory_order_relaxed);
        g_desk_box[3].store(cut(y1, r[1], r[3]), std::memory_order_relaxed);
    }
    bool ok = SUCCEEDED(g_desk_alloc[s]->Reset()) && SUCCEEDED(g_desk_list->Reset(g_desk_alloc[s], g_desk_pso));
    if (ok) {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_desk_rtv->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = f;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView(bb, &rv, rtv);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_desk_list->ResourceBarrier(1, &b);
        g_desk_list->SetGraphicsRootSignature(g_desk_root);
        g_desk_list->SetDescriptorHeaps(1, &g_srv_heap);
        g_desk_list->SetGraphicsRoot32BitConstants(0, 16, c, 0);
        g_desk_list->SetGraphicsRootDescriptorTable(1, g_rt_srv);  // the menu target, in PIXEL_SHADER_RESOURCE after render()
        g_desk_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        const D3D12_VIEWPORT vp{r[0], r[1], r[2] - r[0], r[3] - r[1], 0, 1};
        const D3D12_RECT sr{static_cast<LONG>(r[0]), static_cast<LONG>(r[1]), static_cast<LONG>(r[2]), static_cast<LONG>(r[3])};
        g_desk_list->RSSetViewports(1, &vp);
        g_desk_list->RSSetScissorRects(1, &sr);
        g_desk_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g_desk_list->DrawInstanced(4, 1, 0, 0);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        g_desk_list->ResourceBarrier(1, &b);
        ok = SUCCEEDED(g_desk_list->Close());
    }
    if (ok) {
        d3d::submit_internal(q, g_desk_list);
        g_desk_slot_value[s] = ++g_desk_value;
        q->Signal(g_desk_fence, g_desk_value);
        g_desk_slot = (s + 1) % kSlots;
        g_desk_drawn.fetch_add(1, std::memory_order_relaxed);
        if (flat) g_desk_flat.fetch_add(1, std::memory_order_relaxed);
        g_desk_eye.store(flat ? -1 : eye, std::memory_order_relaxed);
    }
    bb->Release();
    return ok;
}

void init() { g_grab_done = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

std::string grab(const std::string& path) {
    if (!g_grab_done) return "ERROR no grab event (NOT MEASURED)";
    if (!g_visible.load() || !g_ready) return "ERROR the menu is not open (NOT MEASURED)";
    ResetEvent(g_grab_done);
    g_grab_phase = 1;
    if (WaitForSingleObject(g_grab_done, 3000) != WAIT_OBJECT_0) {
        g_grab_phase = 0;
        return "ERROR no menu frame drew (NOT MEASURED)";
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool copied = ev && SUCCEEDED(g_fence->SetEventOnCompletion(g_grab_fence.load(), ev)) && WaitForSingleObject(ev, 3000) == WAIT_OBJECT_0;
    if (ev) CloseHandle(ev);
    if (!copied) {
        g_grab_phase = 0;
        return "ERROR the copy timed out (NOT MEASURED)";
    }
    uint8_t* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(kGrabPitch) * kH};
    std::string out;
    if (SUCCEEDED(g_grab_rb->Map(0, &r, reinterpret_cast<void**>(&p))) && p) {
        // a 24-bit BMP, bottom-up; the menu's colours are premultiplied over transparent black, so its RGB is the image
        // over black
        constexpr uint32_t kRow = kW * 3, kSize = 54 + kRow * kH;
        uint8_t hdr[54] = {'B', 'M'};
        auto put32 = [&hdr](int at, uint32_t v) {
            for (int i = 0; i < 4; ++i) hdr[at + i] = static_cast<uint8_t>(v >> (8 * i));
        };
        put32(2, kSize);
        put32(10, 54);
        put32(14, 40);
        put32(18, kW);
        put32(22, kH);
        hdr[26] = 1;   // planes
        hdr[28] = 24;  // bits
        put32(34, kRow * kH);
        std::vector<uint8_t> row(kRow);
        std::wstring wpath(path.begin(), path.end()), tmp = wpath + L".tmp";
        FILE* f = nullptr;
        bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
        if (ok) {
            fwrite(hdr, 1, sizeof(hdr), f);
            for (int y = kH - 1; y >= 0; --y) {
                const uint8_t* src = p + static_cast<size_t>(y) * kGrabPitch;
                for (int x = 0; x < kW; ++x) {
                    row[x * 3 + 0] = src[x * 4 + 2];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 0];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        D3D12_RANGE none{0, 0};
        g_grab_rb->Unmap(0, &none);
        const int pg = g_page_shared.load(std::memory_order_relaxed);
        char line[400];
        std::snprintf(line, sizeof(line), "%s %s %dx%d (the %s page, scroll %.0f of %.0f)", ok ? "wrote" : "ERROR writing", path.c_str(), kW, kH,
                      pg >= 0 && pg < kPages ? kPageName[pg] : "?", g_scroll_now.load(), g_scroll_max.load());
        out = line;
    } else {
        out = "ERROR Map failed (NOT MEASURED)";
    }
    g_grab_phase = 0;
    return out;
}

bool visible() { return g_visible.load(); }

void set_visible(bool on) {
    if (on && !g_visible.load()) g_place = true;
    g_visible = on;
    log::info("[menu] %s", on ? "open" : "closed");
}

bool frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* quad) {
    g_drawn_now = false;  // (set again below when this frame's quad is drawn: desktop() draws only that)
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
    {  // run 7 item 5: the gun hand's stick scrolls the window under the laser: the right stick (the left when left-handed),
       // never the off hand's, whose Y opens and closes the menu at the wrist; not while the menu button or the B+B
       // chord is held (with it the stick is the D-pad, controls.cpp). Pushed forward: up the page. While the menu is
       // open the controllers do not drive the game's pad (controls::pad), so the stick reaches neither the game nor
       // the turning.
        static double last_ms = 0.0;
        const double now = log::now_ms();
        float dt = last_ms > 0.0 ? static_cast<float>((now - last_ms) / 1000.0) : 0.0f;
        last_ms = now;
        if (!(dt > 0.0f) || dt > 0.1f) dt = 1.0f / 90.0f;
        const hands::Hand l = hands::get(0), r = hands::get(1), s = controls::layout_gun_hand() ? r : l;
        const bool chord = ((l.buttons | r.buttons) & hands::kMenu) || ((l.buttons & hands::kB) && (r.buttons & hands::kB));
        const float sy = s.stick[1], a = std::fabs(sy);
        g_stick_px = hit && !chord && a > kStickDead ? (sy > 0.0f ? -1.0f : 1.0f) * (a - kStickDead) / (1.0f - kStickDead) * kScrollSpeed * dt : 0.0f;
    }
    {
        d3d::InternalSubmitScope internal;
        const pso::OwnCreation own_pso;  // (ImGui's backend makes its pipelines here)
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
    g_drawn_now = true;
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
    char out[240];
    // then its row in the window (pixels from the top, of kH), the page's scroll, and the rows of its window that are
    // seen (the page's under the tabs and above the help line; a tab's the whole window): a row outside them is not seen
    std::snprintf(out, sizeof(out), "%.4f %.4f %.4f row %.0f of %d scroll %.0f view %.0f %.0f", g_pose_shared.position.x + w[0],
                  g_pose_shared.position.y + w[1], g_pose_shared.position.z + w[2], cy, kH, g_scroll_now.load(std::memory_order_relaxed),
                  it->second.v0, it->second.v1);
    return out;
}

void status_text(char* out, size_t len) {
    const int p = g_page_shared.load(std::memory_order_relaxed);
    const int de = g_desk_eye.load(std::memory_order_relaxed);
    char eye[16];
    std::snprintf(eye, sizeof(eye), de < 0 ? "flat" : "eye %d", de);
    // then the desktop copy (2026-10-29): on|off, frames drawn (of them flat), not drawn with a corner behind the eye,
    // skipped, the last one's box in the back buffer (x0 y0 x1 y1 pixels), the mirrored eye, the back buffer, the last
    // one's corners (TL TR BL BR, x y pixels each, not cut to the window)
    std::snprintf(out, len, "menu %s, %s, frames %llu, clicks %llu, scrolled by the stick %llu px, page %s scroll %.0f of %.0f | desktop %s, "
                  "drawn %llu (flat %llu), behind %llu, skipped %llu, box %d %d %d %d, %s, back buffer %ux%u format %u, corners %d %d %d %d %d %d %d %d",
                  g_visible.load() ? "open" : "closed", g_ready ? "ready" : "not made", static_cast<unsigned long long>(g_frames.load()),
                  static_cast<unsigned long long>(g_clicks.load()), static_cast<unsigned long long>(g_stick_scrolled.load()),
                  p >= 0 && p < kPages ? kPageName[p] : "?", g_scroll_now.load(std::memory_order_relaxed),
                  g_scroll_max.load(std::memory_order_relaxed), g_desk_on.load() ? "on" : "off",
                  static_cast<unsigned long long>(g_desk_drawn.load()), static_cast<unsigned long long>(g_desk_flat.load()),
                  static_cast<unsigned long long>(g_desk_behind.load()), static_cast<unsigned long long>(g_desk_skipped.load()),
                  g_desk_box[0].load(), g_desk_box[1].load(), g_desk_box[2].load(), g_desk_box[3].load(), eye, g_desk_bw.load(),
                  g_desk_bh.load(), g_desk_bfmt.load(), g_desk_corner[0].load(), g_desk_corner[1].load(), g_desk_corner[2].load(),
                  g_desk_corner[3].load(), g_desk_corner[4].load(), g_desk_corner[5].load(), g_desk_corner[6].load(), g_desk_corner[7].load());
}

}  // namespace rdrvr::menu
