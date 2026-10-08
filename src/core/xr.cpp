#include "core/xr.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <algorithm>
#include <vector>

#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/eye_shape.h"
#include "core/hands.h"
#include "core/anchors.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/post_target.h"
#include "core/render_res.h"
#include "core/state.h"
#include "core/xr_blit.h"
#include "core/ui_layer.h"
#include "core/menu.h"
#include "core/xinput.h"
#include "core/zone_rings.h"

namespace rdrvr::xr {
namespace {

std::atomic<bool> g_started{false};
std::atomic<uint64_t> g_frames{0};
char g_state[32] = "none";
char g_runtime[128] = "";

std::mutex g_views_mutex;
EyeView g_views[2]{};
bool g_views_valid = false;
uint64_t g_views_frame = 0;
// R5's pose check: the views handed to the scene since the last frame end (under g_views_mutex), against the views
// that frame's projection layer carries.
EyeView g_handed[2]{};
uint64_t g_handed_frame = 0;
int g_handed_n = 0;
bool g_handed_other = false;  // a view handed out this frame came from another XR frame
std::atomic<uint64_t> g_pose_match{0}, g_pose_stale{0}, g_pose_none{0};
std::atomic<uint64_t> g_latched{0}, g_latch_misses{0};
std::atomic<float> g_latch_max_deg{0}, g_latch_sum_deg{0};
std::atomic<bool> g_relayout{false};
// R6 measurement window ("perf reset" starts one): XR frames, missed display periods (a predicted display time more
// than 1.5 periods after the previous one), the longest gap, and the mean CPU and GPU frame times over the window.
struct Perf {
    uint64_t frames = 0, missed = 0, late_frames = 0;
    double longest = 0, cpu_sum = 0, gpu_sum = 0, period_ms = 0;
    double start_ms = 0;
    XrTime prev = 0;
    uint64_t scene_us0 = 0, scene_n0 = 0;  // camera_lever::scene_time at the window's start
};
// Its own lock, not g_frame_mutex: the menu's Debug tab reads it while the presenting thread holds the frame mutex
// (headset round 4: locking g_frame_mutex there again threw std::system_error, a deadlock, and ended the game).
std::mutex g_perf_mutex;
Perf g_perf;  // presenting thread (g_perf_mutex)

// The log's "[xr] timing" window ([XR] TimingLog, seconds; 0 off): the presenting thread's, every frame submitted
struct TimingWin {
    double start_ms = 0;
    uint64_t frames = 0, missed = 0, late = 0;
    double longest = 0, cpu_sum = 0, gpu_sum = 0, wait_sum = 0, wait_max = 0, open_sum = 0, open_max = 0, period_ms = 0;
    float open[4096];
    uint32_t nopen = 0;
    uint64_t pose_m0 = 0, pose_s0 = 0, pose_n0 = 0, latch0 = 0, copies0 = 0, misses0 = 0, late_binds0 = 0;
    float latch_sum0 = 0;
};
TimingWin g_tw;
double g_open_ms = 0;  // when the open XR frame began (xrBeginFrame returned)
double g_wait_ms = 0;  // how long its xrWaitFrame blocked
std::atomic<bool> g_perf_reset{false};

void to_eye_views(const XrView* v, EyeView* e);

void to_eye_views(const XrView* v, EyeView* e) {
    for (int i = 0; i < 2; ++i) {
        e[i].fov[0] = v[i].fov.angleLeft;
        e[i].fov[1] = v[i].fov.angleRight;
        e[i].fov[2] = v[i].fov.angleUp;
        e[i].fov[3] = v[i].fov.angleDown;
        e[i].orientation[0] = v[i].pose.orientation.x;
        e[i].orientation[1] = v[i].pose.orientation.y;
        e[i].orientation[2] = v[i].pose.orientation.z;
        e[i].orientation[3] = v[i].pose.orientation.w;
        e[i].position[0] = v[i].pose.position.x;
        e[i].position[1] = v[i].pose.position.y;
        e[i].position[2] = v[i].pose.position.z;
    }
}

std::atomic<float> g_ramp_step[3];
std::atomic<bool> g_ramp_on{false};
float g_ramp_off[3] = {0, 0, 0};  // under g_frame_mutex

std::atomic<float> g_noise_rot{0.0f}, g_noise_pos{0.0f};  // "xr posnoise": degrees, metres
uint32_t g_noise_state = 0x9e3779b9u;                        // under g_frame_mutex (xorshift32, a fixed seed)

float noise_unit() {  // uniform in [-1, 1)
    uint32_t x = g_noise_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_noise_state = x;
    return static_cast<float>(x) / 2147483648.0f - 1.0f;
}

XrQuaternionf quat_mul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrQuaternionf g_noise_q{0.0f, 0.0f, 0.0f, 1.0f};  // this frame's noise turn (under g_frame_mutex): the late latch keeps it

std::atomic<float> g_rotramp_step{0.0f};  // "xr rotramp": degrees of yaw added every XR frame
float g_rotramp_acc = 0.0f;               // the turn so far, radians (under g_frame_mutex)

// "xr rotramp": this frame's views turned by the ramp's yaw about the eyes' midpoint (a head turning at a constant
// rate, the eyes moving with it); the turn joins g_noise_q, which the late latch applies to its views too
void apply_rot_ramp(XrView* v) {
    const float step = g_rotramp_step.load(std::memory_order_relaxed);
    if (step == 0.0f) {
        g_rotramp_acc = 0.0f;
        return;
    }
    g_rotramp_acc += step * 0.0174532925f;
    const float a = g_rotramp_acc;
    const XrQuaternionf q{0.0f, std::sin(0.5f * a), 0.0f, std::cos(0.5f * a)};
    const float cx = 0.5f * (v[0].pose.position.x + v[1].pose.position.x), cz = 0.5f * (v[0].pose.position.z + v[1].pose.position.z);
    const float c = std::cos(a), s = std::sin(a);
    for (int i = 0; i < 2; ++i) {
        v[i].pose.orientation = quat_mul(q, v[i].pose.orientation);
        const float dx = v[i].pose.position.x - cx, dz = v[i].pose.position.z - cz;  // yaw about +Y: x' = c x + s z, z' = -s x + c z
        v[i].pose.position.x = cx + c * dx + s * dz;
        v[i].pose.position.z = cz - s * dx + c * dz;
    }
    g_noise_q = quat_mul(q, g_noise_q);  // the order the views got: the turn after the noise
}

// "xr posnoise": this frame's views turned and moved by fresh noise (the same for both eyes)
void apply_pose_noise(XrView* v) {
    const float rot = g_noise_rot.load(std::memory_order_relaxed), pos = g_noise_pos.load(std::memory_order_relaxed);
    if (rot == 0.0f && pos == 0.0f) {
        g_noise_q = {0.0f, 0.0f, 0.0f, 1.0f};
        apply_rot_ramp(v);
        return;
    }
    const float yaw = rot * noise_unit() * 0.0174532925f, pitch = rot * noise_unit() * 0.0174532925f;
    const XrQuaternionf qy{0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)}, qp{std::sin(0.5f * pitch), 0.0f, 0.0f, std::cos(0.5f * pitch)};
    const XrQuaternionf q = quat_mul(qy, qp);
    g_noise_q = q;
    const float d[3] = {pos * noise_unit(), pos * noise_unit(), pos * noise_unit()};
    for (int i = 0; i < 2; ++i) {
        v[i].pose.orientation = quat_mul(q, v[i].pose.orientation);
        v[i].pose.position.x += d[0];
        v[i].pose.position.y += d[1];
        v[i].pose.position.z += d[2];
    }
    apply_rot_ramp(v);
}

// "xr posramp": this frame's views moved by the ramp's offset (submit_frame_end, after the views are located)
void apply_pos_ramp(XrView* v) {
    apply_pose_noise(v);
    if (!g_ramp_on.load(std::memory_order_relaxed)) {
        g_ramp_off[0] = g_ramp_off[1] = g_ramp_off[2] = 0;
        return;
    }
    for (int k = 0; k < 3; ++k) g_ramp_off[k] += g_ramp_step[k].load(std::memory_order_relaxed);
    for (int i = 0; i < 2; ++i) {
        v[i].pose.position.x += g_ramp_off[0];
        v[i].pose.position.y += g_ramp_off[1];
        v[i].pose.position.z += g_ramp_off[2];
    }
}

void store_views(const XrView* v, uint64_t frame) {
    EyeView e[2];
    to_eye_views(v, e);
    bool changed;
    {
        std::lock_guard lock(g_views_mutex);
        changed = !g_views_valid;
        for (int i = 0; i < 2 && !changed; ++i)
            for (int k = 0; k < 4; ++k)
                if (std::fabs(g_views[i].fov[k] - e[i].fov[k]) > 1e-4f) changed = true;
        g_views[0] = e[0];
        g_views[1] = e[1];
        g_views_valid = true;
        g_views_frame = frame;
    }
    if (changed) {
        for (int i = 0; i < 2; ++i)
            log::info("[xr] %s eye fov (rad) L %.5f R %.5f U %.5f D %.5f | pos (%.4f %.4f %.4f) rot (%.4f %.4f %.4f %.4f)",
                      i ? "right" : "left", e[i].fov[0], e[i].fov[1], e[i].fov[2], e[i].fov[3], e[i].position[0],
                      e[i].position[1], e[i].position[2], e[i].orientation[0], e[i].orientation[1], e[i].orientation[2],
                      e[i].orientation[3]);
    }
}

const char* result_name(XrInstance inst, XrResult r) {
    static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];
    if (inst != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(inst, r, buf))) return buf;
    switch (r) {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        default: std::snprintf(buf, sizeof(buf), "XrResult(%d)", static_cast<int>(r)); return buf;
    }
}

const char* state_name(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "idle";
        case XR_SESSION_STATE_READY: return "ready";
        case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
        case XR_SESSION_STATE_VISIBLE: return "visible";
        case XR_SESSION_STATE_FOCUSED: return "focused";
        case XR_SESSION_STATE_STOPPING: return "stopping";
        case XR_SESSION_STATE_LOSS_PENDING: return "loss_pending";
        case XR_SESSION_STATE_EXITING: return "exiting";
        default: return "unknown";
    }
}

// ---- the runtime's largest image and the swapchains' sizes. [Render] RenderResolution (render_res.h) can make the
// game's frame larger than a runtime takes (the OpenXR Simulator: maxImageRect 4096x4096), so every swapchain is sized
// by fit_image within one cap per axis: min of XrSystemGraphicsProperties' maxSwapchainImageWidth/Height and the larger
// view's maxImageRectWidth/Height (0: not given, no cap).
std::atomic<uint32_t> g_cap_w{0}, g_cap_h{0};

// The size of an XR swapchain for a w x h source: w x h itself when it fits within max_w x max_h (0: no limit on that
// axis), else scaled down to fit with its aspect kept, each side even and at least 16.
void fit_image(uint32_t w, uint32_t h, uint32_t max_w, uint32_t max_h, uint32_t* ow, uint32_t* oh) {
    const bool over_w = max_w && w > max_w, over_h = max_h && h > max_h;
    if (!w || !h || (!over_w && !over_h)) {
        *ow = w;
        *oh = h;
        return;
    }
    // the axis that limits it: the smaller of max/size (integer, so a side at its max is exactly that)
    const bool by_w = over_w && (!over_h || static_cast<uint64_t>(max_w) * h <= static_cast<uint64_t>(max_h) * w);
    uint32_t a = by_w ? max_w : static_cast<uint32_t>(static_cast<uint64_t>(w) * max_h / h);
    uint32_t b = by_w ? static_cast<uint32_t>(static_cast<uint64_t>(h) * max_w / w) : max_h;
    a &= ~1u;
    b &= ~1u;
    *ow = a < 16 ? 16 : a;
    *oh = b < 16 ? 16 : b;
}

// A quad's largest width: its [XR] key (pixels; 0 or less: the runtime's largest image only), within the cap.
uint32_t quad_max_w(const char* key) {
    const int v = config::get_int("XR", key, 2560);
    const uint32_t cap = g_cap_w.load(), m = v > 0 ? static_cast<uint32_t>(v < 256 ? 256 : v) : 0u;
    return !m ? cap : !cap ? m : m < cap ? m : cap;
}

