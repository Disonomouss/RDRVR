#include "core/frame_grab.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/state.h"

namespace rdrvr::frame_grab {
namespace {

std::mutex g_mutex;              // one request at a time
std::string g_path, g_result;
std::atomic<bool> g_pending{false};
HANDLE g_done = nullptr;

ID3D12Device* g_dev = nullptr;
ID3D12CommandAllocator* g_alloc = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_value = 0;
ID3D12Resource* g_readback = nullptr;
uint64_t g_readback_size = 0;
HANDLE g_fence_event = nullptr;

bool ensure_objects(ID3D12Device* dev, uint64_t bytes) {
    if (g_dev != dev) {  // a new device: start over (the old objects belong to a device that is gone)
        g_dev = dev;
        g_alloc = nullptr;
        g_list = nullptr;
        g_fence = nullptr;
        g_readback = nullptr;
        g_readback_size = 0;
    }
    if (!g_alloc && FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)))) return false;
    if (!g_list) {
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list)))) return false;
        g_list->Close();
    }
    if (!g_fence && FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_readback_size < bytes) {
        if (g_readback) g_readback->Release();
        g_readback = nullptr;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&g_readback))))
            return false;
        g_readback_size = bytes;
    }
    return true;
}

bool is_bgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}
bool is_rgba8(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

std::string write_bmp(const std::string& path, const uint8_t* src, uint32_t w, uint32_t h, uint64_t pitch, bool bgra) {
    std::wstring wpath(path.begin(), path.end());
    if (wpath.find(L':') == std::wstring::npos && !(wpath.size() > 1 && wpath[0] == L'\\')) {
        wchar_t buf[MAX_PATH];
        wpath = log::path_in_game_dir(wpath.c_str(), buf, MAX_PATH);
    }
    std::wstring tmp = wpath + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return "ERROR cannot open " + path;
    uint32_t row = w * 4, image = row * h;
    uint8_t fh[14] = {'B', 'M'};
    uint32_t file_size = 14 + 40 + image, offset = 14 + 40;
    std::memcpy(fh + 2, &file_size, 4);
    std::memcpy(fh + 10, &offset, 4);
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);  // bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = image;
    fwrite(fh, 1, sizeof(fh), f);
    fwrite(&ih, 1, sizeof(ih), f);
    std::vector<uint8_t> line(row);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = src + static_cast<uint64_t>(h - 1 - y) * pitch;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = s + x * 4;
            uint8_t* q = line.data() + x * 4;
            if (bgra) { q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; }
            else { q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; }
            q[3] = 0xff;
        }
        fwrite(line.data(), 1, row, f);
    }
    bool ok = fclose(f) == 0;
    if (!ok || !MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) return "ERROR writing " + path;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "wrote %s %ux%u", path.c_str(), w, h);
    return buf;
}

std::string capture() {
    IDXGISwapChain* sc = state::swapchain.load();
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!sc || !q) return "ERROR no swapchain";
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return "ERROR no IDXGISwapChain3";
    UINT index = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    HRESULT hr = sc3->GetBuffer(index, IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) return "ERROR GetBuffer failed";
    std::string result;
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    ID3D12Device* dev = nullptr;
    bb->GetDevice(IID_PPV_ARGS(&dev));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    if (dev) dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    if (!dev || (!is_bgra(d.Format) && !is_rgba8(d.Format))) {
        result = "ERROR unsupported back buffer format " + std::to_string(static_cast<int>(d.Format));
    } else if (!ensure_objects(dev, total) || FAILED(g_alloc->Reset()) || FAILED(g_list->Reset(g_alloc, nullptr))) {
        result = "ERROR could not create the readback objects";
    } else {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = g_readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        g_list->ResourceBarrier(1, &b);
        g_list->Close();
        d3d::submit_internal(q, g_list);
        q->Signal(g_fence, ++g_fence_value);
        g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
        if (WaitForSingleObject(g_fence_event, 2000) != WAIT_OBJECT_0) {
            result = "ERROR GPU copy timed out";
        } else {
            uint8_t* p = nullptr;
            D3D12_RANGE r{0, static_cast<SIZE_T>(total)};
            if (FAILED(g_readback->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) {
                result = "ERROR Map failed";
            } else {
                result = write_bmp(g_path, p + fp.Offset, static_cast<uint32_t>(d.Width), d.Height, fp.Footprint.RowPitch,
                                   is_bgra(d.Format));
                D3D12_RANGE none{0, 0};
                g_readback->Unmap(0, &none);
            }
        }
    }
    if (dev) dev->Release();
    bb->Release();
    char tail[96];
    std::snprintf(tail, sizeof(tail), " (frame %llu)", static_cast<unsigned long long>(state::presents.load() + 1));
    return result + tail;
}

