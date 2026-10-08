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
#include <string>
#include <vector>

#include "core/aim.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/d3d_hooks.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/state.h"
#include "core/xr.h"
#include "core/xr_blit.h"

namespace rdrvr::zone_rings {
namespace {

constexpr int kCell = 256, kCells = 10;
constexpr float kEdge = kCell * 0.5f - 2.0f;  // the outer edge, px from a cell's centre (2 px kept for filtering)
enum Cell { kRingIdle, kRingIn, kRingHeld, kRingGun, kDotIdle, kDotIn, kReticle, kReticleHot, kReticleDot, kReticleDotHot };
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
                                          {1, 1, 1},       {1, 0.22f, 0.18f}};
    constexpr float kRimA = 0.6f, kFillA = 0.08f;
    float col, a;
    if (cell >= kReticleDot) {  // the dot reticle ([Hands] ReticleStyle=dot): a dot in a dark rim, bigger than the ring's
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

// One attempt: the swapchain, then the atlas copied into one acquired image (an upload buffer and a list on the
// present queue, waited on a fence) and released. The runtime keeps composing the image last released, so nothing is
// acquired again.
bool ensure(XrSession session) {
    if (g_ready) return true;
    if (g_failed || !g_pixels_ready.load(std::memory_order_acquire)) return false;
    g_failed = true;
    ID3D12Device* dev = state::device.load();
    ID3D12CommandQueue* q = xr::image_queue();  // the atlas: an XR image (filled once, waited for)
    if (!dev || !q) {
        log::error("[rings] no device or queue");
        return false;
    }
    uint32_t nf = 0;
    int64_t formats[64];
    xrEnumerateSwapchainFormats(session, 64, &nf, formats);
    g_fmt = xr_blit::pick_format(formats, nf);
    if (g_fmt == DXGI_FORMAT_UNKNOWN) {
        log::error("[rings] the runtime offers no 8-bit sRGB swapchain format");
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = static_cast<int64_t>(g_fmt);
    ci.sampleCount = 1;
    ci.width = kCell * kCells;
    ci.height = kCell;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(session, &ci, &g_sc);
    if (XR_FAILED(r)) {
        log::error("[rings] xrCreateSwapchain -> %d", static_cast<int>(r));
        g_sc = XR_NULL_HANDLE;
        return false;
    }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages(g_sc, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_sc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_sc, &ai, &index)) || index >= ni) {
        log::error("[rings] acquiring the atlas image failed");
        return false;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    XrResult wr = XR_TIMEOUT_EXPIRED;
    for (int tries = 0; tries < 10 && wr == XR_TIMEOUT_EXPIRED; ++tries) wr = xrWaitSwapchainImage(g_sc, &wi);  // a timeout is a success code
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
        const bool bgra = g_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const int w = kCell * kCells;
        for (int y = 0; y < kCell; ++y) {
            uint8_t* row = p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch;
            const uint8_t* src = &g_pixels[static_cast<size_t>(y) * w * 4];
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
        released = XR_SUCCEEDED(xrReleaseSwapchainImage(g_sc, &ri));
    }
    ok = ok && released;
    // not when the GPU may still run the list (then the list, its allocator, the fence and the 1.5 MB upload buffer
    // are left)
    if (list && (done || !submitted)) list->Release();
    if (fence && (done || !submitted)) fence->Release();
    if (alloc && (done || !submitted)) alloc->Release();
    if (up && (done || !submitted)) up->Release();
    if (!ok) {
        log::error("[rings] filling the atlas failed");
        return false;
    }
    g_ready = true;
    g_failed = false;
    log::info("[rings] atlas swapchain %dx%d format %d (%u images), filled once", kCell * kCells, kCell, static_cast<int>(g_fmt), ni);
    return true;
}

// The shortest turn from +z (a quad's front) onto n.
XrQuaternionf facing(const float n[3]) {
    const float w = 1.0f + n[2];
    if (w < 1e-4f) return {0, 1, 0, 0};
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + w * w);
    return {-n[1] / l, n[0] / l, 0.0f, w / l};
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
    for (int i = 0; i < mk.n && n < max; ++i) {
        const holster::Marker& m = mk.m[i];
        float l[3];
        if (!camera_lever::world_to_local(mk.cam, m.pos, l)) break;  // not recentred yet
        float d[3] = {eye[0] - l[0], eye[1] - l[1], eye[2] - l[2]};
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl < m.radius * 1.2f + 0.05f) continue;  // the eyes in or at it: it would fill the view
        for (float& c : d) c /= dl;
        const int cell = m.kind == holster::kDot ? (m.state == holster::kHandIn ? kDotIn : kDotIdle)
                         : m.state == holster::kHandIn ? kRingIn
                         : m.state == holster::kHeld   ? kRingHeld
                         : m.state == holster::kGunIdle ? kRingGun
                                                        : kRingIdle;
        const float s = 2.0f * m.radius * (kCell * 0.5f) / kEdge;  // the drawn outer edge at the radius
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

bool reticle_frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* out) {
    float p[3], deg = 1.2f;
    bool hot = false;
    if (!camera_lever::eyes_follow_head() || !aim::reticle_target(p, &hot, &deg)) return false;
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