// ---- the live-resize guard (xr.h frame_resized): the game's frame made again at another size while the session runs
// (its Graphics menu: research\run7\eye-shape.md, the resize row). The eye path stops at the first sign and the flat
// game goes on the cinema screen until the game is restarted.
std::atomic<bool> g_resize_claim{false}, g_resized{false};
char g_resized_what[96] = "";  // written once, before g_resized (release)
std::atomic<uint32_t> g_sess_w{0}, g_sess_h{0}, g_sess_bw{0}, g_sess_bh{0};  // the frame the eyes were made for, its back buffer
void stop_eyes(const char* what, uint32_t w, uint32_t h);

// ---- R5 step 1: submission (see xr.h)
struct EyeChain {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> images;
};
EyeChain g_chain[2];
std::atomic<bool> g_submit_mode{false};  // frames on the presenting thread ([XR] Submit at start, or "xr submit on")
std::atomic<int> g_submit_request{0};    // 1 on, -1 off: taken by the session thread between its idle frames

// The session thread owns the windows a runtime makes (the simulator's preview), so it pumps their messages: a
// runtime call on another thread that sends one of them a message waits for this thread (cycle 18 hung without it).
void pump_messages(DWORD wait_ms) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, wait_ms, QS_ALLINPUT);
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
std::atomic<bool> g_submit{false};       // swapchains made: the frame loop runs on the presenting thread
std::atomic<bool> g_session_running{false};
std::mutex g_frame_mutex;                // the presenting thread's frame calls against the session thread's end
XrInstance g_inst = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
ID3D12CommandQueue* g_session_queue = nullptr;  // the binding's queue (a reference kept, as state's)
ID3D12Fence* g_to_session = nullptr;            // the present queue's work ordered before the session queue's
ID3D12Fence* g_from_session = nullptr;          // the session queue's work ordered before the game's next lists
uint64_t g_to_value = 0, g_from_value = 0;
std::mutex g_queue_sync_mutex;
std::atomic<uint64_t> g_queue_syncs{0};
XrSpace g_space = XR_NULL_HANDLE;
XrEnvironmentBlendMode g_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
bool g_frame_open = false;               // presenting thread: begun, views located, images acquired
bool g_should_render = false;
XrTime g_display_time = 0;
// Headset round 2: the headset's own recentre (the Quest's: hold the Oculus button) moves the LOCAL space at
// changeTime; the mod takes its origin and the body heading again from the first frame displayed at or after it.
std::atomic<XrTime> g_space_change_at{0};
std::atomic<uint64_t> g_recentres{0};
bool g_scroll_down = false;  // XR thread
XrView g_frame_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
uint32_t g_w = 0, g_h = 0;
uint32_t g_sw = 0, g_sh = 0;  // the eye swapchains: g_w x g_h, or the eye size with [XR] EyeShape (eye_shape.h)
std::atomic<uint32_t> g_rect_w{0}, g_rect_h{0};  // the last layer's imageRect
// The post chain's output the eye images come from: [0] the Post FXAA Target (FXAA), [1] FXAATarget (every other
// technique, native TAA among them; post_target.h).
std::atomic<ID3D12Resource*> g_target[2]{};
uint64_t g_target_rtv[2][8];
int g_target_nrtv[2] = {};
DXGI_FORMAT g_target_fmt = DXGI_FORMAT_UNKNOWN;  // the post output's (both targets'), checked before each copy
bool g_blit = false;                     // eye images through the colour blit (sRGB swapchains), else raw copies
bool g_raw_refused = false;              // raw copies of a frame larger than the runtime's largest image: no submission
bool g_eyes_acquired = false;            // presenting thread: this frame took the eye images (not after a live resize)
std::atomic<ID3D12Resource*> g_dst[2];   // this frame's acquired images, for the recording thread
std::atomic<int> g_filled{0};            // bit per eye filled this frame
int g_fxaa_binds = 0;                    // recording thread
std::atomic<uint64_t> g_submitted{0}, g_copies{0}, g_misses{0}, g_empty{0}, g_late_eye_binds{0};

// ---- cinema (R5 step 3)
std::atomic<bool> g_cinema{false};
bool g_cinema_placed = false;            // presenting thread
XrPosef g_cinema_pose{};
XrSwapchain g_quad = XR_NULL_HANDLE;
std::vector<ID3D12Resource*> g_quad_images;
uint32_t g_quad_w = 0, g_quad_h = 0;     // the quad swapchain: the back buffer's size, at most [XR] CinemaMaxWidth wide
uint32_t g_quad_bw = 0, g_quad_bh = 0;   // the back buffer it was made for, and its format: a CopyResource needs both
DXGI_FORMAT g_quad_bfmt = DXGI_FORMAT_UNKNOWN, g_quad_fmt = DXGI_FORMAT_UNKNOWN;
std::atomic<uint32_t> g_quad_rw{0}, g_quad_rh{0};  // the last frame's picture in it (the layer's imageRect)
ID3D12CommandAllocator* g_quad_alloc = nullptr;
ID3D12GraphicsCommandList* g_quad_list = nullptr;
// the copy path's fence: its one allocator is reset only once the GPU has run the last copy (the review, 2026-10-08)
ID3D12Fence* g_quad_fence = nullptr;
HANDLE g_quad_event = nullptr;
uint64_t g_quad_value = 0;  // the last copy's signal (presenting thread)
std::atomic<uint64_t> g_cinema_frames{0}, g_cinema_copies{0}, g_cinema_resamples{0};

// The quad image at the back buffer's size within [XR] CinemaMaxWidth (default 2560: the screen is 3.2 m wide at 3 m,
// about 56 degrees, where a 3840-wide image is minified without mips) and the runtime's largest image.
bool make_quad_swapchain(ID3D12Resource* bb) {
    if (g_quad != XR_NULL_HANDLE) return g_quad_list != nullptr;  // made once (without its list: never used, not made again)
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    DXGI_FORMAT want = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                       : d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                : d.Format;
    uint32_t qw = 0, qh = 0;
    fit_image(static_cast<uint32_t>(d.Width), d.Height, quad_max_w("CinemaMaxWidth"), g_cap_h.load(), &qw, &qh);
    uint32_t nf = 0;
    int64_t formats[64];
    xrEnumerateSwapchainFormats(g_session, 64, &nf, formats);
    bool have = false;
    for (uint32_t i = 0; i < nf; ++i) have = have || formats[i] == static_cast<int64_t>(want);
    if (!have) {
        log::error("[xr] cinema: the runtime offers no swapchain format %d", static_cast<int>(want));
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = static_cast<int64_t>(want);
    ci.sampleCount = 1;
    ci.width = qw;
    ci.height = qh;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g_session, &ci, &g_quad);
    if (XR_FAILED(r)) {
        log::error("[xr] cinema: xrCreateSwapchain -> %s", result_name(g_inst, r));
        return false;
    }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages(g_quad, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_quad, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    for (auto& im : imgs) g_quad_images.push_back(im.texture);
    ID3D12Device* dev = state::device.load();
    if (!dev || FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_quad_alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_quad_alloc, nullptr, IID_PPV_ARGS(&g_quad_list))))
        return false;
    g_quad_list->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_quad_fence))) ||
        !(g_quad_event = CreateEventW(nullptr, FALSE, FALSE, nullptr)))
        return false;
    g_quad_w = qw;
    g_quad_h = qh;
    g_quad_bw = static_cast<uint32_t>(d.Width);
    g_quad_bh = d.Height;
    g_quad_bfmt = d.Format;
    g_quad_fmt = want;
    if (qw == g_quad_bw && qh == g_quad_bh)
        log::info("[xr] cinema: quad swapchain %ux%u format %d, %zu images", g_quad_w, g_quad_h, static_cast<int>(want), g_quad_images.size());
    else
        log::info("[xr] cinema: quad swapchain %ux%u format %d, %zu images (the back buffer %ux%u resampled into it: [XR] CinemaMaxWidth %u, the "
                  "runtime's largest image %ux%u)",
                  g_quad_w, g_quad_h, static_cast<int>(want), g_quad_images.size(), g_quad_bw, g_quad_bh, quad_max_w("CinemaMaxWidth"),
                  g_cap_w.load(), g_cap_h.load());
    return true;
}

// Presenting thread, frame end: the back buffer about to be presented (in PRESENT) into an acquired quad image (in
// RENDER_TARGET), with our own list on the present queue, after the game's work for the frame. A CopyResource only when
// the back buffer is the quad image's size and the one it was made for (format included); else (a frame larger than
// the caps, or the game's frame resized while the cinema runs: a log showed 37 s of a 2560x1440 back buffer copied
// into a 3840x2160 quad image) the filtered resample into the image's top-left, the back buffer's aspect kept.
bool fill_quad() {
    IDXGISwapChain* sc = state::swapchain.load();
    ID3D12CommandQueue* q = image_queue();
    if (!sc || !q) return false;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return false;
    ID3D12Resource* bb = nullptr;
    HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) return false;
    bool ok = false;
    if (make_quad_swapchain(bb)) {
        const D3D12_RESOURCE_DESC bd = bb->GetDesc();
        const bool copy = bd.Width == g_quad_w && bd.Height == g_quad_h && bd.Width == g_quad_bw && bd.Height == g_quad_bh &&
                          bd.Format == g_quad_bfmt;
        uint32_t rw = g_quad_w, rh = g_quad_h;
        if (!copy) fit_image(static_cast<uint32_t>(bd.Width), bd.Height, g_quad_w, g_quad_h, &rw, &rh);
        static int s_path = 0;  // presenting thread: the last frame's (1 the copy, 2 the resample), logged as it changes
        static uint32_t s_rw = 0, s_rh = 0;
        if ((copy ? 1 : 2) != s_path || rw != s_rw || rh != s_rh) {
            if (s_path || !copy)
                log::limited("xr.cinema.path", 16, "[xr] cinema: the back buffer %llux%u format %d %s the quad image %ux%u (made for %ux%u format %d), "
                             "%ux%u shown",
                             static_cast<unsigned long long>(bd.Width), bd.Height, static_cast<int>(bd.Format), copy ? "copied into" : "resampled into",
                             g_quad_w, g_quad_h, g_quad_bw, g_quad_bh, static_cast<int>(g_quad_bfmt), rw, rh);
            s_path = copy ? 1 : 2;
            s_rw = rw;
            s_rh = rh;
        }
        if (copy && g_quad_fence->GetCompletedValue() < g_quad_value) {  // the last copy still on the GPU: wait (50 ms at most)
            if (SUCCEEDED(g_quad_fence->SetEventOnCompletion(g_quad_value, g_quad_event))) WaitForSingleObject(g_quad_event, 50);
            if (g_quad_fence->GetCompletedValue() < g_quad_value) {
                log::limited("xr.cinema.busy", 4, "[xr] cinema: the last copy not done after 50 ms: this frame's not made");
                bb->Release();
                return false;
            }
        }
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wsi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wsi.timeout = 100000000;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_quad, &ai, &index)) && XR_SUCCEEDED(xrWaitSwapchainImage(g_quad, &wsi)) &&
            index < g_quad_images.size() && (!copy || (SUCCEEDED(g_quad_alloc->Reset()) && SUCCEEDED(g_quad_list->Reset(g_quad_alloc, nullptr))))) {
            ID3D12Resource* dst = g_quad_images[index];
            if (copy) {
                D3D12_RESOURCE_BARRIER b[2]{};
                for (auto& x : b) {
                    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                }
                b[0].Transition.pResource = bb;
                b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b[1].Transition.pResource = dst;
                b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                g_quad_list->ResourceBarrier(2, b);
                g_quad_list->CopyResource(dst, bb);
                for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
                g_quad_list->ResourceBarrier(2, b);
                g_quad_list->Close();
                d3d::submit_internal(q, g_quad_list);
                q->Signal(g_quad_fence, ++g_quad_value);
                ok = true;
            } else {
                ok = xr_blit::cinema_resample(state::device.load(), q, bb, dst, g_quad_fmt, rw, rh);
            }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(g_quad, &ri);
        }
        if (ok) {
            g_quad_rw.store(rw, std::memory_order_relaxed);
            g_quad_rh.store(rh, std::memory_order_relaxed);
            (copy ? g_cinema_copies : g_cinema_resamples).fetch_add(1, std::memory_order_relaxed);
        }
    }
    bb->Release();
    return ok;
}