// ---- depth: piggybacks the game's own CopyResource("Depth Resolve" <- scene depth), see d3d::set_copy_tap. The scene
// depth is in COPY_SOURCE there, so our copy needs no barrier on any game resource.
std::atomic<int> g_depth_phase{0};  // 0 idle, 1 armed, 2 copy recorded in the game's list
std::atomic<const void*> g_depth_dst{nullptr};
std::atomic<uint64_t> g_depth_frame{0};
ID3D12Resource* g_depth_rb = nullptr;
uint64_t g_depth_rb_size = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_depth_fp{};
uint32_t g_depth_w = 0, g_depth_h = 0, g_depth_fmt = 0;
std::string g_depth_path, g_depth_result;
HANDLE g_depth_done = nullptr;

void copy_tap(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    if (g_depth_phase.load(std::memory_order_acquire) != 1 || dst != g_depth_dst.load()) return;
    D3D12_RESOURCE_DESC d = src->GetDesc();
    ID3D12Device* dev = nullptr;
    if (FAILED(src->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);  // subresource 0 = the depth plane
    if (!g_depth_rb || g_depth_rb_size < total) {
        if (g_depth_rb) g_depth_rb->Release();
        g_depth_rb = nullptr;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = total;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&g_depth_rb)))) {
            dev->Release();
            return;
        }
        g_depth_rb_size = total;
    }
    dev->Release();
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = g_depth_rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = fp;
    sl.pResource = src;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    g_depth_fp = fp;
    g_depth_w = static_cast<uint32_t>(d.Width);
    g_depth_h = d.Height;
    g_depth_fmt = static_cast<uint32_t>(fp.Footprint.Format);
    g_depth_frame = state::presents.load() + 1;
    g_depth_phase.store(2, std::memory_order_release);
}

// "RDZ1", width, height, footprint format, then width x height uint32 (top row first): the raw depth texels.
std::string finish_depth() {
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q || !g_fence) {
        ID3D12Device* dev = nullptr;
        if (q && SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
            dev->Release();
        }
        if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!q || !g_fence) return "ERROR no queue/fence for the depth grab";
    }
    q->Signal(g_fence, ++g_fence_value);
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    if (WaitForSingleObject(g_fence_event, 2000) != WAIT_OBJECT_0) return "ERROR depth copy timed out";
    uint8_t* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(g_depth_rb_size)};
    if (FAILED(g_depth_rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) return "ERROR depth Map failed";
    std::wstring wpath(g_depth_path.begin(), g_depth_path.end());
    std::wstring tmp = wpath + L".tmp";
    FILE* f = nullptr;
    std::string result;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) {
        result = "ERROR cannot open " + g_depth_path;
    } else {
        const uint32_t hdr[4] = {0x315a4452u /* RDZ1 */, g_depth_w, g_depth_h, g_depth_fmt};
        fwrite(hdr, 1, sizeof(hdr), f);
        for (uint32_t y = 0; y < g_depth_h; ++y)
            fwrite(p + g_depth_fp.Offset + static_cast<uint64_t>(y) * g_depth_fp.Footprint.RowPitch, 4, g_depth_w, f);
        bool ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s %s %ux%u footprint format %u", ok ? "wrote" : "ERROR writing", g_depth_path.c_str(),
                      g_depth_w, g_depth_h, g_depth_fmt);
        result = buf;
    }
    D3D12_RANGE none{0, 0};
    g_depth_rb->Unmap(0, &none);
    return result;
}

void on_frame_end(uint64_t) {
    if (g_depth_phase.load(std::memory_order_acquire) == 2 && state::presents.load() + 1 > g_depth_frame.load()) {
        g_depth_result = finish_depth();  // one frame later: the game's list with our copy has been submitted
        g_depth_phase = 0;
        SetEvent(g_depth_done);
    }
    if (!g_pending.load(std::memory_order_acquire)) return;
    g_result = capture();
    g_pending = false;
    SetEvent(g_done);
}

}  // namespace

void init() {
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_depth_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d3d::add_frame_end_listener(on_frame_end);
    d3d::set_copy_tap(copy_tap);
}

std::string grab_depth(const std::string& path, unsigned timeout_ms) {
    std::lock_guard lock(g_mutex);
    const void* dst = d3d::find_resource("Depth Resolve");
    if (!dst) return "ERROR no resource named \"Depth Resolve\" yet";
    g_depth_path = path;
    g_depth_dst = dst;
    ResetEvent(g_depth_done);
    g_depth_phase.store(1, std::memory_order_release);
    if (WaitForSingleObject(g_depth_done, timeout_ms) != WAIT_OBJECT_0) {
        int expected = 1;
        g_depth_phase.compare_exchange_strong(expected, 0);  // never armed into a copy: disarm; a recorded copy finishes later
        return "ERROR the game's depth copy did not happen within the timeout";
    }
    log::info("[grab] %s", g_depth_result.c_str());
    return g_depth_result;
}

std::string grab(const std::string& path, unsigned timeout_ms) {
    std::lock_guard lock(g_mutex);
    g_path = path;
    ResetEvent(g_done);
    g_pending.store(true, std::memory_order_release);
    if (WaitForSingleObject(g_done, timeout_ms) != WAIT_OBJECT_0) {
        g_pending = false;
        return "ERROR no frame presented within the timeout";
    }
    log::info("[grab] %s", g_result.c_str());
    return g_result;
}

}  // namespace rdrvr::frame_grab
