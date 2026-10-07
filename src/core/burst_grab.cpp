#include "core/burst_grab.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/post_target.h"
#include "core/state.h"

namespace rdrvr::burst_grab {
namespace {

// The post output target: the Post FXAA Target under FXAA, FXAATarget otherwise (post_target.h).
constexpr int kMaxFrames = 24;
const char* const kEyeTag[3] = {"M", "L", "R"};

struct Slot {
    ID3D12Resource* rb = nullptr;  // one footprint per frame; never released (a late tap may still copy into it)
    uint64_t rb_size = 0;
    bool valid[kMaxFrames] = {};   // recording thread writes, presenting thread reads after the phase handshake
    int recorded = 0;
};
Slot g_slot[3];  // 0 mono, 1 left, 2 right

std::mutex g_mutex;
std::string g_prefix, g_result;
HANDLE g_done = nullptr;
// 0 idle, 1 armed, 2 waiting for a UI bind, 3 recording, 4 recorded (one more frame end), 5 finish
std::atomic<int> g_phase{0};
int g_frames = 8, g_frames_waited = 0;
int g_frame_i = 0, g_fxaa_binds = 0, g_doubles = 0, g_monos = 0;  // recording thread
bool g_swap = false;
ID3D12Resource* g_res = nullptr;
uint64_t g_rtv[8];
int g_nrtv = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp{};
UINT64 g_row_bytes = 0, g_frame_bytes = 0;
D3D12_BOX g_box{};
uint32_t g_w = 0, g_h = 0, g_fmt = 0;
uint64_t g_first_frame = 0;
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_value = 0;
HANDLE g_fence_event = nullptr;

void record(ID3D12GraphicsCommandList* cl, int eye) {
    Slot& s = g_slot[eye];
    if (g_frame_i >= g_frames || s.valid[g_frame_i]) return;
    ID3D12GraphicsCommandList* wcl = nullptr;
    uint32_t st = 0;
    if (!d3d::watched_state(g_res, &wcl, &st) || wcl != cl) return;  // this frame's copy is not measured
    const auto state = static_cast<D3D12_RESOURCE_STATES>(st);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (transition) cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = s.rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = g_fp;
    dl.PlacedFootprint.Offset = static_cast<UINT64>(g_frame_i) * g_frame_bytes;
    sl.pResource = g_res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, &g_box);
    if (transition) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
    }
    s.valid[g_frame_i] = true;
    ++s.recorded;
}

void bind_tap(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int,
              const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    int ph = g_phase.load(std::memory_order_acquire);
    if ((ph != 2 && ph != 3) || n == 0 || !rts) return;
    bool target = false;
    for (int i = 0; i < g_nrtv; ++i) target = target || rts[0].ptr == g_rtv[i];
    if (!target) return;
    bool ui = ds != nullptr;
    if (ph == 2) {
        if (ui) {
            g_fxaa_binds = g_frame_i = g_doubles = g_monos = 0;
            g_phase.store(3, std::memory_order_release);
        }
        return;
    }
    if (!ui) {
        if (++g_fxaa_binds == 2) record(cl, g_swap ? 2 : 1);  // the first pass's eye
        return;
    }
    if (g_fxaa_binds >= 2) {
        record(cl, g_swap ? 1 : 2);  // the last pass's eye
        ++g_doubles;
    } else if (g_fxaa_binds == 1) {
        record(cl, 0);
        ++g_monos;
    }
    g_fxaa_binds = 0;
    if (++g_frame_i >= g_frames) g_phase.store(4, std::memory_order_release);
}

std::string arm(uint64_t frame, int crop_w, int crop_h) {
    char* postfx = *reinterpret_cast<char**>(anchors::addr(anchors::Id::PostFxSingleton));
    if (!postfx) return "ERROR no PostFx object (NOT MEASURED)";
    const char* tname = post_target::name(postfx);
    void* obj = *reinterpret_cast<void**>(postfx + post_target::field(postfx));
    char where[64] = "?";
    const void* found = d3d::resource_in_object(obj, 0x100, tname, where, sizeof(where));
    if (!found) found = d3d::unique_resource(tname);
    auto* res = static_cast<ID3D12Resource*>(const_cast<void*>(found));
    if (!res) return std::string("ERROR no single live resource named \"") + tname + "\" (NOT MEASURED)";
    g_nrtv = d3d::rtv_handles_of(res, g_rtv, 8);
    if (g_nrtv == 0) return std::string("ERROR no render-target view of ") + tname + " seen (NOT MEASURED)";
    D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 || d.DepthOrArraySize != 1 ||
        d.SampleDesc.Count != 1)
        return std::string("ERROR ") + tname + " is not a single-subresource 2D texture (NOT MEASURED)";
    uint32_t w = static_cast<uint32_t>(crop_w), h = static_cast<uint32_t>(crop_h);
    if (w == 0 || h == 0 || w > d.Width || h > d.Height) return "ERROR crop larger than the target";
    g_box.left = static_cast<UINT>((d.Width - w) / 2);
    g_box.top = (d.Height - h) / 2;
    g_box.right = g_box.left + w;
    g_box.bottom = g_box.top + h;
    g_box.front = 0;
    g_box.back = 1;
    D3D12_RESOURCE_DESC cd = d;  // the crop's footprint
    cd.Width = w;
    cd.Height = h;
    ID3D12Device* dev = nullptr;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return "ERROR no device";
    UINT rows = 0;
    UINT64 total = 0;
    dev->GetCopyableFootprints(&cd, 0, 1, 0, &g_fp, &rows, &g_row_bytes, &total);
    g_frame_bytes = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    UINT64 need = g_frame_bytes * static_cast<UINT64>(g_frames);  // kept between grabs (a late tap may still copy)
    bool ok = true;
    for (Slot& s : g_slot) {
        if (!s.rb || s.rb_size < need) {
            if (s.rb) s.rb->Release();
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = need;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            s.rb = nullptr;
            dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&s.rb));
            s.rb_size = s.rb ? need : 0;
        }
        ok = ok && s.rb;
        std::memset(s.valid, 0, sizeof(s.valid));
        s.recorded = 0;
    }
    if (!g_fence) dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev->Release();
    if (!ok || !g_fence) return "ERROR could not create the readback objects";
    g_w = w;
    g_h = h;
    g_fmt = static_cast<uint32_t>(d.Format);
    g_swap = camera_lever::swap_order();
    g_first_frame = frame + 1;
    g_res = res;
    d3d::watch_add(res);
    log::info("[burst] armed at frame %llu: %u x %u crop at (%u, %u) of %s, %d frames",
              static_cast<unsigned long long>(frame), w, h, g_box.left, g_box.top, tname, g_frames);
    return {};
}