// The screen's pose: 3 m ahead of the head (yaw only), at its height.
void place_cinema() {
    float mx = 0.5f * (g_frame_views[0].pose.position.x + g_frame_views[1].pose.position.x);
    float my = 0.5f * (g_frame_views[0].pose.position.y + g_frame_views[1].pose.position.y);
    float mz = 0.5f * (g_frame_views[0].pose.position.z + g_frame_views[1].pose.position.z);
    const XrQuaternionf& q = g_frame_views[0].pose.orientation;
    float bx = 2 * (q.x * q.z + q.y * q.w), bz = 1 - 2 * (q.x * q.x + q.y * q.y);  // the head's back axis
    float yaw = std::atan2(bx, bz);
    g_cinema_pose.orientation = {0, std::sin(0.5f * yaw), 0, std::cos(0.5f * yaw)};
    g_cinema_pose.position = {mx - 3.0f * std::sin(yaw), my, mz - 3.0f * std::cos(yaw)};
    g_cinema_placed = true;
    log::info("[xr] cinema screen 3 m ahead: (%.2f %.2f %.2f), yaw %.1f deg", g_cinema_pose.position.x, g_cinema_pose.position.y,
              g_cinema_pose.position.z, yaw * 57.29578f);
}

int active_target() {
    char* p = *reinterpret_cast<char**>(anchors::addr(anchors::Id::PostFxSingleton));
    return p && !post_target::fxaa(p) ? 1 : 0;
}

// Copies the post output into `dst_stage` (blit mode: a staging texture; -1: the eye's acquired swapchain image).
// `eye` >= 0 marks that eye filled.
void copy_eye_to(ID3D12GraphicsCommandList* cl, int eye, int dst_stage);
void copy_eye(ID3D12GraphicsCommandList* cl, int eye) { copy_eye_to(cl, eye, g_blit ? eye : -1); }

void copy_eye_to(ID3D12GraphicsCommandList* cl, int eye, int dst_stage) {
    ID3D12Resource* acquired = g_dst[eye < 0 ? 0 : eye].load();
    ID3D12Resource* dst = dst_stage >= 0 ? (acquired ? xr_blit::stage(dst_stage) : nullptr) : acquired;
    ID3D12Resource* src = g_target[active_target()].load();
    ID3D12GraphicsCommandList* wcl = nullptr;
    uint32_t st = 0;
    if (!dst || !src || !d3d::watched_state(src, &wcl, &st) || wcl != cl) {
        g_misses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The target's own size and format before each copy (the game just recorded its barrier in this list, so it is
    // live): never a copy between sizes, also when the game made its frame again with no sign the guard saw first (a
    // new target at the old one's address, its views at the old handles).
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (sd.Width != g_w || sd.Height != g_h || sd.Format != g_target_fmt || g_resized.load(std::memory_order_acquire)) {
        g_misses.fetch_add(1, std::memory_order_relaxed);
        if (!g_resized.load(std::memory_order_relaxed)) stop_eyes("the post output at an eye copy", static_cast<uint32_t>(sd.Width), sd.Height);
        return;
    }
    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& x : b) {
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    b[0].Transition.pResource = src;
    b[0].Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(st);
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = dst;  // swapchain images are in RENDER_TARGET when acquired; the stages are SRVs
    b[1].Transition.StateBefore = dst_stage >= 0 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    bool src_transition = st != D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(src_transition ? 2 : 1, src_transition ? b : b + 1);
    if (dst_stage < 0 && (g_sw != g_w || g_sh != g_h)) {
        // [XR] EyeShape, raw copies: the eye rect (the top-left) into the eye-shaped image; without the shape applied the
        // frame's top-left only (a raw copy cannot resample)
        uint32_t cw = g_w, ch = g_h;
        if (!eye_shape::frame_rect(&cw, &ch)) log::limited("xr.rawshape", 1, "[xr] raw copies into eye-shaped swapchains: the frame cropped while the shape is not applied");
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = dst;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = src;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, cw < g_sw ? cw : g_sw, ch < g_sh ? ch : g_sh, 1};
        cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
    } else {
        cl->CopyResource(dst, src);
    }
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    cl->ResourceBarrier(src_transition ? 2 : 1, src_transition ? b : b + 1);
    if (eye >= 0) g_filled.fetch_or(1 << eye, std::memory_order_relaxed);
    g_copies.fetch_add(1, std::memory_order_relaxed);
}

void xr_bind_tap(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int,
                 const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    if (n == 0 || !rts) return;
    int t = active_target();
    bool target = false;
    for (int i = 0; i < g_target_nrtv[t]; ++i) target = target || rts[0].ptr == g_target_rtv[t][i];
    if (!target) return;
    bool swap = camera_lever::swap_order();
    int first = swap ? 1 : 0, second = 1 - first;
    if (!ds) {
        int i = g_fxaa_binds++;
        if (g_blit) {
            // the content before binds 2..4 (index 1..3): one of them is the first eye's final image
            if (i >= 1 && i <= 3) copy_eye_to(cl, -1, 1 + i);
        } else if (i == 1) {
            copy_eye(cl, first);
        }
        return;
    }
    if (g_fxaa_binds >= 2) {
        copy_eye(cl, second);
        if (g_blit) {
            // both post runs bind the output the same number of times: the second run starts at bind m/2 + 1
            int k = g_fxaa_binds / 2;
            if (k > 3) k = 3;
            xr_blit::set_source(first, 1 + k);
            xr_blit::set_source(second, second);
            g_filled.fetch_or(1 << first, std::memory_order_relaxed);
            if (k != 1) g_late_eye_binds.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (g_fxaa_binds == 1) {
        copy_eye(cl, 0);
        copy_eye(cl, 1);
        if (g_blit) {
            xr_blit::set_source(0, 0);
            xr_blit::set_source(1, 1);
        }
    }
    g_fxaa_binds = 0;
}

// [XR] EyeShape: the eyes' FOV tangents (l r u d) for the session's eye size, before the first submitted frame: the
// views located so far, else one empty XR frame here to locate them (the presenting thread, under the frame mutex,
// before the first layer).
bool shape_views(float tan[2][4]) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            std::lock_guard lock(g_views_mutex);
            if (g_views_valid) {
                for (int e = 0; e < 2; ++e)
                    for (int k = 0; k < 4; ++k) tan[e][k] = std::tan(g_views[e].fov[k]);
                return true;
            }
        }
        if (attempt) break;
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(g_session, &wi, &fs))) return false;
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(g_session, &bi))) return false;
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g_space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t nv = 0;
        if (XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &nv, v)) && nv == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
            store_views(v, g_frames.load() + 1);
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = g_blend;
        ei.layerCount = 0;
        xrEndFrame(g_session, &ei);
        ++g_frames;
        g_empty.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

// Presenting thread, once the session runs: the eye swapchains, sized like the Post FXAA Target (or at the eye size,
// [XR] EyeShape), within the runtime's largest image (fit_image: a larger frame is resampled into them by the blit).
// With [XR] ColourBlit (default) they are sRGB and filled by the colour blit (xr_blit.h); otherwise they take the
// target's own format and raw copies (R5 step 1: geometry and pacing only, the colours wrong).
bool make_swapchains() {
    if (g_raw_refused) return false;
    auto* target = static_cast<ID3D12Resource*>(const_cast<void*>(d3d::unique_resource("Post FXAA Target")));
    auto* target_taa = static_cast<ID3D12Resource*>(const_cast<void*>(d3d::unique_resource("FXAATarget")));
    if (!target) return false;
    D3D12_RESOURCE_DESC d = target->GetDesc();
    if (target_taa) {
        D3D12_RESOURCE_DESC dt = target_taa->GetDesc();
        if (dt.Width != d.Width || dt.Height != d.Height || dt.Format != d.Format) {
            log::error("[xr] FXAATarget differs from the Post FXAA Target in size or format: TAA frames not submitted");
            target_taa = nullptr;
        }
    }
    // the frame within the runtime's largest image, then [XR] EyeShape's eye size for this session (eye-shaped
    // swapchains when configured; else the frame's), within it too
    uint32_t fw = 0, fh = 0;
    fit_image(static_cast<uint32_t>(d.Width), d.Height, g_cap_w.load(), g_cap_h.load(), &fw, &fh);
    uint32_t sw = fw, sh = fh;
    {
        float tan[2][4];
        const bool have = eye_shape::configured() && shape_views(tan);
        eye_shape::plan_session(static_cast<uint32_t>(d.Width), d.Height, fw, fh, have ? tan : nullptr, &sw, &sh);
    }
    uint32_t nf = 0;
    int64_t formats[64];
    xrEnumerateSwapchainFormats(g_session, 64, &nf, formats);
    DXGI_FORMAT fmt = d.Format;
    g_blit = false;
    if (config::get_bool("XR", "ColourBlit", true)) {
        DXGI_FORMAT want = xr_blit::pick_format(formats, nf);
        if (want != DXGI_FORMAT_UNKNOWN &&
            xr_blit::init(state::device.load(), static_cast<uint32_t>(d.Width), d.Height, d.Format, want)) {
            fmt = want;
            g_blit = true;
            xr_blit::set_dst_size(sw, sh);
        } else {
            log::error("[xr] colour blit unavailable: raw copies (colours wrong)");
        }
    }
    if (!g_blit && (fw != d.Width || fh != d.Height)) {  // once: make_swapchains is not tried again
        g_raw_refused = true;
        log::error("[xr] raw copies ([XR] ColourBlit=0, or the blit unavailable) cannot resample the %llux%u frame into the runtime's largest "
                   "image %ux%u: no submission (the colour blit resamples it; or a smaller [Render] RenderResolution)",
                   static_cast<unsigned long long>(d.Width), d.Height, g_cap_w.load(), g_cap_h.load());
        return false;
    }
    bool have = false;
    for (uint32_t i = 0; i < nf; ++i) have = have || formats[i] == static_cast<int64_t>(fmt);
    if (!have) {
        log::error("[xr] the runtime offers no swapchain format %d: no submission", static_cast<int>(fmt));
        return false;
    }
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = static_cast<int64_t>(fmt);
        ci.sampleCount = 1;
        ci.width = sw;
        ci.height = sh;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XrResult r = xrCreateSwapchain(g_session, &ci, &g_chain[e].sc);
        if (XR_FAILED(r)) {
            log::error("[xr] xrCreateSwapchain -> %s", result_name(g_inst, r));
            return false;
        }
        uint32_t ni = 0;
        xrEnumerateSwapchainImages(g_chain[e].sc, 0, &ni, nullptr);
        std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(g_chain[e].sc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        for (auto& im : imgs) g_chain[e].images.push_back(im.texture);
    }
    g_w = static_cast<uint32_t>(d.Width);
    g_h = d.Height;
    g_sw = sw;
    g_sh = sh;
    g_target_fmt = d.Format;
    ID3D12Resource* targets[2] = {target, target_taa};
    for (int t = 0; t < 2; ++t) {
        if (!targets[t]) continue;
        g_target_nrtv[t] = d3d::rtv_handles_of(targets[t], g_target_rtv[t], 8);
        g_target[t] = targets[t];
        d3d::watch_add(targets[t]);
    }
    // the live-resize guard watches this frame from here (a target or back buffer named, or ResizeBuffers, at another size)
    g_sess_bw = state::swap_width.load();
    g_sess_bh = state::swap_height.load();
    g_sess_h = g_h;
    g_sess_w = g_w;
    d3d::set_bind_tap(d3d::kBindTapXr, xr_bind_tap);
    log::info("[xr] submission: 2 swapchains %ux%u format %d (%s), %zu images each, RTVs: Post FXAA Target %d, FXAATarget %d%s",
              g_sw, g_sh, static_cast<int>(fmt), g_blit ? "colour blit" : "raw copies", g_chain[0].images.size(),
              g_target_nrtv[0], g_target_nrtv[1],
              g_sw != g_w || g_sh != g_h
                  ? (fw != g_w || fh != g_h ? (g_sw != fw || g_sh != fh ? " (EyeShape: the eye size; the post output is larger than the runtime's largest image)"
                                                                         : " (the post output resampled into the runtime's largest image)")
                                            : " (EyeShape: the eye size; the post output is larger)")
                  : "");
    // the UI target is the frame the eyes use (made at the redirect's first frame end): else the redirect stays off
    uint32_t uw = 0, uh = 0;
    ui_layer::size(&uw, &uh);
    if (uw && (uw != g_w || uh != g_h)) {
        log::error("[xr] the UI target %ux%u is not the frame's %ux%u (the game made its frame again before the session)", uw, uh, g_w, g_h);
        ui_layer::stop("the UI target is not the frame's size");
    }
    return true;
}

