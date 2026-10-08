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
#include "core/holster.h"
#include "core/reload.h"
#include "core/gestures.h"
#include "core/gun_melee.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/dual.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/eye_shape.h"
#include "core/hands.h"
#include "core/log.h"
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

// run 7 item 5: the tabs as buttons in two rows (13 tabs do not fit one ImGui tab bar at this size), each its page in
// a child window that scrolls under them, and a help line under the page: the explanation of the row under the laser
enum Page : int {
    kGeneral, kComfort, kHands, kHolsters, kGunInHand, kWeapons,
    kReloading, kActions, kSounds, kGestures, kControls, kScreen, kDebug, kPages
};
const char* const kPageName[kPages] = {"General",   "Comfort", "Hands",  "Holsters", "Gun in hand", "Weapons", "Reloading",
                                       "Actions",   "Sounds",  "Gestures", "Controls", "Screen",     "Debug"};
constexpr int kRow1 = 6;          // the first row's buttons
constexpr float kTabH = 44.0f;    // a tab button's height (px)
constexpr float kLabelW = 210.0f; // nudge1: its arrows from here (the label before), the value kValueW wide
constexpr float kValueW = 150.0f;
int g_page = kGeneral;            // presenting thread
bool g_page_changed = false;      // a new page opens at its top
std::string g_help;               // this frame's help line (the row under the laser)
std::atomic<float> g_scroll_max{0.0f};
std::atomic<int> g_page_shared{kGeneral};
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

