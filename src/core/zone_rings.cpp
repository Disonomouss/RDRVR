#include "core/zone_rings.h"

#include <windows.h>
#include <d3d12.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/aim.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/d3d_hooks.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/pso.h"
#include "core/state.h"
#include "core/xr.h"
#include "core/xr_blit.h"

namespace rdrvr::zone_rings {
namespace {

constexpr int kCell = 256, kCells = 11;
constexpr float kEdge = kCell * 0.5f - 2.0f;  // the outer edge, px from a cell's centre (2 px kept for filtering)
enum Cell { kRingIdle, kRingIn, kRingHeld, kRingGun, kDotIdle, kDotIn, kReticle, kReticleHot, kReticleDot, kReticleDotHot, kRingNear };
std::vector<uint8_t> g_pixels;  // RGBA8: sRGB-encoded, premultiplied (init)
std::atomic<bool> g_pixels_ready{false};
XrSwapchain g_sc = XR_NULL_HANDLE;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
bool g_ready = false, g_failed = false;  // the presenting thread's
std::atomic<uint64_t> g_frames{0}, g_layers{0}, g_reticles{0};
std::atomic<int> g_last{0};

float s2l(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float l2s(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
// how much of a pixel at distance r from the centre lies within radius x (a 1 px ramp)
float within(float r, float x) {
    const float d = r - x;
    return d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d;
}
uint8_t to_byte(float v) { return static_cast<uint8_t>(std::lround(255.0f * (v < 0 ? 0 : v > 1 ? 1 : v))); }

// A ring: a 10 px band at the edge between 2 px dark rims, a faint fill inside. A dot: a disc in a dark rim.
// White idle, green a hand in it, amber gripped there, blue a point on the gun.
void texel(int cell, float r, float out[4]) {
    static const float kCol[kCells][3] = {{1, 1, 1},       {0.30f, 1, 0.40f}, {1, 0.72f, 0.20f}, {0.35f, 0.85f, 1},
                                          {1, 1, 1},       {0.30f, 1, 0.40f}, {1, 1, 1},          {1, 0.22f, 0.18f},
                                          {1, 1, 1},       {1, 0.22f, 0.18f}, {1, 1, 1}};
    constexpr float kRimA = 0.6f, kFillA = 0.08f;
    float col, a;
    if (cell == kRingNear) {  // [Holsters] ZonesNear: a hand near: a thinner, fainter ring, no fill
        const float e = within(r, kEdge), b1 = within(r, kEdge - 2), b0 = within(r, kEdge - 8), f1 = within(r, kEdge - 10);
        const float band = b1 - b0, rims = (e - b1) + (b0 - f1);
        col = band * 0.55f;
        a = band * 0.55f + rims * kRimA * 0.5f;
    } else if (cell >= kReticleDot) {  // the dot reticle ([Hands] ReticleStyle=dot): a dot in a dark rim, bigger than the ring's
        const float dot = within(r, kEdge * 0.24f), dot_rim = within(r, kEdge * 0.34f) - dot;
        col = dot;
        a = dot + dot_rim * kRimA;
    } else if (cell >= kReticle) {  // the reticle: a thin ring and a centre dot, each in a dark rim
        const float dot = within(r, kEdge * 0.16f), dot_rim = within(r, kEdge * 0.26f) - dot;
        const float ring = within(r, kEdge * 0.80f) - within(r, kEdge * 0.68f);
        const float ring_rim = (within(r, kEdge * 0.88f) - within(r, kEdge * 0.80f)) + (within(r, kEdge * 0.68f) - within(r, kEdge * 0.60f));
        col = dot + ring;
        a = dot + ring + (dot_rim + ring_rim) * kRimA;
    } else if (cell >= kDotIdle) {
        const float disc = within(r, kEdge * 0.75f), all = within(r, kEdge);
        col = disc;
        a = disc + (all - disc) * kRimA;
    } else {
        const float e = within(r, kEdge), b1 = within(r, kEdge - 2), b0 = within(r, kEdge - 12), f1 = within(r, kEdge - 14);
        const float band = b1 - b0, rims = (e - b1) + (b0 - f1);
        col = band + f1 * kFillA;
        a = band + rims * kRimA + f1 * kFillA;
    }
    for (int k = 0; k < 3; ++k) out[k] = s2l(kCol[cell][k]) * col;
    out[3] = a;
}

// One attempt: the swapchain, then an atlas's pixels (aw x ah, RGBA8 sRGB premultiplied) copied into one acquired image
// (an upload buffer and a list on the present queue, waited on a fence) and released. The runtime keeps composing the
// image last released, so nothing is acquired again. tag: the log's ("rings"; 2026-10-29: the highlights' atlas too).
// *sc_out is left as made when a later step fails (as before), XR_NULL_HANDLE when its creation does.
bool fill_swapchain(XrSession session, const uint8_t* pixels, int aw, int ah, const char* tag, XrSwapchain* sc_out, DXGI_FORMAT* fmt_out) {
    XrSwapchain& sc = *sc_out;
    DXGI_FORMAT& fmt = *fmt_out;
    ID3D12Device* dev = state::device.load();
    ID3D12CommandQueue* q = xr::image_queue();  // the atlas: an XR image (filled once, waited for)
    if (!dev || !q) {
        log::error("[%s] no device or queue", tag);
        return false;
    }
    uint32_t nf = 0;
    int64_t formats[64];
    xrEnumerateSwapchainFormats(session, 64, &nf, formats);
    fmt = xr_blit::pick_format(formats, nf);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        log::error("[%s] the runtime offers no 8-bit sRGB swapchain format", tag);
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = static_cast<int64_t>(fmt);
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(aw);
    ci.height = static_cast<uint32_t>(ah);
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    XrResult r = pso::as_own([&] { return xrCreateSwapchain(session, &ci, &sc); });
    if (XR_FAILED(r)) {
        log::error("[%s] xrCreateSwapchain -> %d", tag, static_cast<int>(r));
        sc = XR_NULL_HANDLE;
        return false;
    }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages(sc, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(sc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(sc, &ai, &index)) || index >= ni) {
        log::error("[%s] acquiring the atlas image failed", tag);
        return false;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    XrResult wr = XR_TIMEOUT_EXPIRED;
    for (int tries = 0; tries < 10 && wr == XR_TIMEOUT_EXPIRED; ++tries) wr = xrWaitSwapchainImage(sc, &wi);  // a timeout is a success code
    const bool waited = wr == XR_SUCCESS;
    bool ok = waited;
    ID3D12Resource* dst = imgs[index].texture;
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    dev->GetCopyableFootprints(&dd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* up = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    bool done = false, submitted = false;  // the GPU finished with the upload buffer; the list went to the queue
    ok = ok &&
         SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up))) &&
         SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
         SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
         SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    uint8_t* p = nullptr;
    D3D12_RANGE none{0, 0};
    if (ok) ok = SUCCEEDED(up->Map(0, &none, reinterpret_cast<void**>(&p))) && p;
    if (ok) {
        const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const int w = aw;
        for (int y = 0; y < ah; ++y) {
            uint8_t* row = p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch;
            const uint8_t* src = &pixels[static_cast<size_t>(y) * w * 4];
            if (!bgra) {
                std::memcpy(row, src, static_cast<size_t>(w) * 4);
            } else {
                for (int x = 0; x < w; ++x) {
                    row[x * 4 + 0] = src[x * 4 + 2];
                    row[x * 4 + 1] = src[x * 4 + 1];
                    row[x * 4 + 2] = src[x * 4 + 0];
                    row[x * 4 + 3] = src[x * 4 + 3];
                }
            }
        }
        up->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dl{};
        dl.pResource = dst;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sl{};
        sl.pResource = up;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint = fp;
        list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        ok = SUCCEEDED(list->Close());
        if (ok) {
            d3d::submit_internal(q, list);
            submitted = true;
            ok = SUCCEEDED(q->Signal(fence, 1));
            HANDLE ev = ok ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
            done = ev && SUCCEEDED(fence->SetEventOnCompletion(1, ev)) && WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0;
            if (ev) CloseHandle(ev);
            ok = ok && done;
        }
    }
    bool released = false;
    if (waited) {  // a release only follows a good wait
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        released = XR_SUCCEEDED(xrReleaseSwapchainImage(sc, &ri));
    }
    ok = ok && released;
    // not when the GPU may still run the list (then the list, its allocator, the fence and the 1.5 MB upload buffer
    // are left)
    if (list && (done || !submitted)) list->Release();
    if (fence && (done || !submitted)) fence->Release();
    if (alloc && (done || !submitted)) alloc->Release();
    if (up && (done || !submitted)) up->Release();
    if (!ok) {
        log::error("[%s] filling the atlas failed", tag);
        return false;
    }
    log::info("[%s] atlas swapchain %dx%d format %d (%u images), filled once", tag, aw, ah, static_cast<int>(fmt), ni);
    return true;
}
bool ensure(XrSession session) {
    if (g_ready) return true;
    if (g_failed || !g_pixels_ready.load(std::memory_order_acquire)) return false;
    g_failed = true;
    if (!fill_swapchain(session, g_pixels.data(), kCell * kCells, kCell, "rings", &g_sc, &g_fmt)) return false;
    g_ready = true;
    g_failed = false;
    return true;
}


// The shortest turn from +z (a quad's front) onto n.
XrQuaternionf facing(const float n[3]) {
    const float w = 1.0f + n[2];
    if (w < 1e-4f) return {0, 1, 0, 0};
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + w * w);
    return {-n[1] / l, n[0] / l, 0.0f, w / l};
}

// [Holsters] InteractionHighlights (2026-10-29, GOAL-weapons-handling.md item 5): the highlights' own atlas, made on the
// presenting thread the first time a highlight is drawn (nothing of it exists with the mode rings): two shapes (a part's
// glow, an area's softer one) at kHlLevels strengths, so a layer fades by its cell (a quad layer has no alpha of its
// own). A warm glow, premultiplied with its colour above its alpha (it brightens more than it covers), whiter at the top
// levels (at reach). Composited by the runtime over the eyes' images, after the game's DLSS and TAA: no motion vectors,
// no history, so nothing of it can smear (research\run28\highlights.md 3).
constexpr int kHlCell = 128, kHlLevels = 8, kHlShapes = 2, kHlCells = kHlShapes * kHlLevels;
constexpr float kHlSpread[kHlShapes] = {2.0f, 1.3f};  // the sprite's half width against the part's (the area's) radius
std::vector<uint8_t> g_hl_pixels;
XrSwapchain g_hl_sc = XR_NULL_HANDLE;
DXGI_FORMAT g_hl_fmt = DXGI_FORMAT_UNKNOWN;
bool g_hl_ready = false, g_hl_failed = false;  // the presenting thread's
float g_hl_shown[holster::kHlCount] = {};      // each place's drawn strength (eased), the presenting thread's
float g_hl_pos[holster::kHlCount][3] = {};     // its last place (world) and size, kept while it fades out
float g_hl_alt[holster::kHlCount][3] = {};     // (round 74) the loading place's fallback point (every gun's ring), world
bool g_hl_alt_ok[holster::kHlCount] = {};
float g_hl_rad[holster::kHlCount] = {};
uint8_t g_hl_shape[holster::kHlCount] = {};
float g_hl_cam[16] = {};
double g_hl_prev_ms = 0.0;
double g_hl_onset[holster::kHlCount] = {};  // (2026-10-31, R11) when each place's glow began (0: not shown); the frame thread's
std::atomic<uint64_t> g_hl_frames{0}, g_hl_layers{0}, g_hl_drawn[holster::kHlCount] = {};
// (r246) frames a lit place was not drawn: the eyes within 8 cm of it, or the whole sprite behind both eyes (unseen: its
// layer left for another)
std::atomic<uint64_t> g_hl_cull_eye[holster::kHlCount] = {}, g_hl_cull_back[holster::kHlCount] = {};
std::atomic<int> g_hl_last{0};
struct HlDrawn {  // the last frame's layers, for the status (the tests' pictures)
    int id = -1, level = 0;
    float shown = 0.0f, size = 0.0f;
    bool eye_ok[2] = {false, false};
    float uv[2][2] = {};  // where each eye sees its centre: 0-1 across its view (left to right, top to bottom)
};
std::mutex g_hl_mx;
HlDrawn g_hl_rec[holster::kHlCount];
float g_hl_eye[3] = {};  // the last frame's eyes' centre (under g_hl_mx)
int g_hl_rec_n = 0;
// (run 29 item 2, InteractionHighlights=parts) each place's eased strength, last place and size as drawn (under g_hl_mx: body.cpp
// reads them once a body frame for the glow on the gun's surface), the last frame's views (the tests' projections), and the
// frames a gun place's sprite was left out for its surface glow
holster::Highlight g_hl_pub[holster::kHlCount] = {};
float g_hl_pub_cam[16] = {};
double g_hl_pub_ms = 0.0;
XrView g_hl_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool g_hl_views_ok = false;
std::atomic<uint64_t> g_hl_surface[holster::kHlCount] = {};
std::atomic<uint64_t> g_hl_sprite_held{0};  // (R11) the gun places' sprite frames held back at their onset
std::atomic<uint64_t> g_hl_sprite_none{0};  // (F9) the gun places' sprite frames not drawn in parts mode (no surface glow then)

void hl_texel(int shape, int level, float r, float out[4]) {  // r: 0 at the centre, 1 at the cell's edge
    const float taper = r >= 1.0f ? 0.0f : r <= 0.85f ? 1.0f : (1.0f - r) / 0.15f;
    float g;
    if (shape == 0) {  // a part: a bright core and a faint rim at the part's own size
        const float e = (r - 0.5f) / 0.07f;
        g = 0.8f * std::exp(-4.0f * r * r) + 0.3f * std::exp(-e * e);
    } else {  // an area: a soft light, fainter
        g = 0.55f * std::exp(-3.0f * r * r);
    }
    g *= taper * static_cast<float>(level) / kHlLevels;
    const float w = level > 5 ? static_cast<float>(level - 5) / 3.0f : 0.0f;  // whiter toward reach
    const float col[3] = {1.0f, 0.84f + 0.16f * w, 0.52f + 0.48f * w};
    for (int k = 0; k < 3; ++k) out[k] = s2l(col[k]) * g;
    out[3] = 0.45f * g;
}
void hl_make() {
    const int w = kHlCell * kHlCells;
    g_hl_pixels.resize(static_cast<size_t>(w) * kHlCell * 4);
    for (int y = 0; y < kHlCell; ++y)
        for (int x = 0; x < w; ++x) {
            const int cell = x / kHlCell;
            const float dx = static_cast<float>(x % kHlCell) + 0.5f - kHlCell * 0.5f, dy = static_cast<float>(y) + 0.5f - kHlCell * 0.5f;
            float t[4];
            hl_texel(cell / kHlLevels, cell % kHlLevels + 1, std::sqrt(dx * dx + dy * dy) / (kHlCell * 0.5f), t);
            uint8_t* o = &g_hl_pixels[(static_cast<size_t>(y) * w + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = to_byte(l2s(t[k]));
            o[3] = to_byte(t[3]);
        }
}
bool hl_ensure(XrSession session) {
    if (g_hl_ready) return true;
    if (g_hl_failed) return false;
    g_hl_failed = true;
    if (g_hl_pixels.empty()) hl_make();
    if (!fill_swapchain(session, g_hl_pixels.data(), kHlCell * kHlCells, kHlCell, "highlights", &g_hl_sc, &g_hl_fmt)) return false;
    g_hl_ready = true;
    g_hl_failed = false;
    return true;
}
// the point l (its space's) in view v's own axes (x right, y up, -z ahead)
void view_space(const XrView& v, const float l[3], float r[3]) {
    const XrQuaternionf& q = v.pose.orientation;
    const float d[3] = {l[0] - v.pose.position.x, l[1] - v.pose.position.y, l[2] - v.pose.position.z};
    const float qx = -q.x, qy = -q.y, qz = -q.z, qw = q.w;  // the inverse turn
    const float t[3] = {2.0f * (qy * d[2] - qz * d[1]), 2.0f * (qz * d[0] - qx * d[2]), 2.0f * (qx * d[1] - qy * d[0])};
    r[0] = d[0] + qw * t[0] + (qy * t[2] - qz * t[1]);
    r[1] = d[1] + qw * t[1] + (qz * t[0] - qx * t[2]);
    r[2] = d[2] + qw * t[2] + (qx * t[1] - qy * t[0]);
}
// where view v sees the point l (its space's): 0-1 across, left to right and top to bottom; false behind it
bool project(const XrView& v, const float l[3], float uv[2]) {
    float r[3];
    view_space(v, l, r);
    if (r[2] > -1e-4f) return false;
    const float x = r[0] / -r[2], y = r[1] / -r[2];
    const float tl = std::tan(v.fov.angleLeft), tr = std::tan(v.fov.angleRight), tu = std::tan(v.fov.angleUp), td = std::tan(v.fov.angleDown);
    if (tr - tl < 1e-6f || tu - td < 1e-6f) return false;
    uv[0] = (x - tl) / (tr - tl);
    uv[1] = (tu - y) / (tu - td);
    return true;
}

}  // namespace

void init() {
    const int w = kCell * kCells;
    g_pixels.resize(static_cast<size_t>(w) * kCell * 4);
    for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = static_cast<float>(x % kCell) + 0.5f - kCell * 0.5f, dy = static_cast<float>(y) + 0.5f - kCell * 0.5f;
            float t[4];
            texel(x / kCell, std::sqrt(dx * dx + dy * dy), t);
            uint8_t* o = &g_pixels[(static_cast<size_t>(y) * w + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = to_byte(l2s(t[k]));
            o[3] = to_byte(t[3]);
        }
    g_pixels_ready.store(true, std::memory_order_release);
}

int frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* out, int max) {
    g_last.store(0, std::memory_order_relaxed);
    if (max <= 0 || !camera_lever::eyes_follow_head()) return 0;
    holster::Markers mk;
    if (!holster::markers(&mk) || mk.n <= 0 || !ensure(session)) return 0;
    const float eye[3] = {0.5f * (views[0].pose.position.x + views[1].pose.position.x), 0.5f * (views[0].pose.position.y + views[1].pose.position.y),
                          0.5f * (views[0].pose.position.z + views[1].pose.position.z)};
    int n = 0;
    const float ws = camera_lever::world_scale();  // a ring's size in the headset's metres (world_to_local's positions)
    for (int i = 0; i < mk.n && n < max; ++i) {
        const holster::Marker& m = mk.m[i];
        const float radius = m.radius * ws;
        float l[3];
        if (!camera_lever::world_to_local(mk.cam, m.pos, l)) break;  // not recentred yet
        float d[3] = {eye[0] - l[0], eye[1] - l[1], eye[2] - l[2]};
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl < radius * 1.2f + 0.05f) continue;  // the eyes in or at it: it would fill the view
        for (float& c : d) c /= dl;
        const int cell = m.kind == holster::kDot ? (m.state == holster::kHandIn ? kDotIn : kDotIdle)
                         : m.state == holster::kHandIn ? kRingIn
                         : m.state == holster::kHeld   ? kRingHeld
                         : m.state == holster::kGunIdle ? kRingGun
                         : m.state == holster::kNear    ? kRingNear
                                                        : kRingIdle;
        const float s = 2.0f * radius * (kCell * 0.5f) / kEdge;  // the drawn outer edge at the radius
        XrCompositionLayerQuad& q = out[n++];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied, like the UI quad
        q.space = space;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = g_sc;
        q.subImage.imageRect = {{cell * kCell, 0}, {kCell, kCell}};
        q.pose.orientation = facing(d);
        q.pose.position = {l[0], l[1], l[2]};
        q.size = {s, s};
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_layers.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    g_last.store(n, std::memory_order_relaxed);
    return n;
}

bool reticle_frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* out, int which) {
    float p[3], deg = 1.2f;
    bool hot = false;
    if (!camera_lever::eyes_follow_head() || !aim::reticle_target(p, &hot, &deg, which, true)) return false;
    body::BodyPoints bp;
    if (!body::body_points(&bp) || !bp.cam_ok || !ensure(session)) return false;
    float l[3];
    if (!camera_lever::world_to_local(bp.cam, p, l)) return false;
    const float eye[3] = {0.5f * (views[0].pose.position.x + views[1].pose.position.x), 0.5f * (views[0].pose.position.y + views[1].pose.position.y),
                          0.5f * (views[0].pose.position.z + views[1].pose.position.z)};
    float d[3] = {eye[0] - l[0], eye[1] - l[1], eye[2] - l[2]};
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl < 0.4f) return false;  // at the muzzle: nothing to show
    for (float& c : d) c /= dl;
    // a constant angle: the drawn ring's outer edge (0.88 of the cell's edge) spans `deg` degrees at its distance
    const float across = 2.0f * dl * std::tan(0.5f * deg * 0.0174532925f);
    const float s = across * (kCell * 0.5f) / (kEdge * 0.88f);
    XrCompositionLayerQuad& q = *out;
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_sc;
    const int cell = aim::reticle_dot() ? (hot ? kReticleDotHot : kReticleDot) : (hot ? kReticleHot : kReticle);
    q.subImage.imageRect = {{cell * kCell, 0}, {kCell, kCell}};
    q.pose.orientation = facing(d);
    q.pose.position = {l[0], l[1], l[2]};
    q.size = {s, s};
    g_reticles.fetch_add(1, std::memory_order_relaxed);
    return true;
}

int highlights_frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* out, int max) {
    g_hl_last.store(0, std::memory_order_relaxed);
    if (holster::interaction_highlights() == 0) {  // the default: nothing drawn, nothing made
        for (double& o : g_hl_onset) o = 0.0;
        if (g_hl_prev_ms > 0.0) {
            for (float& s : g_hl_shown) s = 0.0f;
            g_hl_prev_ms = 0.0;
            std::lock_guard lock(g_hl_mx);
            g_hl_rec_n = 0;
        }
        return 0;
    }
    const double now = log::now_ms();
    const float dt = g_hl_prev_ms > 0.0 ? static_cast<float>(std::fmin(0.1, std::fmax(0.0, (now - g_hl_prev_ms) * 0.001))) : 0.0f;
    g_hl_prev_ms = now;
    holster::Highlights hl{};
    const bool have = camera_lever::eyes_follow_head() && holster::highlights(&hl);
    float target[holster::kHlCount] = {};
    if (have) {
        std::memcpy(g_hl_cam, hl.cam, sizeof(g_hl_cam));
        for (int i = 0; i < hl.n; ++i) {
            const holster::Highlight& e = hl.h[i];
            if (e.id < 0 || e.id >= holster::kHlCount) continue;
            std::memcpy(g_hl_pos[e.id], e.pos, sizeof(e.pos));
            std::memcpy(g_hl_alt[e.id], e.alt, sizeof(e.alt));
            g_hl_alt_ok[e.id] = e.alt_ok;
            g_hl_rad[e.id] = e.radius;
            g_hl_shape[e.id] = e.shape < kHlShapes ? e.shape : 0;
            target[e.id] = e.target < 0.0f ? -1.0f : std::fmin(1.0f, e.target);  // (C17b: below 0, off at once)
        }
    }
    // in over 0.12 s, out over 0.2 s; none at once when the places are not known (out of first person, a cutscene)
    for (int i = 0; i < holster::kHlCount; ++i) {
        float& s = g_hl_shown[i];
        if (!have || target[i] < 0.0f) s = 0.0f;
        else s = target[i] > s ? std::fmin(target[i], s + dt / 0.12f) : std::fmax(target[i], s - dt / 0.20f);
        // (the review) each place's onset, every frame (not only those the draw reaches): out of first person, a cutscene
        // or the menu ends it
        const int level = static_cast<int>(std::ceil(s * kHlLevels - 0.25f));
        if (level < 1) g_hl_onset[i] = 0.0;
        else if (g_hl_onset[i] <= 0.0) g_hl_onset[i] = log::now_ms();
    }
    const bool parts = holster::interaction_highlights() == 3;
    {  // (run 29 item 2) published for the glow on the gun's surface (body.cpp), and the views for the tests' projections
        std::lock_guard lock(g_hl_mx);
        for (int i = 0; i < holster::kHlCount; ++i) {
            holster::Highlight& e = g_hl_pub[i];
            std::memcpy(e.pos, g_hl_pos[i], sizeof(e.pos));
            e.radius = g_hl_rad[i];
            e.target = g_hl_shown[i];
            e.shape = g_hl_shape[i];
            e.id = static_cast<int8_t>(i);
            std::memcpy(e.alt, g_hl_alt[i], sizeof(e.alt));
            e.alt_ok = g_hl_alt_ok[i];
        }
        std::memcpy(g_hl_pub_cam, g_hl_cam, sizeof(g_hl_pub_cam));
        g_hl_pub_ms = have ? now : 0.0;
        g_hl_views[0] = views[0];
        g_hl_views[1] = views[1];
        g_hl_views_ok = true;
    }
    int n = 0;
    HlDrawn rec[holster::kHlCount];
    if (have && max > 0) {
        const float eye[3] = {0.5f * (views[0].pose.position.x + views[1].pose.position.x), 0.5f * (views[0].pose.position.y + views[1].pose.position.y),
                              0.5f * (views[0].pose.position.z + views[1].pose.position.z)};
        {  // (the status: the eyes' centre, the views' space, for the tests to aim the head)
            std::lock_guard lock(g_hl_mx);
            std::memcpy(g_hl_eye, eye, sizeof(g_hl_eye));
        }
        const float ws = camera_lever::world_scale();
        const double now_ms = log::now_ms();
        for (int i = 0; i < holster::kHlCount && n < max; ++i) {
            const int level = static_cast<int>(std::ceil(g_hl_shown[i] * kHlLevels - 0.25f));  // (a trace under a quarter step: none)
            if (level < 1) continue;
            // (run 29 item 2) parts: a gun's place glows on the gun's own surface instead, while that glow is drawn (recorded for
            // this place within 100 ms and played within 250 ms: pso::glow::live(i)); else its sprite, as highlights
            if (parts && holster::highlight_gun(i) >= 0 && body::part_glow_live(i) && pso::glow::live(i)) {
                g_hl_surface[i].fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // (2026-10-31, the off-hand build step 7, R11; round 61, the user: "some guns still have old reload point glow"; r80
            // G2: the gold sprite a frame at each place's onset, before its surface glow is recorded) a gun's place in parts
            // mode: no sprite for its first kSpriteHoldMs (the surface glow's first frames); still none: the sprite, as before
            constexpr double kSpriteHoldMs = 150.0;
            if (parts && holster::highlight_gun(i) >= 0 && now_ms - g_hl_onset[i] < kSpriteHoldMs) {
                g_hl_sprite_held.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // (2026-11-01, GOAL-offhand-fixes F9; round 63, the user: "the old glow" still on long guns held by the left hand,
            // and on some right-hand guns for a moment after the let-go) a gun's action and loading places in parts mode glow
            // on the gun's surface only: no sprite when that glow is not drawn (the mirrored gun's places, a gun put away while
            // its glow fades out). The foregrip's ring keeps its sprite
            if (parts && i != holster::kHlFore && holster::highlight_gun(i) >= 0) {
                g_hl_sprite_none.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (n == 0 && !hl_ensure(session)) break;
            float l[3];
            if (!camera_lever::world_to_local(g_hl_cam, g_hl_pos[i], l)) break;
            const float half = g_hl_rad[i] * kHlSpread[g_hl_shape[i]] * ws;  // the sprite's half width (headset metres)
            // (never a quad at a non-finite place or size: holster.cpp drops those already; the runtime must not get one)
            if (!std::isfinite(l[0]) || !std::isfinite(l[1]) || !std::isfinite(l[2]) || !std::isfinite(half) || !(half > 0.0f)) continue;
            float d[3] = {eye[0] - l[0], eye[1] - l[1], eye[2] - l[2]};
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            // (r246) the eyes near it (the shoulders' 25 cm zones beside the head): drawn smaller, its edge 5 cm short of the
            // eyes, rather than not at all (it was culled within its half width + 5 cm); none within 8 cm
            const float fit = dl - 0.05f;
            if (!(fit >= 0.03f)) {  // (a NaN fails closed)
                g_hl_cull_eye[i].fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const float hw = half < fit ? half : fit;
            {  // the whole sprite behind both eyes: unseen, its layer left for another
                float r0[3], r1[3];
                view_space(views[0], l, r0);
                view_space(views[1], l, r1);
                if (r0[2] > hw && r1[2] > hw) {
                    g_hl_cull_back[i].fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }
            for (float& c : d) c /= dl;
            const int cell = g_hl_shape[i] * kHlLevels + (level > kHlLevels ? kHlLevels : level) - 1;
            XrCompositionLayerQuad& q = out[n];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
            q.space = space;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = g_hl_sc;
            q.subImage.imageRect = {{cell * kHlCell, 0}, {kHlCell, kHlCell}};
            q.pose.orientation = facing(d);
            q.pose.position = {l[0], l[1], l[2]};
            q.size = {2.0f * hw, 2.0f * hw};
            HlDrawn& r = rec[n];
            r.id = i;
            r.level = level > kHlLevels ? kHlLevels : level;
            r.shown = g_hl_shown[i];
            r.size = 2.0f * hw;
            for (int e = 0; e < 2; ++e) r.eye_ok[e] = project(views[e], l, r.uv[e]);
            g_hl_drawn[i].fetch_add(1, std::memory_order_relaxed);
            ++n;
        }
    }
    {
        std::lock_guard lock(g_hl_mx);
        for (int i = 0; i < n; ++i) g_hl_rec[i] = rec[i];
        g_hl_rec_n = n;
    }
    g_hl_frames.fetch_add(1, std::memory_order_relaxed);
    g_hl_layers.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    g_hl_last.store(n, std::memory_order_relaxed);
    return n;
}

void highlights_status(char* out, size_t len) {
    int k = std::snprintf(out, len, "highlight layers: atlas %s, frames %llu, layers %llu (last %d)",
                          g_hl_ready ? "made" : g_hl_failed ? "failed" : "not made", static_cast<unsigned long long>(g_hl_frames.load()),
                          static_cast<unsigned long long>(g_hl_layers.load()), g_hl_last.load());
    if (k < 0 || static_cast<size_t>(k) >= len) return;
    k += std::snprintf(out + k, len - k, " | drawn frames");
    for (int i = 0; i < holster::kHlCount && k > 0 && static_cast<size_t>(k) < len; ++i) {
        const uint64_t c = g_hl_drawn[i].load(std::memory_order_relaxed);
        if (c) k += std::snprintf(out + k, len - k, " %s %llu", holster::highlight_name(i), static_cast<unsigned long long>(c));
    }
    if (k > 0 && static_cast<size_t>(k) < len) k += std::snprintf(out + k, len - k, " | on the surface instead (parts)");
    for (int i = 0; i < holster::kHlCount && k > 0 && static_cast<size_t>(k) < len; ++i) {
        const uint64_t c = g_hl_surface[i].load(std::memory_order_relaxed);
        if (c) k += std::snprintf(out + k, len - k, " %s %llu", holster::highlight_name(i), static_cast<unsigned long long>(c));
    }
    if (k > 0 && static_cast<size_t>(k) < len)  // (R11)
        k += std::snprintf(out + k, len - k, " | sprites held at onset %llu, none (no surface glow) %llu", static_cast<unsigned long long>(g_hl_sprite_held.load(std::memory_order_relaxed)),
                           static_cast<unsigned long long>(g_hl_sprite_none.load(std::memory_order_relaxed)));
    if (k > 0 && static_cast<size_t>(k) < len) k += std::snprintf(out + k, len - k, " | culled eyes/behind");
    for (int i = 0; i < holster::kHlCount && k > 0 && static_cast<size_t>(k) < len; ++i) {
        const uint64_t ce = g_hl_cull_eye[i].load(std::memory_order_relaxed), cb = g_hl_cull_back[i].load(std::memory_order_relaxed);
        if (ce || cb)
            k += std::snprintf(out + k, len - k, " %s %llu/%llu", holster::highlight_name(i), static_cast<unsigned long long>(ce),
                               static_cast<unsigned long long>(cb));
    }
    std::lock_guard lock(g_hl_mx);
    if (k > 0 && static_cast<size_t>(k) < len) k += std::snprintf(out + k, len - k, " | eyes (%.3f %.3f %.3f)", g_hl_eye[0], g_hl_eye[1], g_hl_eye[2]);
    for (int i = 0; i < g_hl_rec_n && k > 0 && static_cast<size_t>(k) < len; ++i) {
        const HlDrawn& r = g_hl_rec[i];
        k += std::snprintf(out + k, len - k, " | layer %s level %d shown %.2f size %.3f L %s(%.4f %.4f) R %s(%.4f %.4f)", holster::highlight_name(r.id), r.level,
                           r.shown, r.size, r.eye_ok[0] ? "" : "behind ", r.uv[0][0], r.uv[0][1], r.eye_ok[1] ? "" : "behind ", r.uv[1][0], r.uv[1][1]);
    }
}

bool highlights_shown(holster::Highlight out[holster::kHlCount], float cam[16]) {
    std::lock_guard lock(g_hl_mx);
    if (g_hl_pub_ms <= 0.0 || log::now_ms() - g_hl_pub_ms > 100.0) return false;
    for (int i = 0; i < holster::kHlCount; ++i) out[i] = g_hl_pub[i];
    std::memcpy(cam, g_hl_pub_cam, sizeof(g_hl_pub_cam));
    return true;
}
bool project_world(const float cam[16], const float w[3], int eye, float uv[2]) {
    float l[3];
    if (eye < 0 || eye > 1 || !camera_lever::world_to_local(cam, w, l)) return false;
    XrView v;
    {
        std::lock_guard lock(g_hl_mx);
        if (!g_hl_views_ok) return false;
        v = g_hl_views[eye];
    }
    return project(v, l, uv);
}

std::string write_atlas(const std::string& path) {
    if (!g_pixels_ready.load(std::memory_order_acquire)) return "ERROR the atlas is not made";
    const int w = kCell * kCells, h = kCell;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return "ERROR cannot write " + path;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w * h * 4);
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {  // RGBA to BGRA
            const uint8_t* p = &g_pixels[(static_cast<size_t>(y) * w + x) * 4];
            uint8_t* o = &row[static_cast<size_t>(x) * 4];
            o[0] = p[2];
            o[1] = p[1];
            o[2] = p[0];
            o[3] = p[3];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return "wrote " + path + " " + std::to_string(w) + "x" + std::to_string(h) + " (" + std::to_string(kCells) + " cells)";
}

void status_text(char* out, size_t len) {
    std::snprintf(out, len, "reticle frames %llu | rings %s, frames %llu, layers %llu (last %d)", static_cast<unsigned long long>(g_reticles.load()),
                  g_ready ? "made" : g_failed ? "failed" : "not made", static_cast<unsigned long long>(g_frames.load()),
                  static_cast<unsigned long long>(g_layers.load()), g_last.load());
}

}  // namespace rdrvr::zone_rings