// ---- UI quad (R5; DESIGN 3.7 MVP: a whole-HUD lazy-follow quad). The UI the redirect keeps out of the eye images
// (ui_layer.h) on a quad 2 m ahead, 1.8 m wide, slightly below eye height; it turns with the head only when the head
// has turned more than 20 degrees away from it, and then eases back to the head's yaw.
XrSwapchain g_ui_sc = XR_NULL_HANDLE;
std::vector<ID3D12Resource*> g_ui_images;
DXGI_FORMAT g_ui_fmt = DXGI_FORMAT_UNKNOWN;
uint32_t g_ui_w = 0, g_ui_h = 0;    // the quad's swapchain: the UI target's size, at most [XR] UiQuadMaxWidth wide
uint32_t g_ui_tw = 0, g_ui_th = 0;  // the UI target (the frame's size), drawn into it texel for texel or filtered
// the wrist HUD ([XR] Hud=wrist): the UI's corner with the radar, its meters and the ammo counter above it ([XR]
// WristHudRect, fractions of the screen) on the back of the off hand, shown while you look at it; the floating quad
// keeps the rest (the prompts)
std::atomic<uint32_t> g_max_layers{16};  // XrSystemGraphicsProperties::maxLayerCount
std::atomic<bool> g_hud_wrist{false};
float g_hud_rect[4] = {0.02f, 0.63f, 0.34f, 0.96f};
XrSwapchain g_wr_sc = XR_NULL_HANDLE;
std::vector<ID3D12Resource*> g_wr_images;
int g_wr_x0 = 0, g_wr_y0 = 0;        // the crop in the UI target's pixels
uint32_t g_wr_cw = 0, g_wr_ch = 0;
uint32_t g_wr_w = 0, g_wr_h = 0;     // its swapchain: the crop at the UI quad's scale
bool g_wr_failed = false, g_wr_shown = false;
std::atomic<bool> g_wr_visible{false};  // g_wr_shown for other threads (false while the HUD cannot show)
std::atomic<float> g_wr_up[3] = {0.0f, 0.0f, 0.0f};  // the last shown frame's image top (test status)
std::atomic<uint64_t> g_wr_frames{0}, g_wr_hidden{0};
bool g_ui_failed = false;
float g_ui_yaw = 0;
bool g_ui_placed = false, g_ui_following = false;
std::atomic<uint64_t> g_ui_frames{0};

bool make_ui_swapchain() {
    if (g_ui_sc != XR_NULL_HANDLE) return true;
    if (g_ui_failed) return false;
    g_ui_failed = true;
    uint32_t nf = 0;
    int64_t formats[64];
    xrEnumerateSwapchainFormats(g_session, 64, &nf, formats);
    g_ui_fmt = xr_blit::pick_format(formats, nf);
    ui_layer::size(&g_ui_tw, &g_ui_th);
    if (g_ui_fmt == DXGI_FORMAT_UNKNOWN || !g_ui_tw) {
        log::error("[xr] UI quad: no sRGB swapchain format or no UI target");
        return false;
    }
    // at most [XR] UiQuadMaxWidth wide (default 2560: the quad is 1.8 m at 2 m, about 48 degrees; a 2560x1440 or smaller
    // frame's UI is its own size, texel for texel), and the runtime's largest image
    fit_image(g_ui_tw, g_ui_th, quad_max_w("UiQuadMaxWidth"), g_cap_h.load(), &g_ui_w, &g_ui_h);
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = static_cast<int64_t>(g_ui_fmt);
    ci.sampleCount = 1;
    ci.width = g_ui_w;
    ci.height = g_ui_h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g_session, &ci, &g_ui_sc);
    if (XR_FAILED(r)) {
        log::error("[xr] UI quad: xrCreateSwapchain -> %s", result_name(g_inst, r));
        g_ui_sc = XR_NULL_HANDLE;
        return false;
    }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages(g_ui_sc, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_ui_sc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    for (auto& im : imgs) g_ui_images.push_back(im.texture);
    g_ui_failed = false;
    if (g_ui_w == g_ui_tw && g_ui_h == g_ui_th)
        log::info("[xr] UI quad swapchain %ux%u format %d, %zu images", g_ui_w, g_ui_h, static_cast<int>(g_ui_fmt), g_ui_images.size());
    else
        log::info("[xr] UI quad swapchain %ux%u format %d, %zu images (the UI target %ux%u box-filtered into it: [XR] UiQuadMaxWidth %u)", g_ui_w,
                  g_ui_h, static_cast<int>(g_ui_fmt), g_ui_images.size(), g_ui_tw, g_ui_th, quad_max_w("UiQuadMaxWidth"));
    return true;
}

// The wrist HUD's swapchain: the crop (fractions of the UI target, in its pixels) at the UI quad's scale (its own size
// when the quad is the UI's), the UI quad's format.
bool make_wrist_swapchain() {
    if (g_wr_sc != XR_NULL_HANDLE) return true;
    if (g_wr_failed || !make_ui_swapchain()) return false;
    g_wr_failed = true;
    auto px = [](float f, uint32_t n) {
        const int v = static_cast<int>(f * static_cast<float>(n));
        return v < 0 ? 0 : v > static_cast<int>(n) ? static_cast<int>(n) : v;
    };
    g_wr_x0 = px(g_hud_rect[0], g_ui_tw);
    g_wr_y0 = px(g_hud_rect[1], g_ui_th);
    const int x1 = px(g_hud_rect[2], g_ui_tw), y1 = px(g_hud_rect[3], g_ui_th);
    if (x1 - g_wr_x0 < 16 || y1 - g_wr_y0 < 16) return false;
    g_wr_cw = static_cast<uint32_t>(x1 - g_wr_x0);
    g_wr_ch = static_cast<uint32_t>(y1 - g_wr_y0);
    uint32_t max_w = g_wr_cw;
    if (g_ui_w != g_ui_tw) {
        max_w = static_cast<uint32_t>(std::lround(static_cast<double>(g_wr_cw) * g_ui_w / g_ui_tw));
        if (max_w < 16) max_w = 16;
    }
    fit_image(g_wr_cw, g_wr_ch, max_w, g_cap_h.load(), &g_wr_w, &g_wr_h);
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = static_cast<int64_t>(g_ui_fmt);
    ci.sampleCount = 1;
    ci.width = g_wr_w;
    ci.height = g_wr_h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g_session, &ci, &g_wr_sc);
    if (XR_FAILED(r)) {
        log::error("[xr] wrist HUD: xrCreateSwapchain -> %s", result_name(g_inst, r));
        g_wr_sc = XR_NULL_HANDLE;
        return false;
    }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages(g_wr_sc, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_wr_sc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    for (auto& im : imgs) g_wr_images.push_back(im.texture);
    g_wr_failed = false;
    if (g_wr_w == g_wr_cw && g_wr_h == g_wr_ch)
        log::info("[xr] wrist HUD swapchain %ux%u from UI pixel (%d, %d)", g_wr_w, g_wr_h, g_wr_x0, g_wr_y0);
    else
        log::info("[xr] wrist HUD swapchain %ux%u from UI pixel (%d, %d): the UI's %ux%u there, box-filtered", g_wr_w, g_wr_h, g_wr_x0, g_wr_y0,
                  g_wr_cw, g_wr_ch);
    return true;
}

float wrap_pi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// Presenting thread: the quad's pose for this frame's views (lazy follow on yaw).
XrPosef ui_pose() {
    const XrQuaternionf& q = g_frame_views[0].pose.orientation;
    // the head's forward (-z) in the horizontal plane
    float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float head_yaw = std::atan2(-fx, -fz);
    if (!g_ui_placed) {
        g_ui_yaw = head_yaw;
        g_ui_placed = true;
    }
    float d = wrap_pi(head_yaw - g_ui_yaw);
    if (std::fabs(d) > 20.0f / 57.29578f) g_ui_following = true;
    if (g_ui_following) {
        g_ui_yaw = wrap_pi(g_ui_yaw + 0.08f * d);
        if (std::fabs(d) < 2.0f / 57.29578f) g_ui_following = false;
    }
    float mx = 0.5f * (g_frame_views[0].pose.position.x + g_frame_views[1].pose.position.x);
    float my = 0.5f * (g_frame_views[0].pose.position.y + g_frame_views[1].pose.position.y);
    float mz = 0.5f * (g_frame_views[0].pose.position.z + g_frame_views[1].pose.position.z);
    XrPosef p{};
    p.orientation = {0, std::sin(0.5f * g_ui_yaw), 0, std::cos(0.5f * g_ui_yaw)};
    p.position = {mx - 2.0f * std::sin(g_ui_yaw), my - 0.15f, mz - 2.0f * std::cos(g_ui_yaw)};
    return p;
}

// Presenting thread, frame end: this frame's UI into an acquired quad image. False when there is none.
bool fill_ui(XrCompositionLayerQuad& quad) {
    if (!ui_layer::has_ui() || !make_ui_swapchain()) return false;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wsi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wsi.timeout = 100000000;
    if (XR_FAILED(xrAcquireSwapchainImage(g_ui_sc, &ai, &index))) return false;
    // with the HUD on the wrist, its corner is left out here (the prompts stay; in the UI target's pixels)
    const bool wrist = g_hud_wrist.load(std::memory_order_relaxed) && g_wr_sc != XR_NULL_HANDLE;
    const int hole[4] = {g_wr_x0, g_wr_y0, g_wr_x0 + static_cast<int>(g_wr_cw), g_wr_y0 + static_cast<int>(g_wr_ch)};
    bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(g_ui_sc, &wsi)) && index < g_ui_images.size() &&
              ui_layer::draw_quad(g_ui_images[index], g_ui_fmt, wrist ? hole : nullptr);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_ui_sc, &ri);
    if (!ok) return false;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g_space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_ui_sc;
    quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_ui_w), static_cast<int32_t>(g_ui_h)}};
    quad.pose = ui_pose();
    quad.size = {1.8f, 1.8f * static_cast<float>(g_ui_h) / static_cast<float>(g_ui_w)};
    g_ui_frames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// q * v (x y z w)
void qrot(const XrQuaternionf& q, const float v[3], float out[3]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}
// A rotation from its axes (unit, right-handed: the matrix's columns x, y, z).
XrQuaternionf quat_from_axes(const float x[3], const float y[3], const float z[3]) {
    const float m00 = x[0], m10 = x[1], m20 = x[2], m01 = y[0], m11 = y[1], m21 = y[2], m02 = z[0], m12 = z[1], m22 = z[2];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float t = std::sqrt(tr + 1.0f) * 2.0f;
        return {(m21 - m12) / t, (m02 - m20) / t, (m10 - m01) / t, 0.25f * t};
    }
    if (m00 > m11 && m00 > m22) {
        const float t = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        return {0.25f * t, (m01 + m10) / t, (m02 + m20) / t, (m21 - m12) / t};
    }
    if (m11 > m22) {
        const float t = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        return {(m01 + m10) / t, 0.25f * t, (m12 + m21) / t, (m02 - m20) / t};
    }
    const float t = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    return {(m02 + m20) / t, (m12 + m21) / t, 0.25f * t, (m10 - m01) / t};
}