// round 13: an adjustment by arrows (sliders were hard to set): for right, up and forward a left and a right arrow,
// one step a press, the value between; true when a press changed it. Each arrow tracked as "<name> <axis> minus" /
// "plus", the name the label's part after "##" (else the label). Run 7 item 5: the row's help line, every
// adjustment by arrows (no sliders left)
bool nudge3(const char* label, float v[3], float step, float lim = 1.0f, const char* tip = nullptr) {
    static const char* const kAxis[3] = {"right", "up", "forward"};
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
        ImGui::Text("%s %+.3f", kAxis[i], v[i]);
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
    ImGui::TextUnformatted(shown_part(label).c_str());
    ImGui::SameLine(kLabelW);
    if (ImGui::ArrowButton("##dn", ImGuiDir_Left)) *v -= step, changed = true;
    std::snprintf(name, sizeof(name), "%s minus", nm);
    track(name);
    ImGui::SameLine();
    ImGui::Text(fmt, *v);
    ImGui::SameLine(kLabelW + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + kValueW);
    if (ImGui::ArrowButton("##up", ImGuiDir_Right)) *v += step, changed = true;
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
    for (int p = 0; p < kPages; ++p) {
        const bool row1 = p < kRow1;
        const int n = row1 ? kRow1 : kPages - kRow1, i = row1 ? p : p - kRow1;
        const float w = std::floor((avail - static_cast<float>(n - 1) * st.ItemSpacing.x) / static_cast<float>(n));
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

void page_general() {
    heading("The HUD");
    bool wrist = xr::hud_on_wrist();
    if (check("HUD on the left wrist", &wrist,
              "The radar, its meters and the ammo counter on the back of the left hand (the right one when left-handed), shown "
              "while you look at it (the palm flat, face down, like a watch); the prompts stay on the floating quad."))
        xr::set_hud_on_wrist(wrist);
    heading("Anti-aliasing");
    // DLSS is chosen for the next start: switching to or from it while the game runs would re-create its swapchain
    const char* aa[] = {"Off", "FXAA", "Native TAA", "DLSS (from the next start)"};
    int a = g_set.aa < 0 ? 1 : g_set.aa > 3 ? 3 : g_set.aa;
    ImGui::SetNextItemWidth(380);
    if (ImGui::Combo("Anti-aliasing", &a, aa, 4)) {
        g_set.aa = a;
        if (a <= 2) render_settings::set_aa(a);  // refused (kept for the next start) while DLSS runs
        config::set("Render", "ForceAntiAliasing", std::to_string(a));
    }
    track("Anti-aliasing");
    help("FXAA is the default; native TAA is the game's own, a history per eye. DLSS (DLAA, a history per eye) is chosen for "
         "the next start: switching to or from it while the game runs would re-create its swapchain.");
    if (g_set.aa == 3) {  // the game's quality index: what DLSS renders before it upscales to the game's resolution
        sub_begin(true);
        static const char* const kQ[6] = {"DLAA (native resolution)", "Dynamic (DLAA here)", "Ultra performance (1/3 per axis)",
                                          "Performance (1/2 per axis)", "Balanced (0.58 per axis)", "Quality (2/3 per axis)"};
        int q = g_set.dlss_q < 0 ? 0 : g_set.dlss_q > 5 ? 5 : g_set.dlss_q;
        ImGui::SetNextItemWidth(380);
        if (ImGui::Combo("DLSS quality", &q, kQ, 6)) {
            g_set.dlss_q = q;
            config::set("Render", "DlssQuality", std::to_string(q));
        }
        track("DLSS quality");
        help("What DLSS renders before it upscales to the game's resolution: DLAA (native) is the sharpest and the dearest. From "
             "the next start.");
        sub_end();
    }
    const int running = render_settings::forced_aa();
    if (running >= 0 && ((running == 3) != (g_set.aa == 3) || (running == 3 && g_set.dlss_q != render_settings::dlss_quality())))
        note("Restart the game to apply (DLSS can only change at the start).");
    // [XR] EyeShape (run 7 item 4): with FXAA only (the mode stands aside for the other techniques)
    sub_begin(g_set.aa == 1);
    bool eye = eye_shape::enabled();
    if (check("Eyes in the headset's shape", &eye,
              "Each eye rendered in the headset's own shape and size (square pixels) instead of the game's 16:9 frame stretched "
              "over it: fewer pixels drawn for the same sharpness. With FXAA only. The eye images take their size at the start: "
              "turned on now, the eye is drawn inside the 16:9 images until the next start.")) {
        if (!eye_shape::set_enabled(eye, true)) note("Not available: the hooks it needs are not installed (see the log).");
    }
    sub_end();
    // [Render] RenderResolution: the game's frame at a headset's height, apart from the monitor (from the next start)
    heading("Resolution");
    {  // 2026-10-09: the headset first, then its size at 100%, 150% or 200% of its pixels
        const int cur = render_res::choice(), sc = render_res::scale();
        char why[192];
        const std::string preview = cur >= 0 ? render_res::headset_label(cur) : std::string("Custom (the ini's RenderResolution)");
        ImGui::TextUnformatted("Render resolution: headset");
        ImGui::SetNextItemWidth(560);
        const bool open = ImGui::BeginCombo("##Render headset", preview.c_str(), ImGuiComboFlags_HeightLargest);  // all its rows seen
        if (!open) track("Render resolution: headset");
        help("The game's frame from the next start: your headset's screen height, at the size below. For the size your runtime "
             "(SteamVR, Pimax Play, Virtual Desktop...) asks for, choose Automatic. Windowed mode keeps your window's size.");
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
            help("The headset's own resolution at 100%, or 150% and 200% of its pixels (each side about 1.22 and 1.41 times): the "
                 "frame stays 16:9, its height set by the headset's eyes.");
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
        // Automatic: the runtime's own size for its lenses (2026-10-09: a Dream Air's 4036x3376 under SteamVR); a note, not
        // the list's help (the open list covers the help line)
        if (cur == 1)
            note("Automatic takes the size your runtime (SteamVR, Pimax Play, Virtual Desktop...) asks for, for its lenses. Its own "
                 "resolution setting multiplies with the size above: keep it at its default (SteamVR's 100%).");
        uint32_t want_w = 0, want_h = 0, run_w = 0, run_h = 0;
        const bool want = cur >= 0 && render_res::choice_size(cur, sc, &want_w, &want_h);
        const bool run = render_res::active(&run_w, &run_h);
        if (xr::frame_resized()) note("The game's frame changed size while running (its Graphics menu): restart the game for the VR view.");
        else if (want != run || (want && (want_w != run_w || want_h != run_h))) note("Restart the game to apply.");
    }
}

void page_comfort() {
    heading("The view");
    if (ImGui::Button("Recentre")) camera_lever::recentre();
    track("Recentre");
    help("The view, the body's heading, the cinema screen and the HUD in front of you again. Also: the headset's own "
         "recentre, Scroll Lock with the game in front, or both stick clicks held a second.");
    bool fp = pose::anchor_enabled();
    if (check("First person", &fp,
              "The view at John's eyes (the camera anchor): his root plus the eye height, facing the body's heading, moved by "
              "your head. Off: the game's own camera, turned by your head.")) {
        pose::set_anchor(fp);
        config::set("Body", "CameraAnchor", fp ? "1" : "0");
    }
    float lift = pose::eye_lift();
    if (nudge1("Eye height", &lift, 0.01f, -0.2f, 0.3f, "%+.2f m",
               "Added to the eye height measured from John's head bone (the seated lift; a press 1 cm).")) {
        pose::set_eye_lift(lift);
        save_float("Body", "SeatedLift", "%.2f", lift);
    }

    heading("The drawn body");
    {
        body::HandCfg hc = body::hand_cfg();
        int show = hc.show;
        ImGui::TextUnformatted("Shown:");
        const char* const kShowTip = "What of John's body is drawn in first person (gameplay keeps his own pose and place).";
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
              "While crouching, in cover, on a horse or driving a wagon, only the forearms and hands are drawn (the body gets in "
              "the way of the view there).")) {
        body::set_auto_shows(as);
        config::set("Body", "AutoShow", as ? "1" : "0");
    }
    bool hide = body::hide_enabled();
    if (check("Hide head and hat", &hide,
              "The head and the hat hidden in first person (cutscenes and the cinema screen keep them); only what is drawn "
              "changes.")) {
        body::set_hide_enabled(hide);
        config::set("Body", "HideHead", hide ? "1" : "0");
    }
    int st = body::stance();
    {
        const char* const kStanceTip = "The drawn torso: upright (the spine, chest and shoulders over the hips; the arms and legs "
                                       "still animate), only the lean removed, or the game's animation.";
        ImGui::TextUnformatted("Torso:");
        ImGui::SameLine();
        if (radio("Upper body upright", st == 1, kStanceTip)) st = 1;
        ImGui::SameLine();
        if (radio("Remove lean only", st == 2, kStanceTip)) st = 2;
        ImGui::SameLine();
        if (radio("Game animation", st == 0, kStanceTip)) st = 0;
        if (st != body::stance()) {
            body::set_stance(st);
            config::set("Body", "Stance", st == 0 ? "game" : st == 2 ? "lean" : "upright");
        }
    }
    sub_begin(st == 1);
    float pitch = body::torso_pitch();
    if (nudge1("Torso pitch", &pitch, 1.0f, -20.0f, 20.0f, "%+.0f deg", "Degrees added to the upright torso: positive leans it forward.")) {
        body::set_torso_pitch(pitch);
        config::set("Body", "TorsoPitch", std::to_string(static_cast<int>(pitch)));
    }
    sub_end();
    float back = body::body_back();
    if (nudge1("Body back", &back, 0.01f, 0.0f, 0.4f, "%.2f m",
               "How far the body is drawn behind the camera's place, so looking down shows the chest from outside (a press 1 cm).")) {
        body::set_body_back(back);
        save_float("Body", "BodyBack", "%.2f", back);
    }
    bool lt = body::locks_torso();
    if (check("Keep the torso over the hips", &lt,
              "The upper body kept over the hips as at rest, the whole body moving with it: running no longer pushes the torso "
              "forward into view.")) {
        body::set_locks_torso(lt);
        config::set("Body", "LockTorso", lt ? "1" : "0");
    }
    bool lockf = body::locks_facing();
    if (check("Lock body facing", &lockf,
              "The drawn body faces the camera's heading, the legs too (walking sideways no longer turns his shoulder into view).")) {
        body::set_locks_facing(lockf);
        config::set("Body", "LockFacing", lockf ? "1" : "0");
    }
    bool follow = body::follows_head();
    if (check("Body moves with the headset", &follow,
              "The drawn body moves with your head's sideways and forward offset. Off: it stays where John is.")) {
        body::set_follows_head(follow);
        config::set("Body", "BodyFollowsHead", follow ? "1" : "0");
    }

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
    if (nudge1("Turn speed", &speed, 10.0f, 30.0f, 240.0f, "%.0f deg/s", "Smooth turning's speed at full tilt (a press 10 degrees a second).")) {
        pose::set_turning(pose::snap_turning(), speed, angle);
        config::set("Comfort", "TurnSpeed", std::to_string(static_cast<int>(speed)));
    }
    sub_end();
    sub_begin(turn == 1);
    if (nudge1("Snap angle", &angle, 5.0f, 10.0f, 90.0f, "%.0f deg", "Snap turning's step (a press 5 degrees).")) {
        pose::set_turning(pose::snap_turning(), speed, angle);
        config::set("Comfort", "SnapAngle", std::to_string(static_cast<int>(angle)));
    }
    sub_end();

    heading("Riding");
    int steer = pose::steer_by_head() ? 1 : 0;
    if (radio("Steer with the stick", steer == 0, "The view faces the horse and the left stick steers it; your head looks around freely."))
        steer = 0;
    ImGui::SameLine();
    if (radio("Steer with the head", steer == 1, "As on foot: the left stick goes where you look.")) steer = 1;
    if ((steer == 1) != pose::steer_by_head()) pose::set_steer_by_head(steer == 1);
    bool saddle = pose::saddle_anchor();
    float stau = pose::saddle_smoothing();
    if (check("View from the saddle", &saddle,
              "The camera above the horse's own position (no gait bounce in it), at the rider's eye height averaged over a "
              "second, instead of the bouncing head."))
        pose::set_saddle(saddle, stau, true);
    sub_begin(saddle);
    bool climb = pose::saddle_climb();
    if (check("Keep its height on climbs", &climb,
              "On a climb the view keeps its height (it used to sink to the saddle on a steep gallop) and never goes below the "
              "rider's seat + 0.5 m; on flat ground the same as before."))
        pose::set_saddle_climb(climb, true);
    if (nudge1("Saddle smoothing", &stau, 0.05f, 0.0f, 0.6f, "%.2f s", "The view's height smoothing (0 = none; a press 0.05 s)."))
        pose::set_saddle(saddle, stau, true);
    sub_end();
    bool cb = controls::click_brake();
    if (check("Left stick click brakes", &cb,
              "On a horse or a wagon the left stick click is the game's brake: hold it to slow down, with the stick pulled back for "
              "a hard stop (on foot it is still crouch)."))
        controls::set_click_brake(cb);
}

void page_hands() {
    heading("Where your hands grab (m, a press 5 mm)");
    {  // [Hands] InteractOffset (round 13): the arrows, 5 mm a press
        float go[3];
        holster::interact_offset(go);
        if (nudge3("Interaction spot", go, 0.005f, 1.0f,
                   "Where your hands grab (the holsters, the chest's rounds, the rings, an action's parts; the white dot the "
                   "holsters' rings show): moved from the wrist in your hand's axes, the left hand's right mirrored."))
            holster::set_interact_offset(go, true);
    }
    {  // 2026-10-07: how the controller sits in the hand, per controller type ([Controls] GripFit<Type>)
        float fo[3], fa[3];
        const char* nm = controllers::fit_name(1);
        if (nm && controllers::fit(1, fo, fa)) {
            char hd[128];
            std::snprintf(hd, sizeof(hd), "Controller fit: %s (m, a press 5 mm; degrees, a press 2)", nm);
            heading(hd);
            const char* const kFitTip = "How this controller type sits in your hand: the hands moved and turned from where the "
                                        "controller puts them (the right hand's; the left mirrored).";
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

    heading("John's arms");
    body::HandCfg hc = body::hand_cfg();
    bool changed = false;
    if (check("Arms follow the controllers", &hc.ik,
              "Each tracked controller's arm reaches its grip (the shoulder, elbow and wrist bent to it) and the hand turns to the "
              "controller. Only what is drawn: the game keeps its own arms for aiming and shooting.")) {
        changed = true;
        config::set("Hands", "ArmIK", hc.ik ? "1" : "0");
    }
    sub_begin(hc.ik);
    if (check("Arms stretch to reach the controllers", &hc.stretch,
              "Past John's reach the arm lengthens so the hand stays on the controller (the upper arm and the forearm alike; the "
              "hand keeps its size and turn).")) {
        changed = true;
        config::set("Hands", "ArmStretch", hc.stretch ? "1" : "0");
    }
    sub_begin(hc.stretch);
    if (nudge1("Longest arm", &hc.stretch_max, 0.05f, 1.0f, 1.6f, "%.2fx John's",
               "The longest arm, times John's (1.0 - 1.6); past it the hand stops short of the controller.")) {
        changed = true;
        save_float("Hands", "ArmStretchMax", "%.2f", hc.stretch_max);
    }
    sub_end();
    sub_end();
    if (nudge1("Wrist offset", &hc.wrist_offset, 0.005f, -0.1f, 0.2f, "%.3f m",
               "From the controller's grip back to John's wrist joint (a press 5 mm).")) {
        changed = true;
        save_float("Hands", "WristOffset", "%.3f", hc.wrist_offset);
    }
    {
        const char* const kTurnTip = "The hand's turn against the controller, degrees in the grip's frame (mirrored for the left "
                                     "hand; a press 2): until John's hand sits on the controller as yours does.";
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

    heading("The gun in hand");
    {  // 2026-10-07: the reticle where the shot lands
        bool rt = aim::reticle_on();
        if (check("Reticle where the shot lands", &rt,
                  "While aiming: a reticle at the game's own target point along the gun's barrel (where the bullet goes), the same "
                  "size near and far, red over a person or an animal."))
            aim::set_reticle_on(rt);
        sub_begin(rt);  // [Hands] ReticleStyle
        const bool dot = aim::reticle_dot();
        const char* const kStyleTip = "The reticle's look: a ring with a dot in its middle, or a dot only (smaller, less in the way).";
        if (radio("Ring and dot", !dot, kStyleTip)) aim::set_reticle_dot(false);
        ImGui::SameLine();
        if (radio("Dot only", dot, kStyleTip)) aim::set_reticle_dot(true);
        sub_end();
    }
    bool kd = holster::keeps_drawn();
    if (check("Keep the gun drawn", &kd,
              "A drawn gun stays drawn until you put it away (the game puts it away 3 s after the last aim or shot).")) {
        holster::set_keeps_drawn(kd);
        config::set("Hands", "KeepGunDrawn", kd ? "1" : "0");
    }
    bool be = aim::block_executions();  // round 13
    if (check("No executions up close", &be,
              "The trigger close to someone fires: the game's third-person execution, pistol whip or butt strike never starts "
              "from it."))
        aim::set_block_executions(be);
    bool pab = aim::shoot_past_arm_block();  // run 7 item 1e: [Hands] ShootPastArmBlock
    if (check("Shoot past the arm block", &pab,
              "The game drops every pull while the line from John's shoulder to where the gun points hits something; your gun can "
              "be over a wall his shoulder is behind. On: that block lifted while you aim (a friendly in the way still stops it)."))
        aim::set_shoot_past_arm_block(pab, true);
    bool fg = body::fixed_gun_grip();  // run 7 item 1c: [Hands] FixedGunGrip
    if (check("Long guns held by their aiming grip", &fg,
              "A long gun is held differently lowered or carried (the wrist higher on it); each gun's aiming hold is learned while "
              "the game aims (saved per gun) and used in every pose. Off: the game's own hold of each moment."))
        body::set_fixed_gun_grip(fg, true);

    heading("Dual wield");
    bool dw = dual::enabled();
    if (check("Dual wield", &dw,
              "With a gun in one hand, the free hand gripping another gun slot's holster takes that gun as a second: it fires on "
              "its own trigger, from its own barrel, with its own ammo."))
        dual::set_enabled(dw);
    bool dc = dual::copy_enabled();
    if (check("The same sidearm in both hands", &dc,
              "With a sidearm in hand, the free hand gripping a hip holster takes a copy of it: its own trigger, muzzle and clip; "
              "let go at a hip to put it back."))
        dual::set_copy_enabled(dc);
    sub_begin(dc);
    bool cap = dual::copy_as_prop();  // run 7 item 1: [Hands] CopyAsProp
    if (check("The copy drawn as a prop", &cap,
              "The copy of the same sidearm drawn as a prop of that gun's model, as your other sidearm's model is (the gun in hand "
              "drawn a second time at the free hand was never seen there). Off: the old second draw."))
        dual::set_copy_as_prop(cap, true);
    bool om = dual::own_model();
    if (check("Your other sidearm's model", &om,
              "The copy shows your other sidearm's own model when you own a different one (the hip's weapon, else the first "
              "other revolver you own); its shot leaves that model's muzzle."))
        dual::set_own_model(om);
    sub_begin(om);
    bool sh = dual::same_at_its_holster();  // round 13 item 10
    if (check("A second of the gun at its own holster", &sh,
              "The free hand at the gun in hand's own holster (the one it was drawn from) takes a second of that gun, its own "
              "model; the other hip still gives your other sidearm."))
        dual::set_same_at_its_holster(sh);
    sub_end();
    sub_end();
}

void page_holsters() {
    heading("The holsters");
    bool on = holster::holsters_enabled();
    if (check("Body holsters", &on,
              "Grip at a holster to draw its weapon; grip and let go there with that gun in hand to put it away. A short buzz "
              "when a hand reaches one; a grip used at a holster is not LB / RB."))
        holster::set_holsters_enabled(on);
    sub_begin(on);
    bool fists = holster::unarmed_after_holster();
    if (check("Putting a gun away selects the fists", &fists,
              "As the game's weapon wheel does: the left trigger is then the fist-fight stance, not a draw of the last gun."))
        holster::set_unarmed_after_holster(fists);
    bool dg = controls::draw_to_grabbing_hand();
    if (check("Draw into the hand that grabs the holster", &dg,
              "A holster draw puts the gun in the hand that grabbed the holster: its trigger then fires and the other hand aims "
              "and loads (right-handed, or left-handed with the gun in John's left hand)."))
        controls::set_draw_to_grabbing_hand(dg);
    {  // run 7 item 1d
        const char* const kAnchorTip = "What the holsters follow: John's drawn body (it stays where John is when you step in the "
                                       "room), or you (moved with your neck from where they are at a recentre). Riding and cover "
                                       "keep the body.";
        int an = holster::anchor();
        ImGui::TextUnformatted("They follow:");
        ImGui::SameLine();
        if (radio("John's body", an == 0, kAnchorTip)) holster::set_anchor(0);
        ImGui::SameLine();
        if (radio("Your headset", an == 1, kAnchorTip)) holster::set_anchor(1);
    }
    sub_end();

    heading("Weapons by");  // run 8 item 5: [Holsters] Mode
    const bool wm = wheel::wheel_mode();
    static const char* const kModeTip = "The holsters: reach to a holster and grip to draw or put away. The weapon wheel: hold a grip "
                                        "to open the game's weapon wheel, move the hand toward a weapon and let go to take it into that "
                                        "hand (the holsters then draw nothing; the chest still gives rounds).";
    if (radio("The holsters", !wm, kModeTip)) wheel::set_wheel_mode(false, true);
    ImGui::SameLine();
    if (radio("The weapon wheel", wm, kModeTip)) wheel::set_wheel_mode(true, true);

    heading("Shown");
    bool rings = holster::show_zones();
    if (check("Show the holsters", &rings, "Rings to place and size the holsters by eye: white, green with a hand in it, amber gripped."))
        holster::set_show_zones(rings);
    bool hdots = holster::show_hand_dots();  // run 8 item 5b
    if (check("Show the hand dots", &hdots, "A dot at each hand's grab point: green when it is in a holster or on the foregrip."))
        holster::set_show_hand_dots(hdots, true);
    bool wpts = holster::show_weapon_points();
    if (check("Show the weapon points", &wpts,
              "Where a gun takes your other hand: the foregrip ring on a long gun, where a held round goes in, and the action's hints "
              "(the open revolver, the bolt, the lever, the pump)."))
        holster::set_show_weapon_points(wpts, true);
    bool sg = holster::show_guns();
    if (check("Show the guns at their holsters", &sg,
              "Each holster's gun shown at it while holstered: a sidearm at the hip barrel down, a long gun across the back over "
              "its shoulder; hidden while that gun is in a hand."))
        holster::set_show_guns(sg);
    sub_begin(sg);
    bool sb = holster::show_back_guns();  // round 13 item 14
    if (check("The long guns on the back too", &sb, "Off: only the hips' guns are shown.")) holster::set_show_back_guns(sb);
    sub_end();

    heading("Each holster (m, a press 5 mm)");
    note("Each holster's offset right / up / forward from the drawn holster, and its size (its ring's radius).");
    for (int z = 0; z < holster::zone_count(); ++z) {
        float off[3], radius = 0;
        holster::zone(z, off, &radius);
        const std::string zl = holster::zone_label(z);
        ImGui::PushID(z);
        bool en = true;
        if (holster::zone_takes_weapons(z)) {
            en = holster::zone_enabled(z);
            if (check((zl + "##" + zl + " on").c_str(), &en, "This holster on or off.")) holster::set_zone_enabled(z, en, true);
        } else {
            ImGui::TextUnformatted(zl.c_str());
        }
        sub_begin(en);
        // round 13 item 9: arrows, a press one increment (saved at once)
        bool zc = nudge3(("Offset##" + zl + " offset").c_str(), off, 0.005f, 0.4f);
        zc |= nudge1(("Size##" + zl + " size").c_str(), &radius, 0.005f, 0.05f, 0.4f);
        if (zc) holster::set_zone(z, off, radius, true);
        sub_end();
        ImGui::PopID();
    }
}

// round 13 item 8: the gun in hand's own foregrip, foregrip ring and loading ring (else the Reloading tab's)
void page_gun() {
    const int w = holster::gun_in_hand();
    if (w < 0) {
        heading("The gun in hand");
        note("Hold a gun to give it its own foregrip and rings here; a gun without its own uses every gun's (the Reloading tab).");
        return;
    }
    char hd[160];
    std::snprintf(hd, sizeof(hd), "%s (m, a press 5 mm)", holster::weapon_label(w));
    heading(hd);
    note("This gun's own settings; without its own it uses every gun's (the Reloading tab). Offsets right / up / forward along "
         "the gun.");
    static const char* const kName[3] = {"The foregrip", "The foregrip ring", "The loading ring"};
    static const char* const kTip[3] = {"Where John's front hand sits on this gun: the game's grip moved.",
                                        "Where your front hand takes hold of this gun, moved from the grip, and its size.",
                                        "Where a round goes into this gun, moved from its loading point, and the ring's size."};
    static const char* const kTrack[3] = {"Gun foregrip", "Gun foregrip ring", "Gun loading ring"};
    for (int what = 0; what < 3; ++what) {
        float off[3], r = 0.0f;
        bool own = false;
        holster::gun_adjust(w, what, off, &r, &own);
        std::snprintf(hd, sizeof(hd), "%s (%s)", kName[what], own ? "its own" : "now every gun's");
        heading(hd);
        char lb[96];
        std::snprintf(lb, sizeof(lb), "Offset##%s", kTrack[what]);
        bool ch = nudge3(lb, off, 0.005f, 1.0f, kTip[what]);
        if (what > 0) {
            std::snprintf(lb, sizeof(lb), "Size##%s size", kTrack[what]);
            ch |= nudge1(lb, &r, 0.005f, 0.03f, 0.5f, "%.3f", kTip[what]);
        }
        if (ch) holster::set_gun_adjust(w, what, off, r, true);
        std::snprintf(lb, sizeof(lb), "Use every gun's##%s: every gun's", kTrack[what]);
        if (reset_button(lb, own, "This gun's own setting cleared: it uses every gun's (the Reloading tab) again."))
            holster::clear_gun_adjust(w, what);
    }
}

void page_weapons() {
    heading("The weapon in each holster");
    bool wc = holster::weapon_choice();
    if (check("Choose the weapon in each holster", &wc,
              "A holster draws the weapon chosen for it, and the game keeps it in that slot (its weapon wheel follows). "
              "Automatic: the game's weapon for that slot."))
        holster::set_weapon_choice(wc);
    sub_begin(wc);
    bool any = holster::any_weapon();
    if (check("List every weapon you own for every holster", &any,
              "Every owned weapon offered for every holster; a gun still holsters where the game carries it (a rifle on the back)."))
        holster::set_any_weapon(any);
    holster::Arsenal a{};
    const bool have = holster::arsenal(&a);
    if (!have) note("(your weapons are read in gameplay)");
    for (int z = 0; z < holster::zone_count(); ++z) {
        if (!holster::zone_takes_weapons(z)) continue;
        const int chosen = holster::zone_weapon(z);
        const std::string label = holster::zone_label(z);
        std::string preview = chosen < 0 ? std::string("Automatic (the game's weapon for it)") : holster::weapon_label(chosen);
        if (chosen >= 0 && have && !(a.owned >> chosen & 1)) preview += " (not owned: automatic)";
        ImGui::TextUnformatted(label.c_str());
        ImGui::Indent();
        ImGui::SetNextItemWidth(460);
        const bool open = ImGui::BeginCombo(("##" + label).c_str(), preview.c_str());
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
        ImGui::Unindent();
    }
    sub_end();
}

// run 7 item 5: the Reloading tab split in three: the reload by hand and the rings (here), the guns' actions, the sounds
void page_reloading() {
    heading("Reload by hand");
    bool v = reload::hand_reload();
    if (check("Reload by hand", &v,
              "Grip at the chest holster with the hand that has no gun (a buzz: a round) and bring it to the gun (a buzz in both "
              "hands: one round in, from your spare ammo). One round at a time; not in Dead Eye."))
        reload::set_hand_reload(v);
    bool sa_ = round_draw::show_ammo();
    if (check("Show the rounds at the chest", &sa_,
              "While a round could go in (a gun open for it, or one loaded through its gate with room), a row of its rounds at the "
              "chest, as many as could go in (up to six)."))
        round_draw::set_show_ammo(sa_);
    {
        int sq = reload::chest_squeeze();
        const char* const kSqTip = "The gun hand gripped at the chest holster: nothing, one round in, or the whole clip at once "
                                   "(from the spare rounds).";
        ImGui::TextUnformatted("The gun hand squeezed at the chest holster:");
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
              "The game's own reloads back on beside the hand reload: after the last round, drawing an empty gun, pulling the "
              "trigger on an empty gun."))
        reload::set_automatic(v);
    v = reload::button();
    if (check("The game's reload button", &v, "The game's own reload button back on beside the hand reload.")) reload::set_button(v);

    heading("The round in the hand");
    bool rh = round_draw::enabled();
    if (check("A round seen in the hand", &rh,
              "A cartridge drawn in the hand that holds one taken at the chest (a revolver's, a rifle's or a shotgun shell, by the "
              "gun in hand), hidden where the scene is nearer (the fingers, the gun)."))
        round_draw::set_enabled(rh);
    {  // run 6 item 5: where the round sits in the hand
        note("Its place: moved from the pinch (m, the hand's axes; a press 2 mm); its direction (degrees from along the fingers, a "
             "press 5); its brightness (the chest's rounds too).");
        round_draw::Place pl = round_draw::place();
        bool ch = nudge3("Offset##Round offset", pl.offset, 0.002f, 0.08f,
                         "The round moved from the pinch between the drawn thumb and index tips, in the hand's wrist axes.");  // round 13 item 9: arrows
        bool done = ch;
        const char* const kAxis[3] = {"Along the fingers", "Out of the hand", "Custom"};
        const char* const kAxisTip = "The round's base to tip direction: along the fingers (the bullet out past the fingertips), "
                                     "out of the hand (from the wrist through the pinch), or your own by the two angles.";
        ImGui::TextUnformatted("Direction:");
        for (int i = 0; i < 3; ++i) {
            ImGui::SameLine();
            if (radio(kAxis[i], pl.axis_mode == i, kAxisTip)) {
                pl.axis_mode = i;
                ch = done = true;
            }
        }
        {  // round 13 item 12: its direction as two angles from along the fingers, a press 5 degrees (Custom)
            constexpr float kDeg = 57.29578f;
            float turn = 0.0f, tilt = 0.0f;
            if (pl.axis_mode != 1) {
                turn = std::round(std::atan2(pl.axis[1], pl.axis[0]) * kDeg);
                tilt = std::round(std::asin(std::fmin(1.0f, std::fmax(-1.0f, pl.axis[2]))) * kDeg);
            }
            const char* const kAngleTip = "The round's direction as two angles from along the fingers (a press 5 degrees); either "
                                          "makes it your own (Custom).";
            bool ac = nudge1("Turn##Round turn (degrees)", &turn, 5.0f, -180.0f, 180.0f, "%+.0f", kAngleTip);
            ac |= nudge1("Tilt##Round tilt (degrees)", &tilt, 5.0f, -90.0f, 90.0f, "%+.0f", kAngleTip);
            if (ac) {
                pl.axis_mode = 2;
                pl.axis[0] = std::cos(tilt / kDeg) * std::cos(turn / kDeg);
                pl.axis[1] = std::cos(tilt / kDeg) * std::sin(turn / kDeg);
                pl.axis[2] = std::sin(tilt / kDeg);
                ch = done = true;
            }
        }
        const bool bc = nudge1("Brightness##Round brightness", &pl.bright, 0.25f, 0.25f, 3.0f, "%.2f",
                               "The drawn rounds' brightness, in the hand and at the chest (a press 0.25).");
        ch |= bc;
        done |= bc;
        if (ch) round_draw::set_place(pl, false);
        if (done) round_draw::set_place(pl, true);
    }

    heading("Two-handed long guns");
    v = reload::two_handed();
    if (check("Two-handed long guns", &v,
              "The front hand on a long gun's foregrip (grip it ahead of the gun hand, near the barrel) aims it with both hands."))
        reload::set_two_handed(v);
    sub_begin(v);
    bool snap = holster::foregrip_snap();
    if (check("The front hand snaps onto the gun", &snap,
              "While held, the front hand snaps onto the gun in the game's own grip (where the game puts John's left hand, its "
              "fingers too) instead of following the controller."))
        holster::set_foregrip_snap(snap, true);
    sub_end();

    heading("Every gun's rings (m, a press 5 mm)");
    note("A gun's own (the Gun in hand tab) replace these. Offsets right / up / forward along the gun.");
    {
        float fo[3], fr = 0;
        holster::foregrip(fo, &fr);
        ImGui::TextUnformatted("The foregrip: where John's front hand sits on the gun");
        if (nudge3("Offset##Foregrip offset", fo, 0.005f, 0.3f, "Where John's front hand sits on the gun: the game's grip moved."))
            holster::set_foregrip(fo, fr, true);  // round 13 item 9: arrows
        // run 6 item 4: the ring where your front hand takes the gun, moved and sized apart from the grip
        float zo[3], zr = 0;
        holster::foregrip_zone(zo, &zr);
        ImGui::TextUnformatted("The foregrip ring: where your front hand takes hold");
        const char* const kZoneTip = "Where your front hand takes hold of the gun, moved from the grip, and the ring's size (the "
                                     "Holsters tab shows it); John's hand still sits at the grip.";
        bool zch = nudge3("Offset##Foregrip ring offset", zo, 0.005f, 0.3f, kZoneTip);
        zch |= nudge1("Size##Foregrip ring size", &zr, 0.005f, 0.03f, 0.4f, "%.3f", kZoneTip);
        if (zch) holster::set_foregrip_zone(zo, zr, true);
    }
    {  // run 6 item 3: the loading point
        float lo[3], lr = 0;
        bool touch = false;
        holster::load_point(lo, &lr, &touch);
        ImGui::TextUnformatted("The loading ring: where a round goes in");
        const char* const kLoadTip = "Where a round goes into the gun in hand: its loading point (0.10 m out along the barrel "
                                     "from the gun hand's wrist) moved, and the ring's size: a round held within it goes in.";
        bool ch = nudge3("Offset##Loading point offset", lo, 0.005f, 0.3f, kLoadTip);  // round 13 item 9: arrows
        ch |= nudge1("Size##Loading point size", &lr, 0.005f, 0.03f, 0.4f, "%.3f", kLoadTip);
        ch |= check("A round goes in at a touch", &touch,
                    "The round itself touching the ring puts it in, the grip still held (the round is drawn at your fingertips, "
                    "ahead of the wrist). Off: the wrist within the ring.");
        if (ch) holster::set_load_point(lo, lr, touch, true);
    }
}

void page_actions() {
    heading("Each gun's action by hand");
    bool ma = actions::enabled();
    if (check("Work each gun's action by hand", &ma,
              "A revolver opens by a flick of the right stick down and closes by a flick of the gun; after a shot a bolt, a lever or "
              "a pump is worked by your hands. Until then the trigger does not fire."))
        actions::set_enabled(ma);
    sub_begin(ma);
    bool oc = actions::open_cylinder();
    if (check("Revolvers drawn open", &oc,
              "The open revolver drawn open: the Schofield and the LeMat tip down, the Double-action's cylinder swings out, the "
              "Cattleman's gate opens (the game's reload sounds on your steps)."))
        actions::set_open_cylinder(oc);
    bool lp = actions::lever_parts();
    if (check("Lever guns: the lever by your flick", &lp,
              "The lever guns worked by your flick: down opens the lever, up closes it and chambers the round; the gun stays still "
              "through John's fire clip, the game's lever sounds on your flicks."))
        actions::set_lever_parts(lp);
    bool pp = actions::pump_parts();
    if (check("Pump-action: the fore-end in your front hand", &pp,
              "After a shot pull the fore-end back with your front hand (held two-handed) and push it forward; the gun's own "
              "pumping removed, the game's pump sounds on your strokes."))
        actions::set_pump_parts(pp);
    bool bo = actions::bolt_parts();
    if (check("Bolt actions: the bolt in your other hand", &bo,
              "The Bolt Action and the Carcano: grip the bolt with your other hand, lift the handle, pull back (rounds go in only "
              "now), push forward and turn it down."))
        actions::set_bolt_parts(bo);
    bool br = actions::breech_parts();
    if (check("Single-shot rifles opened by hand", &br,
              "The Springfield's trapdoor and the Rolling Block's block by your other hand, the Buffalo's by a flick down and up: "
              "open, one round in by hand, shut."))
        actions::set_breech_parts(br);
    bool sa = actions::semi_auto_parts();
    if (check("Semi-Auto Shotgun: the bolt racked by hand", &sa,
              "After loading it from empty, grip the bolt's handle (right of the receiver) with your other hand, pull it back and "
              "let go; between shots it cycles itself."))
        actions::set_semi_auto_parts(sa);
    bool mb = actions::manual_break();
    if (check("Break actions opened by the stick", &mb,
              "The Double-barrel and the Sawed-off broken open by a flick of the right stick down (the shells go in only then); "
              "shut by a flick of the gun up, or swing the barrels up with the other hand."))
        actions::set_manual_break(mb);
    sub_begin(mb);
    bool bs = actions::barrel_hand_snap();
    if (check("John's hand on the barrels as they swing up", &bs,
              "While your other hand holds the open barrels, John's hand is drawn on them, turning with them as they swing up."))
        actions::set_barrel_hand_snap(bs);
    sub_end();
    sub_begin(bo || br || sa);
    bool ps = actions::part_hand_snap();
    if (check("John's hand on the part your other hand grips", &ps,
              "With the bolts, the breeches or the semi-auto's bolt worked by hand: while your other hand grips the part, John's "
              "hand is drawn on it, moving with it."))
        actions::set_part_hand_snap(ps);
    sub_end();
    bool hc = actions::held_click();  // round 13 item 13
    if (check("A click when fired before the action is worked", &hc,
              "A pull of the trigger held back (the gun open, or its action not worked yet) plays the gun's empty click too."))
        actions::set_held_click(hc);
    sub_end();
}

void page_sounds() {
    heading("An empty gun");
    const int em = static_cast<int>(audio::empty_click_mode());
    const char* const names[4] = {"Clicks", "Buzzes", "Clicks and buzzes", "Does nothing"};
    const char* const kClickTip = "The trigger on an empty gun: a hammer's click from the gun hand's side (the mod's sound, on the "
                                  "Windows default audio device), the controller buzzes, both, or nothing.";
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (radio(names[i], em == i, kClickTip)) audio::set_empty_click_mode(static_cast<audio::ClickMode>(i));
    }
    sub_begin(em == 0 || em == 2);
    float vol = audio::click_volume();
    if (nudge1("Click volume", &vol, 0.05f, 0.0f, 1.0f, "%.2f",
               "The click's loudness, 0 - 1 (the game's volume settings do not reach it; the Windows mixer's slider for the game "
               "does). Each press plays it once at the new level.")) {
        audio::set_click_volume(vol, true);  // saved, and one click at the new level: the gun in hand's
        RdrvrActorState st{};
        audio::preview(api::actor_state(&st) ? st.weapon : -1);
    }
    sub_end();
}

void page_gestures() {
    heading("Loose objects");
    bool hc = physics::enabled();
    if (check("Hands push loose objects", &hc,
              "An empty hand moving through a bottle, a crate or a chair pushes it along the hand, through the game's own natives "
              "(never a person)."))
        physics::set_enabled(hc);
    bool gb = physics::grab_enabled();
    if (check("Grab loose objects", &gb,
              "An empty hand gripping at a loose prop (a bottle, a chair, a crate) holds it at the palm; letting go throws it with "
              "the hand's speed. People are never grabbed."))
        physics::set_grab_enabled(gb);

    heading("By hand");
    float sw = gestures::swing_speed(), tg = gestures::throw_gain();
    bool th = gestures::throw_by_hand();
    if (check("Throw by hand", &th,
              "A thrown weapon (dynamite, a fire bottle, a throwing knife, a tomahawk) leaves your gun hand with its speed and "
              "direction instead of the game's arc: pull the trigger to throw as usual, and swing."))
        gestures::set_throw_by_hand(th);
    sub_begin(th);
    if (nudge1("Throw strength", &tg, 0.1f, 0.5f, 4.0f, "%.1fx", "The throw's speed, times your hand's (a press 0.1)."))
        gestures::set_tuning(sw, tg, true);
    sub_end();
    bool me = gestures::melee_by_swing();
    if (check("Melee by swing", &me,
              "A swing of either hand faster than the swing speed attacks: a punch with the fists, a stab with the knife, a strike "
              "with the torch."))
        gestures::set_melee_by_swing(me);
    bool la = gestures::lasso_by_hand();
    if (check("Lasso by hand", &la,
              "With the lasso in hand, a forward swing of the gun hand faster than the swing speed throws it; a yank back steps "
              "John back on the rope to pull."))
        gestures::set_lasso_by_hand(la);
    sub_begin(me || la);
    if (nudge1("Swing speed", &sw, 0.25f, 1.0f, 8.0f, "%.2f m/s", "How fast a swing must be for a punch or the lasso (a press 0.25 m/s)."))
        gestures::set_tuning(sw, tg, true);
    sub_end();

    heading("The horse whistle");  // run 8 item 4: [Gestures] Whistle
    bool wh = whistle::enabled();
    if (check("Whistle for the horse", &wh,
              "A hand at your mouth and its trigger: John whistles for his horse. The trigger of that hand does nothing else while it is "
              "at the mouth. The gun hand whistles only with no gun in it."))
        whistle::set_enabled(wh, true);

    heading("The gun-butt melee");  // run 7 item 2: [Gestures] GunMelee, GunMeleeDryRun
    bool gm = gun_melee::enabled();
    if (check("Gun-butt melee", &gm,
              "Hit someone by swinging the gun in your hand (a long gun's butt or barrel, a pistol's frame or barrel): the game's own "
              "melee hit on who the swing passes through, a knock-out blow by default, with its crime and anger."))
        gun_melee::set_enabled(gm, true);
    sub_begin(gm);
    bool dr = gun_melee::dry_run();
    if (check("Dry run", &dr, "The hit found and logged (\"[gunmelee] dry hit ...\"), never made.")) gun_melee::set_dry(dr, true);
    float hs = gun_melee::config().speed;
    if (nudge1("Hit speed", &hs, 0.25f, 1.0f, 6.0f, "%.2f m/s",
               "How fast the swing's peak must be for a hit (the gun's butt or barrel; a press 0.25 m/s). The log's \"swing over\" "
               "line gives each missed swing's peak."))
        gun_melee::set_speed(hs, true);
    sub_end();
}

void page_controls() {
    heading("Left-handed");
    bool lh = controls::left_handed();
    if (check("Left-handed", &lh, "The two controllers' roles swapped: the left trigger fires, the right stick moves."))
        controls::set_left_handed(lh);
    sub_begin(lh);
    bool gih = body::gun_in_gun_hand();
    if (check("The gun in John's left hand", &gih,
              "Each of John's arms follows its own controller and the gun is drawn in his left hand. Off: John's right arm on the "
              "left controller (the arms cross)."))
        body::set_gun_in_gun_hand(gih);
    sub_end();

    heading("The trigger and the aim");
    bool ta = controls::trigger_aims();
    if (check("The trigger alone fires", &ta,
              "With a gun in hand in first person the right trigger alone fires: the game shoots only from its aim pose, so the "
              "left trigger is held for you from the pull."))
        controls::set_trigger_aims(ta);
    bool ar = controls::aim_when_raised();
    if (check("Aim while the gun is raised", &ar,
              "The left trigger held for you while the gun hand is raised (the barrel near level, the hand ahead of the chest): the "
              "gun stays up in the aim stance and every pull fires at once."))
        controls::set_aim_when_raised(ar);
    bool sd = controls::sprint_drops_aim();
    if (check("Sprinting lowers the raised gun", &sd,
              "On foot, the sprint button lets the raised gun's left trigger go, so John can sprint with a gun in hand; a pull "
              "still aims and fires."))
        controls::set_sprint_drops_aim(sd);
    bool jd = controls::jump_drops_aim();  // run 7 item 1f: [Hands] JumpDropsAim
    if (check("Jumping lowers the raised gun", &jd,
              "With a gun in hand the game takes no jump, vault or climb from its aim stance: the jump's button lets the raised "
              "gun's left trigger go first, the press held until the aim is down (at most 250 ms)."))
        controls::set_jump_drops_aim(jd, true);
    if (aim::spawn_hooked()) {
        bool pa = aim::perfect_accuracy();
        if (check("Perfect accuracy", &pa,
                  "Every shot leaves exactly along the barrel: the game's random spread skipped, a muzzle it thinks is in a wall no "
                  "longer bends the shot, John's own speed not added. On by default; off: the game's own spread."))
            aim::set_perfect_accuracy(pa);
        bool sp = aim::shotgun_pattern();
        if (check("Shotgun pellets spread", &sp, "The pellets in the game's cone around the shot's line. Off: every pellet on that one line."))
            aim::set_shotgun_pattern(sp);
    } else {
        note("Perfect accuracy: unavailable (the spawn hook is not installed)");
    }

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
        help("The game button this VR button drives (a trigger mapped to a button presses it past half way; a button mapped to "
             "LT / RT pulls it fully).");
    }
    if (reset_button("Reset to the game's scheme", true, "Every VR button back on the game button of its own name."))
        controls::reset_mapping();
    note("The menu: hold the menu button (without one: both B buttons), hold Y with the wrist HUD in view, or F7. A tap of the menu "
         "button pauses; held with the right stick it is the D-pad, with A the map. The right stick scrolls the menu's page.");
}

void page_screen() {
    heading("Cutscenes");
    int m = g_set.cut3d ? 1 : 0;
    if (radio("On the cinema screen", m == 0, "Cutscenes on the cinema screen: the game's own camera at 16:9.")) m = 0;
    ImGui::SameLine();
    if (radio("In 3D", m == 1, "Cutscenes in 3D: both eyes from the cutscene camera, your head's rotation only.")) m = 1;
    if ((m == 1) != g_set.cut3d) {
        g_set.cut3d = m == 1;
        vr_mode::set_cutscene_3d(g_set.cut3d);
        config::set("Screen", "CutsceneMode", g_set.cut3d ? "3D" : "Screen");
    }
    sub_begin(g_set.cut3d);
    if (nudge1("3D separation", &g_set.separation, 0.05f, 0.0f, 1.0f, "%.2f",
               "3D cutscenes: the eye separation as a fraction of yours (1.00 = your IPD; a press 0.05).")) {
        vr_mode::set_separation(g_set.separation);
        save_float("Screen", "Cutscene3DSeparation", "%.2f", g_set.separation);
    }
    sub_end();
}

void page_debug() {
    char line[400];
    xr::perf_status(line, sizeof(line));
    ImGui::TextWrapped("%s", line);
    vr_mode::status_text(line, sizeof(line));
    ImGui::TextWrapped("%s", line);

    heading("Test aids");
    if (check("Automatic view modes", &g_set.auto_mode,
              "View modes chosen by the game's state: stereo in gameplay, the cinema screen for loading screens, menus and "
              "cutscenes. Off: the test commands set the view.")) {
        vr_mode::set_auto(g_set.auto_mode);
        config::set("XR", "AutoMode", g_set.auto_mode ? "1" : "0");
    }

    heading("Rendering fixes (off: for a comparison)");
    if (check("HUD on its own quad", &g_set.ui_quad,
              "The UI kept out of the eye images and shown on its own quad (or the wrist). Off: the UI stays in the game's image, "
              "and is then missing from the eye images.")) {
        ui_layer::set_redirect(g_set.ui_quad && xr::submitting());
        config::set("XR", "UiQuad", g_set.ui_quad ? "1" : "0");
    }
    bool imp = camera_lever::level_imposters();
    if (check("Distant trees: no spinning with the head", &imp,
              "Distant trees' pictures captured along the game camera's heading instead of your eye's view, so they do not spin "
              "with your head.")) {
        camera_lever::set_level_imposters(imp);
        config::set("XR", "LevelImposters", imp ? "1" : "0");
    }
    {
        const int hg = body::hidden_geometry();  // [Body] HiddenGeometry: how the hidden parts are drawn (the sliver)
        const char* const kHgTip = "The hidden parts in the forearms and hands modes: filtered (the default) leaves out the "
                                   "triangles across two cuts (no line between the elbows), skipped only the wholly hidden, "
                                   "collapsed draws them all.";
        ImGui::TextUnformatted("Hidden parts:");
        ImGui::SameLine();
        if (radio("Collapsed", hg == 0, kHgTip)) body::set_hidden_geometry(0);
        ImGui::SameLine();
        if (radio("Skipped", hg == 1, kHgTip)) body::set_hidden_geometry(1);
        ImGui::SameLine();
        if (radio("Filtered", hg == 2, kHgTip)) body::set_hidden_geometry(2);
    }

    heading("Stereo: the passes split per eye");
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
        help("Each of the game's passes drawn per eye (on) or once for both (off; a test of the stereo).");
        if (i % 3 != 2) ImGui::SameLine(static_cast<float>((i % 3 + 1) * kW / 3));
    }
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
        case kGeneral: page_general(); break;
        case kComfort: page_comfort(); break;
        case kHands: page_hands(); break;
        case kHolsters: page_holsters(); break;
        case kGunInHand: page_gun(); break;
        case kWeapons: page_weapons(); break;
        case kReloading: page_reloading(); break;
        case kActions: page_actions(); break;
        case kSounds: page_sounds(); break;
        case kGestures: page_gestures(); break;
        case kControls: page_controls(); break;
        case kScreen: page_screen(); break;
        case kDebug: page_debug(); break;
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

}  // namespace

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
    std::snprintf(out, len, "menu %s, %s, frames %llu, clicks %llu, scrolled by the stick %llu px, page %s scroll %.0f of %.0f",
                  g_visible.load() ? "open" : "closed", g_ready ? "ready" : "not made", static_cast<unsigned long long>(g_frames.load()),
                  static_cast<unsigned long long>(g_clicks.load()), static_cast<unsigned long long>(g_stick_scrolled.load()),
                  p >= 0 && p < kPages ? kPageName[p] : "?", g_scroll_now.load(std::memory_order_relaxed),
                  g_scroll_max.load(std::memory_order_relaxed));
}

}  // namespace rdrvr::menu