void stop(const std::string& result) {
    d3d::set_bind_tap(d3d::kBindTapBurst, nullptr);
    d3d::watch_remove(g_res);
    g_res = nullptr;
    g_result = result;
    g_phase = 0;
    SetEvent(g_done);
}

std::string finish() {
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q || !g_fence) return "ERROR no present queue";
    q->Signal(g_fence, ++g_fence_value);
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    if (WaitForSingleObject(g_fence_event, 3000) != WAIT_OBJECT_0) return "ERROR the copies timed out (NOT MEASURED)";
    std::string out;
    for (int e = 0; e < 3; ++e) {
        Slot& s = g_slot[e];
        if (!s.recorded) continue;
        uint8_t* p = nullptr;
        D3D12_RANGE r{0, static_cast<SIZE_T>(s.rb_size)};
        if (FAILED(s.rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) {
            out += std::string(" | ") + kEyeTag[e] + " Map failed";
            continue;
        }
        std::string path = g_prefix + "-" + kEyeTag[e] + ".rdb";
        std::wstring wpath(path.begin(), path.end()), tmp = wpath + L".tmp";
        FILE* f = nullptr;
        bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
        if (ok) {
            const uint32_t hdr[8] = {0x31424452u /* RDB1 */, g_w, g_h, g_fmt, static_cast<uint32_t>(e - 1),
                                     static_cast<uint32_t>(g_frames), static_cast<uint32_t>(g_first_frame),
                                     static_cast<uint32_t>(g_row_bytes)};
            fwrite(hdr, 1, sizeof(hdr), f);
            for (int i = 0; i < g_frames; ++i) {
                uint32_t v = s.valid[i] ? 1u : 0u;
                fwrite(&v, 1, 4, f);
                for (uint32_t y = 0; y < g_h; ++y)
                    fwrite(p + static_cast<UINT64>(i) * g_frame_bytes + g_fp.Offset + static_cast<UINT64>(y) * g_fp.Footprint.RowPitch, 1,
                           static_cast<size_t>(g_row_bytes), f);
            }
            ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        D3D12_RANGE none{0, 0};
        s.rb->Unmap(0, &none);
        char line[96];
        std::snprintf(line, sizeof(line), " | %s %d of %d frames %s", kEyeTag[e], s.recorded, g_frames,
                      ok ? "written" : "NOT WRITTEN");
        out += line;
    }
    char head[160];
    std::snprintf(head, sizeof(head), "from frame %llu, %u x %u format %u: %d double, %d mono frame(s)",
                  static_cast<unsigned long long>(g_first_frame), g_w, g_h, g_fmt, g_doubles, g_monos);
    return out.empty() ? std::string("ERROR nothing captured (NOT MEASURED): ") + head : head + out;
}

int g_crop_w = 640, g_crop_h = 360;

void on_frame_end(uint64_t frame) {
    int ph = g_phase.load(std::memory_order_acquire);
    if (ph == 1) {
        std::string err = arm(frame, g_crop_w, g_crop_h);
        if (!err.empty()) {
            stop(err);
            return;
        }
        g_frames_waited = 0;
        d3d::set_bind_tap(d3d::kBindTapBurst, bind_tap);
        g_phase.store(2, std::memory_order_release);
    } else if (ph == 2 || ph == 3) {
        if (++g_frames_waited > g_frames + 12) stop("ERROR the bind sequence was not seen in time (NOT MEASURED)");
    } else if (ph == 4) {
        g_phase = 5;
    } else if (ph == 5) {
        d3d::set_bind_tap(d3d::kBindTapBurst, nullptr);
        stop(finish());
    }
}

}  // namespace

void init() {
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d3d::add_frame_end_listener(on_frame_end);
}

std::string grab(const std::string& prefix, int frames, int crop_w, int crop_h, unsigned timeout_ms) {
    std::lock_guard lock(g_mutex);
    g_prefix = prefix;
    g_frames = frames < 2 ? 2 : frames > kMaxFrames ? kMaxFrames : frames;
    g_crop_w = crop_w;
    g_crop_h = crop_h;
    ResetEvent(g_done);
    g_phase.store(1, std::memory_order_release);
    if (WaitForSingleObject(g_done, timeout_ms) != WAIT_OBJECT_0) {
        int expected = 1;
        g_phase.compare_exchange_strong(expected, 0);
        return "ERROR no frame within the timeout (NOT MEASURED)";
    }
    log::info("[burst] %s", g_result.c_str());
    return g_result;
}

}  // namespace rdrvr::burst_grab