// Presenting thread, frame end: the wrist HUD on the off hand's back (like a watch: looked at with the palm flat, face
// down), read upright from the eyes, only while the head looks at it and it faces them (on within 30 degrees of the
// gaze, off past 40).
bool fill_wrist(XrCompositionLayerQuad& quad) {
    if (!g_hud_wrist.load(std::memory_order_relaxed) || !ui_layer::has_ui() || !make_wrist_swapchain()) {
        g_wr_visible.store(false, std::memory_order_relaxed);
        return false;
    }
    const int h = 1 - controls::layout_gun_hand();  // the layout's off hand (it does not hop with a draw by the other hand)
    const hands::Hand hd = hands::get(h);
    if (!hd.grip_valid) {
        g_wr_visible.store(false, std::memory_order_relaxed);
        return false;
    }
    const XrQuaternionf g{hd.grip_rot[0], hd.grip_rot[1], hd.grip_rot[2], hd.grip_rot[3]};
    // the back of the hand: OpenXR's grip +x points out of the left palm and into the right one, so -x on the left
    // hand and +x on the right (round 7: the HUD showed on the inner wrist)
    const float back[3] = {h == 0 ? -1.0f : 1.0f, 0, 0}, toward_wrist[3] = {0, 0, 1};
    float bw[3], tw[3];
    qrot(g, back, bw);
    qrot(g, toward_wrist, tw);
    float pos[3];
    for (int k = 0; k < 3; ++k) pos[k] = hd.grip_pos[k] + bw[k] * 0.035f + tw[k] * 0.07f;
    const float* n = bw;  // the quad faces out of the back of the hand
    // looked at: the gaze (the first view's -z) within the cone, and the face toward the eyes
    const XrPosef& v0 = g_frame_views[0].pose;
    float eye[3] = {0.5f * (g_frame_views[0].pose.position.x + g_frame_views[1].pose.position.x),
                    0.5f * (g_frame_views[0].pose.position.y + g_frame_views[1].pose.position.y),
                    0.5f * (g_frame_views[0].pose.position.z + g_frame_views[1].pose.position.z)};
    const float mz[3] = {0, 0, -1};
    float gaze[3], to[3];
    qrot(v0.orientation, mz, gaze);
    for (int k = 0; k < 3; ++k) to[k] = pos[k] - eye[k];
    const float tl = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (tl < 1e-3f) return false;
    const float cosg = (gaze[0] * to[0] + gaze[1] * to[1] + gaze[2] * to[2]) / tl;
    const float face = -(n[0] * to[0] + n[1] * to[1] + n[2] * to[2]) / tl;
    // its top reads as up from the eyes: the line where its plane meets the plane of the view ray and the head's up,
    // n x (v x u) = v (n.u) - u (n.v) (a page on a table: the top away from them; facing them: the head's up); toward
    // the fingers when that is edge-on; x = y cross z
    float qy[3], qx[3];
    {
        const float my[3] = {0, 1, 0};
        float up[3];
        qrot(v0.orientation, my, up);
        const float nu = n[0] * up[0] + n[1] * up[1] + n[2] * up[2], nv = (n[0] * to[0] + n[1] * to[1] + n[2] * to[2]) / tl;
        for (int k = 0; k < 3; ++k) qy[k] = to[k] / tl * nu - up[k] * nv;
        float l = std::sqrt(qy[0] * qy[0] + qy[1] * qy[1] + qy[2] * qy[2]);
        if (l < 1e-3f) {
            for (int k = 0; k < 3; ++k) qy[k] = -tw[k];
            l = 1.0f;
        }
        for (int k = 0; k < 3; ++k) qy[k] /= l;
        qx[0] = qy[1] * n[2] - qy[2] * n[1];
        qx[1] = qy[2] * n[0] - qy[0] * n[2];
        qx[2] = qy[0] * n[1] - qy[1] * n[0];
    }
    const XrQuaternionf qo = quat_from_axes(qx, qy, n);
    const float cone = std::cos((g_wr_shown ? 40.0f : 30.0f) * 0.0174532925f);
    const bool look = cosg > cone && face > 0.25f;
    g_wr_visible.store(look, std::memory_order_relaxed);
    if (look != g_wr_shown) {
        g_wr_shown = look;
        log::info("[xr] wrist HUD %s (gaze %.0f deg off, face %.2f)", look ? "shown" : "hidden", std::acos(cosg > 1 ? 1 : cosg) * 57.29578f, face);
    }
    if (!look) {
        g_wr_hidden.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wsi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wsi.timeout = 100000000;
    if (XR_FAILED(xrAcquireSwapchainImage(g_wr_sc, &ai, &index))) return false;
    bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(g_wr_sc, &wsi)) && index < g_wr_images.size() &&
              ui_layer::draw_crop(g_wr_images[index], g_ui_fmt, g_wr_x0, g_wr_y0, g_wr_cw, g_wr_ch);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_wr_sc, &ri);
    if (!ok) return false;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g_space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_wr_sc;
    quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_wr_w), static_cast<int32_t>(g_wr_h)}};
    quad.pose.orientation = qo;
    quad.pose.position = {pos[0], pos[1], pos[2]};
    for (int k = 0; k < 3; ++k) g_wr_up[k].store(qy[k], std::memory_order_relaxed);
    quad.size = {0.16f, 0.16f * static_cast<float>(g_wr_h) / static_cast<float>(g_wr_w)};
    g_wr_frames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Presenting thread, as a frame with a layer ends: did the scene draw it with exactly the views the layer carries?
void check_poses(uint64_t frame) {
    EyeView sub[2];
    to_eye_views(g_frame_views, sub);
    std::lock_guard lock(g_views_mutex);
    if (!g_handed_n)
        g_pose_none.fetch_add(1, std::memory_order_relaxed);
    else if (!g_handed_other && g_handed_frame == frame && std::memcmp(g_handed, sub, sizeof(sub)) == 0)
        g_pose_match.fetch_add(1, std::memory_order_relaxed);
    else
        g_pose_stale.fetch_add(1, std::memory_order_relaxed);
    g_handed_n = 0;
    g_handed_other = false;
}

void end_open_frame() {
    if (g_relayout.exchange(false)) {
        g_cinema_placed = false;
        g_ui_placed = false;
    }
    // the live-resize guard: no eye blit and no projection layer from its first sign on, the flat game on the cinema
    // screen (placed from the head when it was not on)
    const bool resized = g_resized.load(std::memory_order_acquire);
    static bool s_resize_seen = false;  // presenting thread
    if (resized && !s_resize_seen) {
        s_resize_seen = true;
        if (!g_cinema.load()) g_cinema_placed = false;
    }
    ID3D12Resource* acquired[2] = {g_dst[0].load(), g_dst[1].load()};
    g_dst[0] = nullptr;
    g_dst[1] = nullptr;
    // [XR] EyeShape: the eye content (the post output's top-left eye rect, else all of it) and the image submitted (the
    // content itself when it fits the swapchain image, else the swapchain image, resampled into: also a frame larger
    // than the runtime's largest image, the swapchains fitted to it with the frame's aspect); without either the whole
    // post output, as before
    uint32_t cw = g_w, ch = g_h, rw = 0, rh = 0;
    const bool rect = eye_shape::frame_rect(&cw, &ch, &rw, &rh);
    const uint32_t iw = cw <= g_sw ? cw : g_sw, ih = ch <= g_sh ? ch : g_sh;
    // the eyes' queue: the session's, after the game's work (the raw copies are in the game's lists: the fence alone)
    ID3D12CommandQueue* q = g_filled.load() == 3 && !resized ? image_queue() : nullptr;
    if (g_blit && g_filled.load() == 3 && !resized) {
        const xr_blit::Frame f{cw, ch, iw, ih, rect ? rw : 0, rect ? rh : 0};
        const bool plain = !rect && g_sw == g_w && g_sh == g_h;
        if (!q || !xr_blit::blit(q, acquired, xr_blit::game_gamma(1.0f), plain ? nullptr : &f)) g_filled = 0;  // not converted: no layer
    }
    g_rect_w.store(iw, std::memory_order_relaxed);
    g_rect_h.store(ih, std::memory_order_relaxed);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (g_eyes_acquired)
        for (auto& c : g_chain) xrReleaseSwapchainImage(c.sc, &ri);
    g_eyes_acquired = false;
    XrCompositionLayerProjectionView pv[2];
    for (int i = 0; i < 2; ++i) {
        pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        pv[i].pose = g_frame_views[i].pose;
        pv[i].fov = g_frame_views[i].fov;
        pv[i].subImage.swapchain = g_chain[i].sc;
        pv[i].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(iw), static_cast<int32_t>(ih)}};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_space;
    layer.viewCount = 2;
    layer.views = pv;
    XrCompositionLayerQuad ui_quad{XR_TYPE_COMPOSITION_LAYER_QUAD}, menu_quad{XR_TYPE_COMPOSITION_LAYER_QUAD},
        wrist_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad ring_quads[holster::kMaxMarkers];
    XrCompositionLayerQuad reticle_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    constexpr uint32_t kLayerArray = 16;
    const XrCompositionLayerBaseHeader* layers[kLayerArray] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    uint32_t nlayers = 1;
    bool full = g_should_render && g_filled.load() == 3 && !resized;
    if (full) check_poses(g_frames.load() + 1);
    const bool cinema_on = g_cinema.load() || resized;
    // the holster rings over the scene, under the HUD, the wrist and the menu (which keep three places); never more
    // layers than the runtime takes (a frame over it is dropped whole)
    if (full && !cinema_on) {
        const uint32_t cap = g_max_layers.load(std::memory_order_relaxed) < kLayerArray ? g_max_layers.load(std::memory_order_relaxed) : kLayerArray;
        const int room = static_cast<int>(cap) - static_cast<int>(nlayers) - 3;
        const int nr = zone_rings::frame(g_frame_views, g_session, g_space, ring_quads,
                                         room - 1 < holster::kMaxMarkers ? room - 1 : holster::kMaxMarkers);  // one kept for the reticle
        for (int i = 0; i < nr; ++i) layers[nlayers++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ring_quads[i]);
        if (room - nr >= 1 && zone_rings::reticle_frame(g_frame_views, g_session, g_space, &reticle_quad))
            layers[nlayers++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&reticle_quad);
    }
    bool ui = full && !cinema_on && fill_ui(ui_quad);
    if (ui) layers[nlayers++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ui_quad);
    if (ui && fill_wrist(wrist_quad)) layers[nlayers++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&wrist_quad);
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* quad_layers[2] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    uint32_t nquad = 1;
    bool cinema = cinema_on && g_should_render;
    if (cinema) {
        if (!g_cinema_placed) place_cinema();
        cinema = fill_quad();
        // the picture drawn this frame (the whole image when copied; the back buffer's aspect in its top-left when
        // resampled): the screen 3.2 m wide at that aspect
        const uint32_t qw = g_quad_rw.load(std::memory_order_relaxed), qh = g_quad_rh.load(std::memory_order_relaxed);
        quad.space = g_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_quad;
        quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(qw), static_cast<int32_t>(qh)}};
        quad.pose = g_cinema_pose;
        quad.size = {3.2f, 3.2f * static_cast<float>(qh) / static_cast<float>(qw ? qw : 1)};
    }
    // the menu over everything, in either view
    if (g_should_render && menu::frame(g_frame_views, g_session, g_space, &menu_quad)) {
        if (cinema) quad_layers[nquad++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu_quad);
        else if (full) layers[nlayers++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu_quad);
    }
    image_written();  // the frame's XR writes are in: the game's next lists wait for them
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_display_time;
    ei.environmentBlendMode = g_blend;
    ei.layerCount = cinema ? nquad : (full ? nlayers : 0);
    ei.layers = cinema ? quad_layers : layers;
    if (cinema) g_cinema_frames.fetch_add(1, std::memory_order_relaxed);
    XrResult r = xrEndFrame(g_session, &ei);
    if (XR_FAILED(r)) log::limited("xr.endframe", 8, "[xr] xrEndFrame -> %s", result_name(g_inst, r));
    if (g_open_ms > 0) {  // the timing window: how long this XR frame was open, and its wait
        const double open = log::now_ms() - g_open_ms;
        g_tw.open_sum += open;
        if (open > g_tw.open_max) g_tw.open_max = open;
        if (g_tw.nopen < sizeof(g_tw.open) / sizeof(g_tw.open[0])) g_tw.open[g_tw.nopen++] = static_cast<float>(open);
        g_tw.wait_sum += g_wait_ms;
        if (g_wait_ms > g_tw.wait_max) g_tw.wait_max = g_wait_ms;
    }
    ++g_frames;
    (full ? g_submitted : g_empty).fetch_add(1, std::memory_order_relaxed);
    g_frame_open = false;
}

// The timing window (the presenting thread, after each xrWaitFrame): the display gaps as perf counts them, and every
// [XR] TimingLog seconds one log line with the window's frames, waits, open times, CPU/GPU, pose check and late latch
void timing_window(const XrFrameState& fs) {
    static const double every = config::get_float("XR", "TimingLog", 10.0f);
    static XrTime prev = 0;
    if (every <= 0) return;
    const double now = log::now_ms();
    if (g_tw.start_ms <= 0) {
        g_tw = TimingWin{};
        g_tw.start_ms = now;
        g_tw.pose_m0 = g_pose_match.load();
        g_tw.pose_s0 = g_pose_stale.load();
        g_tw.pose_n0 = g_pose_none.load();
        g_tw.latch0 = g_latched.load();
        g_tw.latch_sum0 = g_latch_sum_deg.load();
        g_tw.copies0 = g_copies.load();
        g_tw.misses0 = g_misses.load();
        g_tw.late_binds0 = g_late_eye_binds.load();
        prev = 0;
    }
    if (fs.predictedDisplayPeriod > 0) {
        const double period = static_cast<double>(fs.predictedDisplayPeriod);
        g_tw.period_ms = period / 1e6;
        if (prev) {
            const double gap = static_cast<double>(fs.predictedDisplayTime - prev) / period;
            if (gap > 1.5) {
                g_tw.missed += static_cast<uint64_t>(gap + 0.5) - 1;
                ++g_tw.late;
            }
            if (gap > g_tw.longest) g_tw.longest = gap;
        }
        prev = fs.predictedDisplayTime;
    }
    ++g_tw.frames;
    g_tw.cpu_sum += state::cpu_frame_ms.load();
    g_tw.gpu_sum += state::gpu_frame_ms.load();
    const double secs = (now - g_tw.start_ms) / 1000.0;
    if (secs < every) return;
    float p95 = 0;
    if (g_tw.nopen) {
        std::vector<float> v(g_tw.open, g_tw.open + g_tw.nopen);
        std::nth_element(v.begin(), v.begin() + (v.size() * 95) / 100, v.end());
        p95 = v[(v.size() * 95) / 100];
    }
    const double n = g_tw.frames ? static_cast<double>(g_tw.frames) : 1.0, no = g_tw.nopen ? static_cast<double>(g_tw.nopen) : 1.0;
    const uint64_t lat = g_latched.load() - g_tw.latch0;
    log::info("[xr] timing %.1f s: %llu frames (%.2f Hz, display period %.3f ms), missed periods %llu (%.2f%%) in %llu late frames, "
              "longest gap %.2f periods | xrWaitFrame wait mean %.2f max %.2f ms | frame open mean %.2f p95 %.2f max %.2f ms | "
              "CPU %.2f GPU %.2f ms | poses this frame %llu, another %llu, none %llu | late latch %llu, mean %.3f deg | eye copies %llu, "
              "missed %llu, first eye late %llu",
              secs, static_cast<unsigned long long>(g_tw.frames), g_tw.frames / secs, g_tw.period_ms,
              static_cast<unsigned long long>(g_tw.missed),
              100.0 * static_cast<double>(g_tw.missed) / static_cast<double>(g_tw.frames + g_tw.missed ? g_tw.frames + g_tw.missed : 1),
              static_cast<unsigned long long>(g_tw.late), g_tw.longest, g_tw.wait_sum / no, g_tw.wait_max, g_tw.open_sum / no, p95,
              g_tw.open_max, g_tw.cpu_sum / n, g_tw.gpu_sum / n,
              static_cast<unsigned long long>(g_pose_match.load() - g_tw.pose_m0), static_cast<unsigned long long>(g_pose_stale.load() - g_tw.pose_s0),
              static_cast<unsigned long long>(g_pose_none.load() - g_tw.pose_n0), static_cast<unsigned long long>(lat),
              lat ? (g_latch_sum_deg.load() - g_tw.latch_sum0) / static_cast<float>(lat) : 0.0f,
              static_cast<unsigned long long>(g_copies.load() - g_tw.copies0), static_cast<unsigned long long>(g_misses.load() - g_tw.misses0),
              static_cast<unsigned long long>(g_late_eye_binds.load() - g_tw.late_binds0));
    g_tw.start_ms = 0;  // the next frame starts a new window
}

// Presenting thread, at each game frame end (before its Present).
void submit_frame_end() {
    std::lock_guard lock(g_frame_mutex);
    if (!g_session_running.load()) return;
    if (!g_submit.load()) {
        if (!make_swapchains()) return;
        g_submit = true;
    }
    if (g_frame_open) end_open_frame();
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    const double w0 = log::now_ms();
    XrResult r = xrWaitFrame(g_session, &wi, &fs);
    if (XR_FAILED(r)) {
        log::limited("xr.waitframe", 8, "[xr] xrWaitFrame -> %s", result_name(g_inst, r));
        return;
    }
    g_wait_ms = log::now_ms() - w0;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(g_session, &bi);
    if (XR_FAILED(r)) {
        log::limited("xr.beginframe", 8, "[xr] xrBeginFrame -> %s", result_name(g_inst, r));
        return;
    }
    g_open_ms = log::now_ms();
    g_display_time = fs.predictedDisplayTime;
    g_should_render = fs.shouldRender == XR_TRUE;
    {
        const char* why = nullptr;
        XrTime at = g_space_change_at.load();
        if (at && fs.predictedDisplayTime >= at) {
            g_space_change_at = 0;
            why = "the headset's recentre";
        }
        // Scroll Lock while the game is in front (the game reads raw keyboard input; this key is unbound in it)
        bool down = false;
        DWORD pid = 0;
        if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) down = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
        if (down && !g_scroll_down) why = "Scroll Lock";
        g_scroll_down = down;
        if (xinput::take_recentre_combo()) why = "the gamepad (L3 + R3 held)";
        if (controls::take_recentre()) why = "the controllers (both stick clicks held)";
        if (why) {
            g_recentres.fetch_add(1);
            log::info("[xr] recentre: %s", why);
            camera_lever::recentre();
        }
    }
    std::unique_lock perf_lock(g_perf_mutex);
    if (g_perf_reset.exchange(false)) {
        g_perf = Perf{};
        g_perf.start_ms = log::now_ms();
        camera_lever::scene_time(&g_perf.scene_us0, &g_perf.scene_n0);
    }
    if (fs.predictedDisplayPeriod > 0) {
        double period = static_cast<double>(fs.predictedDisplayPeriod);
        g_perf.period_ms = period / 1e6;
        if (g_perf.prev) {
            double gap = static_cast<double>(fs.predictedDisplayTime - g_perf.prev) / period;
            if (gap > 1.5) {
                g_perf.missed += static_cast<uint64_t>(gap + 0.5) - 1;
                ++g_perf.late_frames;
            }
            if (gap > g_perf.longest) g_perf.longest = gap;
        }
        g_perf.prev = fs.predictedDisplayTime;
        ++g_perf.frames;
        g_perf.cpu_sum += state::cpu_frame_ms.load();
        g_perf.gpu_sum += state::gpu_frame_ms.load();
    }
    perf_lock.unlock();
    timing_window(fs);
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t nv = 0;
    g_frame_views[0] = {XR_TYPE_VIEW};
    g_frame_views[1] = {XR_TYPE_VIEW};
    if (XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &nv, g_frame_views)) && nv == 2 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        apply_pos_ramp(g_frame_views);
        store_views(g_frame_views, g_frames.load() + 1);
    }
    controllers::sync(g_space, fs.predictedDisplayTime);
    g_filled = 0;
    g_eyes_acquired = !g_resized.load(std::memory_order_acquire);  // the live-resize guard: no eye images from then on
    for (int e = 0; e < 2; ++e) {
        if (!g_eyes_acquired) {
            g_dst[e] = nullptr;
            continue;
        }
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wsi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wsi.timeout = 100000000;  // 100 ms
        if (XR_FAILED(xrAcquireSwapchainImage(g_chain[e].sc, &ai, &index)) || XR_FAILED(xrWaitSwapchainImage(g_chain[e].sc, &wsi)) ||
            index >= g_chain[e].images.size()) {
            g_dst[e] = nullptr;
            continue;
        }
        g_dst[e] = g_chain[e].images[index];
    }
    g_frame_open = true;
}

// The live-resize guard's stop, on whichever thread saw the first sign (the game's main or render thread at SetName or
// ResizeBuffers, the recording thread at an eye copy). Nothing here waits: the bind tap goes, and a tap already running
// sees g_resized (or the target's own size) before its next copy; the frame end (under g_frame_mutex) reads g_resized
// once a frame. The old targets' views may be the new targets' handles, so the UI redirect goes too, and EyeShape.
void stop_eyes(const char* what, uint32_t w, uint32_t h) {
    if (g_resize_claim.exchange(true)) return;
    std::snprintf(g_resized_what, sizeof(g_resized_what), "%s %ux%u", what, w, h);
    g_resized.store(true, std::memory_order_seq_cst);
    d3d::set_bind_tap(d3d::kBindTapXr, nullptr);
    for (auto& t : g_target)
        if (ID3D12Resource* r = t.exchange(nullptr)) d3d::watch_remove(r);
    ui_layer::stop("the game's frame was made again at another size");
    eye_shape::stop("the game's frame was made again at another size");
    log::error("[xr] LIVE RESIZE: %s, the session's frame %ux%u (back buffer %ux%u): the eye images stop (no projection layer), the UI "
               "redirect and EyeShape are off, the flat game is on the cinema screen; restart the game for the headset view",
               g_resized_what, g_sess_w.load(), g_sess_h.load(), g_sess_bw.load(), g_sess_bh.load());
}

// d3d's frame-size listener: a target or back buffer named, or ResizeBuffers, at a size other than the session's.
void on_frame_size(const char* what, uint32_t w, uint32_t h) {
    const uint32_t fw = g_sess_w.load(std::memory_order_acquire), fh = g_sess_h.load(std::memory_order_relaxed);
    if (!fw || !w || !h || g_resized.load(std::memory_order_relaxed)) return;  // no eyes made yet; 0: the window's size (its "done" follows)
    const bool bb = std::strncmp(what, "Main", 4) == 0 || std::strncmp(what, "Resize", 6) == 0;
    const uint32_t ew = bb ? g_sess_bw.load(std::memory_order_relaxed) : fw, eh = bb ? g_sess_bh.load(std::memory_order_relaxed) : fh;
    if (!ew || !eh || (w == ew && h == eh)) return;
    stop_eyes(what, w, h);
}

void set_state(const char* s) {
    std::snprintf(g_state, sizeof(g_state), "%s", s);
    log::info("[xr] session state -> %s", s);
}

bool select_runtime() {
    wchar_t path[MAX_PATH];
    log::path_in_game_dir(L"RDRVR_xr_runtime.txt", path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
        log::info("[xr] no RDRVR_xr_runtime.txt: using the system's active OpenXR runtime");
        return true;
    }
    char line[MAX_PATH * 2] = {};
    std::fgets(line, sizeof(line), f);
    std::fclose(f);
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
    // Strip a UTF-8 BOM if the file was written with one.
    const char* p = line;
    if (n >= 3 && static_cast<unsigned char>(p[0]) == 0xEF) p += 3;
    wchar_t wide[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, p, -1, wide, MAX_PATH * 2);
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON", wide);
    int fps = config::get_int("Debug", "SimulatorFps", 0);  // the simulator's rate (SIMXR_FPS); 0 = its default
    if (fps > 0) {
        wchar_t rate[16];
        std::swprintf(rate, 16, L"%d", fps);
        SetEnvironmentVariableW(L"SIMXR_FPS", rate);
        log::info("[xr] SIMXR_FPS = %d", fps);
    }
    _wputenv_s(L"XR_RUNTIME_JSON", wide);
    log::info("[xr] XR_RUNTIME_JSON = %ls (from RDRVR_xr_runtime.txt)", wide);
    return true;
}

DWORD WINAPI session_thread(void*) {
    select_runtime();
    XrInstance inst = XR_NULL_HANDLE;
    std::vector<const char*> exts = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    {  // the extra controllers' interaction profiles, where the runtime offers them (controllers.cpp binds them)
        uint32_t n = 0;
        std::vector<XrExtensionProperties> props;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)) && n) {
            props.assign(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
            if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()))) props.clear();
        }
        std::string got;
        for (const char* want : controllers::kProfileExtensions)
            for (const XrExtensionProperties& p : props)
                if (std::strcmp(p.extensionName, want) == 0) {
                    exts.push_back(want);
                    controllers::note_extension(want);
                    got += std::string(" ") + want;
                }
        log::info("[xr] %zu instance extensions offered; the controllers' enabled:%s", props.size(), got.empty() ? " none" : got.c_str());
    }
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "RDRVR");
    std::snprintf(ci.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "RAGE (Red Dead Redemption)");
    ci.applicationInfo.applicationVersion = 1;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.enabledExtensionNames = exts.data();
    XrResult r = XR_ERROR_API_VERSION_UNSUPPORTED;
    for (XrVersion v : {XR_MAKE_VERSION(1, 1, 0), XR_MAKE_VERSION(1, 0, 0)}) {
        ci.applicationInfo.apiVersion = v;
        r = xrCreateInstance(&ci, &inst);
        log::info("[xr] xrCreateInstance(api %u.%u) -> %s", XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v), result_name(inst, r));
        if (r != XR_ERROR_API_VERSION_UNSUPPORTED) {
            if (XR_SUCCEEDED(r)) controllers::note_api(v);
            break;
        }
    }
    if (XR_FAILED(r)) {
        set_state("no_instance");
        return 0;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(inst, &ip))) {
        std::snprintf(g_runtime, sizeof(g_runtime), "%s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                      XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
        log::info("[xr] runtime: %s", g_runtime);
    }
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(inst, &sgi, &sys);
    log::info("[xr] xrGetSystem -> %s", result_name(inst, r));
    if (XR_FAILED(r)) {
        set_state("no_system");
        return 0;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    uint32_t max_sw = 0, max_sh = 0;  // the largest swapchain image (0: not given)
    if (XR_SUCCEEDED(xrGetSystemProperties(inst, sys, &sp))) {
        g_max_layers = sp.graphicsProperties.maxLayerCount;
        max_sw = sp.graphicsProperties.maxSwapchainImageWidth;
        max_sh = sp.graphicsProperties.maxSwapchainImageHeight;
        log::info("[xr] system: %s (vendor %u), up to %u layers, swapchain images up to %ux%u", sp.systemName, sp.vendorId,
                  sp.graphicsProperties.maxLayerCount, max_sw, max_sh);
    }
    {  // the runtime's recommended eye image ([XR] EyeShape sizes the eyes from it) and the largest image it takes
        XrViewConfigurationView vv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t nvv = 0;
        r = xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nvv, vv);
        auto least = [](uint32_t a, uint32_t b) { return !a ? b : !b ? a : a < b ? a : b; };  // 0: not given
        if (XR_SUCCEEDED(r) && nvv == 2) {
            for (int i = 0; i < 2; ++i)
                log::info("[xr] %s eye image: recommended %ux%u (max %ux%u), %u samples", i ? "right" : "left", vv[i].recommendedImageRectWidth,
                          vv[i].recommendedImageRectHeight, vv[i].maxImageRectWidth, vv[i].maxImageRectHeight, vv[i].recommendedSwapchainSampleCount);
            auto big = [](uint32_t a, uint32_t b) { return a > b ? a : b; };
            const uint32_t rec_w = big(vv[0].recommendedImageRectWidth, vv[1].recommendedImageRectWidth),
                           rec_h = big(vv[0].recommendedImageRectHeight, vv[1].recommendedImageRectHeight),
                           view_w = big(vv[0].maxImageRectWidth, vv[1].maxImageRectWidth),
                           view_h = big(vv[0].maxImageRectHeight, vv[1].maxImageRectHeight);
            const uint32_t cap_w = least(max_sw, view_w), cap_h = least(max_sh, view_h);
            g_cap_w = cap_w;
            g_cap_h = cap_h;
            log::info("[xr] the largest image: %ux%u (swapchain images up to %ux%u, the views' maxImageRect %ux%u); recommended %ux%u", cap_w,
                      cap_h, max_sw, max_sh, view_w, view_h, rec_w, rec_h);
            eye_shape::set_recommended(rec_w, rec_h, cap_w, cap_h);
            render_res::record_runtime(g_runtime, rec_w, rec_h, cap_w, cap_h);
        } else {
            g_cap_w = max_sw;
            g_cap_h = max_sh;
            log::warn("[xr] xrEnumerateViewConfigurationViews -> %s (%u views): no recommended eye size; the largest image %ux%u (the "
                      "swapchain's)",
                      result_name(inst, r), nvv, max_sw, max_sh);
        }
    }

    PFN_xrGetD3D12GraphicsRequirementsKHR get_req = nullptr;
    xrGetInstanceProcAddr(inst, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get_req));
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (get_req) {
        r = get_req(inst, sys, &req);
        log::info("[xr] D3D12 requirements -> %s (adapter LUID %08lx:%08lx, min feature level %#x)", result_name(inst, r),
                  req.adapterLuid.HighPart, req.adapterLuid.LowPart, static_cast<unsigned>(req.minFeatureLevel));
    }
    ID3D12Device* dev = state::device.load();
    ID3D12CommandQueue* queue = state::present_queue.load();
    LUID game_luid = dev->GetAdapterLuid();
    if (std::memcmp(&game_luid, &req.adapterLuid, sizeof(LUID)) != 0) {
        log::warn("[xr] the game's adapter %08lx:%08lx is not the runtime's adapter", game_luid.HighPart, game_luid.LowPart);
    }
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = dev;
    binding.queue = queue;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = sys;
    XrSession session = XR_NULL_HANDLE;
    r = xrCreateSession(inst, &sci, &session);
    log::info("[xr] xrCreateSession(device %p, queue %p) -> %s", static_cast<void*>(dev), static_cast<void*>(queue), result_name(inst, r));
    if (XR_FAILED(r)) {
        set_state("no_session");
        return 0;
    }
    {
        std::lock_guard lock(g_queue_sync_mutex);
        if (g_session_queue != queue) {
            queue->AddRef();
            g_session_queue = queue;  // the old reference, if any, kept: the game's queues are never released by the mod
        }
        if (!g_to_session && FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_to_session)))) g_to_session = nullptr;
        if (!g_from_session && FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_from_session)))) g_from_session = nullptr;
        if (!g_to_session || !g_from_session) log::error("[xr] the queue-order fences could not be made: XR images unordered against a changed present queue");
    }
    set_state("created");
    controllers::create(inst, session);

    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    uint32_t nb = 0;
    XrEnvironmentBlendMode modes[8];
    if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &nb, modes)) && nb)
        blend = modes[0];
    g_inst = inst;
    g_session = session;
    g_blend = blend;
    g_submit_mode = config::get_bool("XR", "Submit", false);

    bool running = false;
    XrSpace local = XR_NULL_HANDLE;
    for (;;) {
        pump_messages(0);
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                set_state(state_name(sc->state));
                if (sc->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    r = xrBeginSession(session, &bi);
                    log::info("[xr] xrBeginSession -> %s", result_name(inst, r));
                    running = XR_SUCCEEDED(r);
                    if (running && local == XR_NULL_HANDLE) {
                        XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                        rs.poseInReferenceSpace.orientation.w = 1.0f;
                        r = xrCreateReferenceSpace(session, &rs, &local);
                        log::info("[xr] LOCAL reference space -> %s", result_name(inst, r));
                    }
                    if (running && g_submit_mode) {
                        g_space = local;
                        g_session_running = true;  // the presenting thread takes the frames from here
                        ui_layer::set_redirect(config::get_bool("XR", "UiQuad", true));
                        d3d::set_present_unsynced(config::get_bool("XR", "Pacing", true));
                        log::info("[xr] submission mode: frames on the game's presenting thread");
                    }
                } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                    {
                        std::lock_guard lock(g_frame_mutex);
                        g_session_running = false;
                        if (g_frame_open) end_open_frame();
                        d3d::set_bind_tap(d3d::kBindTapXr, nullptr);
                    }
                    xrEndSession(session);
                    running = false;
                } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                    {  // a loss may come while running (no STOPPING first): the frames end as at STOPPING
                        std::lock_guard lock(g_frame_mutex);
                        g_session_running = false;
                        if (g_frame_open) end_open_frame();
                        d3d::set_bind_tap(d3d::kBindTapXr, nullptr);
                    }
                    xrDestroySession(session);
                    xrDestroyInstance(inst);
                    log::info("[xr] session ended");
                    return 0;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                controllers::on_profile_changed();
            } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                auto* sc = reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&ev);
                if (sc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    XrTime at = sc->changeTime ? sc->changeTime : 1;
                    g_space_change_at = at;
                    log::info("[xr] the runtime recentres the LOCAL space (change time %lld)", static_cast<long long>(sc->changeTime));
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                set_state("instance_loss");
                return 0;
            }
            ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }
        // Hand the frames over between idle frames (the last call here was xrEndFrame), or take them back.
        int handover = g_submit_request.exchange(0);
        if (handover > 0 && running && !g_submit_mode.load()) {
            g_space = local;
            g_submit_mode = true;
            g_session_running = true;
            ui_layer::set_redirect(config::get_bool("XR", "UiQuad", true));
            d3d::set_present_unsynced(config::get_bool("XR", "Pacing", true));
            log::info("[xr] submission on: frames on the game's presenting thread");
        } else if (handover < 0 && g_submit_mode.load()) {
            {
                std::lock_guard lock(g_frame_mutex);
                g_session_running = false;
                if (g_frame_open) end_open_frame();
            }
            g_submit_mode = false;
            ui_layer::set_redirect(false);
            d3d::set_present_unsynced(false);
            log::info("[xr] submission off: the idle frame loop again");
        }
        if (!running || g_submit_mode.load()) {
            pump_messages(running ? 10 : 50);
            continue;
        }
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(session, &wi, &fs))) {
            Sleep(10);
            continue;
        }
        if (local != XR_NULL_HANDLE) {
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = fs.predictedDisplayTime;
            li.space = local;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t nv = 0;
            if (XR_SUCCEEDED(xrLocateViews(session, &li, &vs, 2, &nv, views)) && nv == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
                store_views(views, g_frames.load() + 1);
        }
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &bi);
        controllers::sync(local, fs.predictedDisplayTime);
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = blend;
        ei.layerCount = 0;
        r = xrEndFrame(session, &ei);
        uint64_t n = ++g_frames;
        if (XR_FAILED(r)) log::limited("xr.endframe", 8, "[xr] xrEndFrame -> %s", result_name(inst, r));
        if (n == 1 || n % 6000 == 0) log::info("[xr] idle frames %llu", static_cast<unsigned long long>(n));
    }
}

void on_frame_end(uint64_t frame) {
    if (g_session_running.load()) submit_frame_end();
    if (g_started.load() || frame < 30) return;  // let the game settle first
    if (!state::device.load() || !state::present_queue.load()) return;
    // The game replaces its swapchain, on a new direct queue, about 12.8 s after start (ENGINE-NOTES 3.2). The session
    // binds a queue for its whole life, so wait until the newest swapchain has presented 300 frames.
    if (frame < state::swapchain_frame.load() + 300) return;
    if (g_started.exchange(true)) return;
    if (!config::get_bool("Debug", "XrIdle", true)) {
        set_state("disabled");
        return;
    }
    if (HANDLE t = CreateThread(nullptr, 0, session_thread, nullptr, 0, nullptr)) {
        SetThreadDescription(t, L"RDRVR OpenXR");
        CloseHandle(t);
    }
}

}  // namespace

void init() {
    g_hud_wrist = config::get_string("XR", "Hud", "quad") == "wrist";
    std::string r = config::get_string("XR", "WristHudRect", "");
    float v[4];
    if (!r.empty() && sscanf_s(r.c_str(), "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) == 4 && v[0] < v[2] && v[1] < v[3])
        std::memcpy(g_hud_rect, v, sizeof(v));
    log::info("[xr] HUD %s", g_hud_wrist.load() ? "on the wrist (when you look at it)" : "on the floating quad");
    d3d::add_frame_end_listener(on_frame_end);
    d3d::set_frame_size_listener(on_frame_size);
}

bool frame_resized() { return g_resized.load(std::memory_order_acquire); }
void force_frame_resized() {
    if (g_sess_w.load()) stop_eyes("the test command", g_sess_w.load(), g_sess_h.load());  // only once the eyes are made
}

bool hud_on_wrist() { return g_hud_wrist.load(); }
void hud_status(char* out, size_t len) {
    std::snprintf(out, len, "hud %s, wrist swapchain %ux%u from (%d, %d), shown frames %llu, hidden frames %llu, now %s, top (%.2f %.2f %.2f)",
                  g_hud_wrist.load() ? "wrist" : "quad", g_wr_w, g_wr_h, g_wr_x0, g_wr_y0, static_cast<unsigned long long>(g_wr_frames.load()),
                  static_cast<unsigned long long>(g_wr_hidden.load()), g_wr_shown ? "shown" : "hidden", g_wr_up[0].load(), g_wr_up[1].load(),
                  g_wr_up[2].load());
}
void set_hud_on_wrist(bool on) {
    if (g_hud_wrist.exchange(on) != on) log::info("[xr] HUD %s", on ? "on the wrist (when you look at it)" : "on the floating quad");
    config::set("XR", "Hud", on ? "wrist" : "quad");
}

bool wrist_hud_shown() { return g_wr_visible.load(std::memory_order_relaxed); }

bool eye_views(EyeView out[2], uint64_t* xr_frame) {
    std::lock_guard lock(g_views_mutex);
    if (!g_views_valid) return false;
    out[0] = g_views[0];
    out[1] = g_views[1];
    if (g_handed_n && g_handed_frame != g_views_frame) g_handed_other = true;
    g_handed[0] = g_views[0];
    g_handed[1] = g_views[1];
    g_handed_frame = g_views_frame;
    ++g_handed_n;
    if (xr_frame) *xr_frame = g_views_frame;
    return true;
}
bool eye_views_peek(EyeView out[2]) {
    std::lock_guard lock(g_views_mutex);
    if (!g_views_valid) return false;
    out[0] = g_views[0];
    out[1] = g_views[1];
    return true;
}
const char* session_state() { return g_state; }
uint64_t frames() { return g_frames.load(); }
const char* runtime_name() { return g_runtime; }

void rings_status(char* out, size_t len) {
    char r[160];
    zone_rings::status_text(r, sizeof(r));
    std::snprintf(out, len, "%s, max layers %u", r, g_max_layers.load());
}

void submit_status(char* out, size_t len) {
    char blit[160];
    xr_blit::status_text(blit, sizeof(blit));
    std::snprintf(out, len, "%s, frames with the layer %llu, without %llu, copies %llu, misses %llu, first eye after overlays %llu, %ux%u | %s | poses: this frame's %llu, "
                  "another frame's %llu, no XR views %llu | late latch %llu (misses %llu, mean %.3f max %.3f deg) | ui quad %llu | "
                  "cinema %d (%llu frames) | eye swapchains %ux%u, imageRect %ux%u",
                  g_session_running.load() && g_submit.load() ? "submitting" : (g_submit_mode.load() ? "submit mode, waiting" : "off"),
                  static_cast<unsigned long long>(g_submitted.load()), static_cast<unsigned long long>(g_empty.load()),
                  static_cast<unsigned long long>(g_copies.load()), static_cast<unsigned long long>(g_misses.load()),
                  static_cast<unsigned long long>(g_late_eye_binds.load()), g_w, g_h,
                  g_blit ? blit : "raw copies", static_cast<unsigned long long>(g_pose_match.load()),
                  static_cast<unsigned long long>(g_pose_stale.load()), static_cast<unsigned long long>(g_pose_none.load()),
                  static_cast<unsigned long long>(g_latched.load()), static_cast<unsigned long long>(g_latch_misses.load()),
                  g_latched.load() ? g_latch_sum_deg.load() / static_cast<float>(g_latched.load()) : 0.0f, g_latch_max_deg.load(),
                  static_cast<unsigned long long>(g_ui_frames.load()), g_cinema.load() ? 1 : 0, static_cast<unsigned long long>(g_cinema_frames.load()),
                  g_sw, g_sh, g_rect_w.load(), g_rect_h.load());
    // the runtime's largest image, the quads' swapchains (and what they show) and the live-resize guard
    const int n = static_cast<int>(std::strlen(out));
    if (n <= 0 || static_cast<size_t>(n) + 1 >= len) return;
    const bool resized = g_resized.load(std::memory_order_acquire);
    std::snprintf(out + n, len - static_cast<size_t>(n),
                  " | cap %ux%u | ui quad %ux%u of %ux%u, wrist %ux%u of %ux%u | cinema quad %ux%u, shown %ux%u (copies %llu, resampled %llu) | "
                  "resized %d%s%s%s",
                  g_cap_w.load(), g_cap_h.load(), g_ui_w, g_ui_h, g_ui_tw, g_ui_th, g_wr_w, g_wr_h, g_wr_cw, g_wr_ch, g_quad_w, g_quad_h,
                  g_quad_rw.load(), g_quad_rh.load(), static_cast<unsigned long long>(g_cinema_copies.load()),
                  static_cast<unsigned long long>(g_cinema_resamples.load()), resized ? 1 : 0, resized ? " (" : "", resized ? g_resized_what : "",
                  resized ? ")" : g_raw_refused ? " (raw copies refused: the frame is larger than the cap)" : "");
}

bool submitting() { return g_session_running.load() && g_submit.load(); }

ID3D12CommandQueue* image_queue() {
    ID3D12CommandQueue* present = state::present_queue.load();
    ID3D12CommandQueue* session = g_session_queue;
    if (!session) return present;
    if (!present || present == session) return session;
    std::lock_guard lock(g_queue_sync_mutex);
    if (g_to_session && SUCCEEDED(present->Signal(g_to_session, ++g_to_value))) session->Wait(g_to_session, g_to_value);
    if (g_queue_syncs.fetch_add(1, std::memory_order_relaxed) == 0)
        log::info("[xr] the game's present queue %p is not the session's %p (its last swapchain made on a new queue after the session: DLSS): "
                  "XR images are written on the session's queue behind a fence on the game's",
                  static_cast<void*>(present), static_cast<void*>(session));
    return session;
}

void image_written() {
    ID3D12CommandQueue* present = state::present_queue.load();
    ID3D12CommandQueue* session = g_session_queue;
    if (!session || !present || present == session || !g_from_session) return;
    std::lock_guard lock(g_queue_sync_mutex);
    if (SUCCEEDED(session->Signal(g_from_session, ++g_from_value))) present->Wait(g_from_session, g_from_value);
}

uint64_t queue_syncs() { return g_queue_syncs.load(std::memory_order_relaxed); }

void recentre_layers() { g_relayout = true; }

void perf_reset() { g_perf_reset = true; }

void perf_status(char* out, size_t len) {
    std::lock_guard lock(g_perf_mutex);
    const Perf& p = g_perf;
    double secs = p.start_ms > 0 ? (log::now_ms() - p.start_ms) / 1000.0 : 0;
    double n = p.frames ? static_cast<double>(p.frames) : 1.0;
    uint64_t sus = 0, sn = 0;
    camera_lever::scene_time(&sus, &sn);
    const double scene_ms = sn > p.scene_n0 ? static_cast<double>(sus - p.scene_us0) / 1000.0 / static_cast<double>(sn - p.scene_n0) : 0.0;
    std::snprintf(out, len,
                  "window %.1f s: %llu XR frames (%.2f Hz, period %.3f ms), missed periods %llu (%.3f%%) in %llu late frames, "
                  "longest gap %.2f periods | mean CPU frame %.3f ms, GPU %.3f ms | scene render %.3f ms a frame",
                  secs, static_cast<unsigned long long>(p.frames), secs > 0 ? p.frames / secs : 0.0, p.period_ms,
                  static_cast<unsigned long long>(p.missed),
                  100.0 * static_cast<double>(p.missed) / static_cast<double>(p.frames + p.missed ? p.frames + p.missed : 1),
                  static_cast<unsigned long long>(p.late_frames), p.longest, p.cpu_sum / n, p.gpu_sum / n, scene_ms);
}

void late_latch() {
    static const bool enabled = config::get_bool("XR", "LateLatch", true);
    if (!enabled || !submitting()) return;
    std::lock_guard lock(g_frame_mutex);
    if (!g_frame_open || !g_session_running.load()) return;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_display_time;
    li.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView late[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t nv = 0;
    if (XR_FAILED(xrLocateViews(g_session, &li, &vs, 2, &nv, late)) || nv != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        g_latch_misses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float deg = 0;
    for (int i = 0; i < 2; ++i) {
        const XrQuaternionf& a = g_frame_views[i].pose.orientation;
        const XrQuaternionf b = quat_mul(g_noise_q, late[i].pose.orientation);  // "xr posnoise": the frame's noise kept
        float dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        deg = std::fmax(deg, 2.0f * std::acos(std::fmin(1.0f, dot)) * 57.29578f);
        g_frame_views[i].pose.orientation = b;  // rotation only: the positions stay the frame's
        g_frame_views[i].fov = late[i].fov;
    }
    store_views(g_frame_views, g_frames.load() + 1);
    g_latched.fetch_add(1, std::memory_order_relaxed);
    g_latch_sum_deg.store(g_latch_sum_deg.load() + deg, std::memory_order_relaxed);
    if (deg > g_latch_max_deg.load()) g_latch_max_deg.store(deg, std::memory_order_relaxed);
}

std::atomic<int> g_latch_hands{-1};  // [XR] LateLatchHands (-1: not read yet); "controllers latch on|off" (the session only)
void set_latch_hands(bool on) { g_latch_hands = on ? 1 : 0; }
bool latch_hands_on() {
    int v = g_latch_hands.load(std::memory_order_relaxed);
    if (v < 0) {
        v = config::get_bool("XR", "LateLatchHands", true) ? 1 : 0;
        int expected = -1;
        if (!g_latch_hands.compare_exchange_strong(expected, v)) v = expected;
    }
    return v == 1;
}
void latch_hands() {
    if (!latch_hands_on() || !submitting()) return;
    // located under the frame mutex, as late_latch's views: the session thread takes it to stop the frames before the
    // session (and its action spaces) is destroyed. The frame mutex, then the controllers' and the hands' locks:
    // submit_frame_end's order (controllers::sync)
    std::lock_guard lock(g_frame_mutex);
    if (!g_frame_open || !g_session_running.load()) return;
    controllers::relocate(g_space, g_display_time);
}

void set_rot_ramp(float deg_per_frame) {
    g_rotramp_step = deg_per_frame;
    log::info("[xr] rotation ramp %.4f deg of yaw a frame%s", deg_per_frame, deg_per_frame == 0.0f ? " (off, the turn taken off)" : "");
}

void set_pose_noise(float rot_deg, float pos_m) {
    g_noise_rot = rot_deg;
    g_noise_pos = pos_m;
    log::info("[xr] pose noise: +-%.4f deg, +-%.5f m a frame%s", rot_deg, pos_m, rot_deg == 0.0f && pos_m == 0.0f ? " (off)" : "");
}

void set_pos_ramp(float dx, float dy, float dz) {
    g_ramp_step[0] = dx;
    g_ramp_step[1] = dy;
    g_ramp_step[2] = dz;
    g_ramp_on = dx != 0.0f || dy != 0.0f || dz != 0.0f;
    log::info("[xr] position ramp (%.4f, %.4f, %.4f) m a frame%s", dx, dy, dz, g_ramp_on ? "" : " (off, the offset taken off)");
}

void set_submit(bool on) {
    g_submit_request = on ? 1 : -1;
    log::info("[xr] submission %s requested", on ? "on" : "off");
}

void set_cinema(bool on) {
    g_cinema_placed = false;  // placed again from the head pose at the next frame
    g_cinema = on;
    log::info("[xr] cinema %s", on ? "on" : "off");
}

}  // namespace rdrvr::xr
