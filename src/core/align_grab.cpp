#include "core/align_grab.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/dlss.h"
#include "core/log.h"
#include "core/post_target.h"
#include "core/state.h"
#include "core/xr.h"

namespace rdrvr::align_grab {
namespace {

constexpr int kMaxFrames = 24;
constexpr int kSrc = 3;  // 0 the post output, 1 FullScreenCopy (the upscaler's input), 2 the Velocity RT
const char* const kTag[kSrc] = {"post", "scene", "vel"};
const size_t kField[kSrc] = {0, 0x10, 0x3f0};  // PostFx fields (0: post_target's)
const char* const kName[kSrc] = {nullptr, "FullScreenCopy", "Velocity RT"};

struct Src {
    ID3D12Resource* res = nullptr;
    ID3D12Resource* rb = nullptr;  // one footprint per frame; never released (a late tap may still copy into it)
    uint64_t rb_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 row_bytes = 0, frame_bytes = 0;
    D3D12_BOX box{};
    uint32_t w = 0, h = 0, fmt = 0;
    uint64_t src_w = 0;
    uint32_t src_h = 0;
};
Src g_src[kSrc];
bool g_valid[kMaxFrames];      // recording thread writes, presenting thread reads after the phase handshake
float g_jitter[kMaxFrames][2];
int g_recorded = 0;

std::mutex g_mutex;
std::string g_prefix, g_result;
HANDLE g_done = nullptr;
// 0 idle, 1 armed, 2 waiting for a UI bind, 3 recording, 4 recorded (one more frame end), 5 the copies' fence signalled,
// 6 the copies done (the grab's thread writes the files: never the present thread, the headset would freeze)
std::atomic<int> g_phase{0};
int g_frames = 16, g_frames_waited = 0;
int g_frame_i = 0, g_binds = 0, g_doubles = 0, g_monos = 0;  // recording thread
bool g_swap = false;
uint64_t g_rtv[8];
int g_nrtv = 0;
uint64_t g_first_frame = 0;
int g_crop_w = 1536, g_crop_h = 864;
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_value = 0;
HANDLE g_fence_event = nullptr;

ID3D12Resource* find_target(char* postfx, size_t field, const char* name) {
    void* obj = *reinterpret_cast<void**>(postfx + field);
    char where[64] = "?";
    const void* found = obj ? d3d::resource_in_object(obj, 0x100, name, where, sizeof(where)) : nullptr;
    if (!found) found = d3d::unique_resource(name);
    return static_cast<ID3D12Resource*>(const_cast<void*>(found));
}

// All three in this frame, or none: each from the state its barrier left in this list.
void record(ID3D12GraphicsCommandList* cl) {
    if (g_frame_i >= g_frames || g_valid[g_frame_i]) return;
    uint32_t st[kSrc];
    for (int k = 0; k < kSrc; ++k) {
        ID3D12GraphicsCommandList* wcl = nullptr;
        if (!d3d::watched_state(g_src[k].res, &wcl, &st[k]) || wcl != cl) return;  // this frame is not measured
        const D3D12_RESOURCE_DESC d = g_src[k].res->GetDesc();
        if (d.Width != g_src[k].src_w || d.Height != g_src[k].src_h || static_cast<uint32_t>(d.Format) != g_src[k].fmt) return;
    }
    for (int k = 0; k < kSrc; ++k) {
        Src& s = g_src[k];
        const auto state = static_cast<D3D12_RESOURCE_STATES>(st[k]);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = s.res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        const bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (transition) cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = s.rb;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dl.PlacedFootprint = s.fp;
        dl.PlacedFootprint.Offset = static_cast<UINT64>(g_frame_i) * s.frame_bytes;
        sl.pResource = s.res;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, &s.box);
        if (transition) {
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            cl->ResourceBarrier(1, &b);
        }
    }
    // the jitter this eye's DLSS constants carried (the playback thread set them earlier in this run); 0 without them
    if (!dlss::last_jitter(g_jitter[g_frame_i])) g_jitter[g_frame_i][0] = g_jitter[g_frame_i][1] = 0.0f;
    g_valid[g_frame_i] = true;
    ++g_recorded;
}

void bind_tap(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int,
              const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    const int ph = g_phase.load(std::memory_order_acquire);
    if ((ph != 2 && ph != 3) || n == 0 || !rts) return;
    bool target = false;
    for (int i = 0; i < g_nrtv; ++i) target = target || rts[0].ptr == g_rtv[i];
    if (!target) return;
    const bool ui = ds != nullptr;
    if (ph == 2) {
        if (ui) {
            g_binds = g_frame_i = g_doubles = g_monos = 0;
            g_phase.store(3, std::memory_order_release);
        }
        return;
    }
    if (!ui) {
        ++g_binds;
        return;
    }
    if (g_binds >= 2) {
        record(cl);  // the last pass's eye: its post output, its scene colour and its motion vectors are all in place
        ++g_doubles;
    } else if (g_binds == 1) {
        ++g_monos;  // a mono frame: not measured (its slot stays empty)
    }
    g_binds = 0;
    if (++g_frame_i >= g_frames) g_phase.store(4, std::memory_order_release);
}

std::string arm_src(ID3D12Device* dev, Src& s, ID3D12Resource* res, uint32_t w, uint32_t h, const char* name) {
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1)
        return std::string("ERROR ") + name + " is not a single-subresource 2D texture (NOT MEASURED)";
    if (w > d.Width) w = static_cast<uint32_t>(d.Width);
    if (h > d.Height) h = d.Height;
    if (w == 0 || h == 0) return std::string("ERROR ") + name + ": empty crop";
    s.box.left = static_cast<UINT>((d.Width - w) / 2);
    s.box.top = (d.Height - h) / 2;
    s.box.right = s.box.left + w;
    s.box.bottom = s.box.top + h;
    s.box.front = 0;
    s.box.back = 1;
    D3D12_RESOURCE_DESC cd = d;
    cd.Width = w;
    cd.Height = h;
    UINT rows = 0;
    UINT64 total = 0;
    dev->GetCopyableFootprints(&cd, 0, 1, 0, &s.fp, &rows, &s.row_bytes, &total);
    s.frame_bytes = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    const UINT64 need = s.frame_bytes * static_cast<UINT64>(g_frames);
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
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.rb));
        s.rb_size = s.rb ? need : 0;
        if (!s.rb) return "ERROR could not create the readback objects";
    }
    s.res = res;
    s.w = w;
    s.h = h;
    s.fmt = static_cast<uint32_t>(d.Format);
    s.src_w = d.Width;
    s.src_h = d.Height;
    return {};
}

std::string arm(uint64_t frame) {
    char* postfx = *reinterpret_cast<char**>(anchors::addr(anchors::Id::PostFxSingleton));
    if (!postfx) return "ERROR no PostFx object (NOT MEASURED)";
    ID3D12Resource* res[kSrc];
    for (int k = 0; k < kSrc; ++k) {
        const char* name = k ? kName[k] : post_target::name(postfx);
        res[k] = find_target(postfx, k ? kField[k] : post_target::field(postfx), name);
        if (!res[k]) return std::string("ERROR no single live resource named \"") + name + "\" (NOT MEASURED)";
    }
    g_nrtv = d3d::rtv_handles_of(res[0], g_rtv, 8);
    if (g_nrtv == 0) return "ERROR no render-target view of the post output seen (NOT MEASURED)";
    ID3D12Device* dev = nullptr;
    if (FAILED(res[0]->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return "ERROR no device";
    const D3D12_RESOURCE_DESC dp = res[0]->GetDesc(), ds = res[1]->GetDesc();
    // the input's crop covers the post crop's field: scaled by the render / output size
    const uint32_t pw = static_cast<uint32_t>(g_crop_w), ph = static_cast<uint32_t>(g_crop_h);
    const uint32_t sw = static_cast<uint32_t>(static_cast<double>(pw) * static_cast<double>(ds.Width) / static_cast<double>(dp.Width) + 0.5);
    const uint32_t sh = static_cast<uint32_t>(static_cast<double>(ph) * ds.Height / dp.Height + 0.5);
    std::string err = arm_src(dev, g_src[0], res[0], pw, ph, "the post output");
    if (err.empty()) err = arm_src(dev, g_src[1], res[1], sw, sh, kName[1]);
    if (err.empty()) err = arm_src(dev, g_src[2], res[2], sw, sh, kName[2]);
    if (err.empty() && !g_fence) dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev->Release();
    if (!err.empty()) return err;
    if (!g_fence) return "ERROR no fence";
    std::memset(g_valid, 0, sizeof(g_valid));
    std::memset(g_jitter, 0, sizeof(g_jitter));
    g_recorded = 0;
    g_swap = camera_lever::swap_order();
    g_first_frame = frame + 1;
    for (const Src& s : g_src) d3d::watch_add(s.res);
    log::info("[align] armed at frame %llu: post %ux%u of %llux%u, input %ux%u of %llux%u, motion vectors %ux%u, %d frames",
              static_cast<unsigned long long>(frame), g_src[0].w, g_src[0].h, static_cast<unsigned long long>(g_src[0].src_w),
              g_src[0].src_h, g_src[1].w, g_src[1].h, static_cast<unsigned long long>(g_src[1].src_w), g_src[1].src_h, g_src[2].w,
              g_src[2].h, g_frames);
    return {};
}

void stop(const std::string& result) {
    d3d::set_bind_tap(d3d::kBindTapAlign, nullptr);
    for (Src& s : g_src)
        if (s.res) {
            d3d::watch_remove(s.res);
            s.res = nullptr;
        }
    g_result = result;
    g_phase = 0;
    SetEvent(g_done);
}

bool write_rdb(const Src& s, const std::string& path, int eye) {
    uint8_t* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(s.rb_size)};
    if (FAILED(s.rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) return false;
    std::wstring wpath(path.begin(), path.end()), tmp = wpath + L".tmp";
    FILE* f = nullptr;
    bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
    if (ok) {
        const uint32_t hdr[8] = {0x31424452u /* RDB1 */, s.w, s.h, s.fmt, static_cast<uint32_t>(eye), static_cast<uint32_t>(g_frames),
                                 static_cast<uint32_t>(g_first_frame), static_cast<uint32_t>(s.row_bytes)};
        fwrite(hdr, 1, sizeof(hdr), f);
        for (int i = 0; i < g_frames; ++i) {
            const uint32_t v = g_valid[i] ? 1u : 0u;
            fwrite(&v, 1, 4, f);
            for (uint32_t y = 0; y < s.h; ++y)
                fwrite(p + static_cast<UINT64>(i) * s.frame_bytes + s.fp.Offset + static_cast<UINT64>(y) * s.fp.Footprint.RowPitch, 1,
                       static_cast<size_t>(s.row_bytes), f);
        }
        ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    D3D12_RANGE none{0, 0};
    s.rb->Unmap(0, &none);
    return ok;
}

std::string write_files() {
    char head[200];
    std::snprintf(head, sizeof(head), "from frame %llu: %d double, %d mono frame(s), %d of %d measured",
                  static_cast<unsigned long long>(g_first_frame), g_doubles, g_monos, g_recorded, g_frames);
    if (!g_recorded) return std::string("ERROR nothing captured (NOT MEASURED): ") + head;
    const int eye = g_swap ? 0 : 1;  // the second pass's eye
    std::string out = head;
    for (int k = 0; k < kSrc; ++k) {
        const std::string path = g_prefix + "-" + kTag[k] + ".rdb";
        out += std::string(" | ") + kTag[k] + (write_rdb(g_src[k], path, eye) ? " written" : " NOT WRITTEN");
    }
    const std::string jp = g_prefix + "-jitter.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, jp.c_str(), "w") == 0 && f) {
        std::fprintf(f, "# frame valid jitter_x jitter_y (render pixels, the second pass's DLSS constants)\n");
        for (int i = 0; i < g_frames; ++i) std::fprintf(f, "%d %d %.6f %.6f\n", i, g_valid[i] ? 1 : 0, g_jitter[i][0], g_jitter[i][1]);
        fclose(f);
        out += " | jitter written";
    }
    return out;
}

void on_frame_end(uint64_t frame) {
    const int ph = g_phase.load(std::memory_order_acquire);
    if (ph == 1) {
        const std::string err = arm(frame);
        if (!err.empty()) {
            stop(err);
            return;
        }
        g_frames_waited = 0;
        d3d::set_bind_tap(d3d::kBindTapAlign, bind_tap);
        g_phase.store(2, std::memory_order_release);
    } else if (ph == 2 || ph == 3) {
        if (++g_frames_waited > g_frames + 12) stop("ERROR the bind sequence was not seen in time (NOT MEASURED)");
    } else if (ph == 4) {
        d3d::set_bind_tap(d3d::kBindTapAlign, nullptr);
        ID3D12CommandQueue* q = state::present_queue.load();
        if (!q || !g_fence) return stop("ERROR no present queue");
        q->Signal(g_fence, ++g_fence_value);  // after this frame's lists: every copy is before it
        g_frames_waited = 0;
        g_phase = 5;
    } else if (ph == 5) {
        if (g_fence->GetCompletedValue() >= g_fence_value) {  // never a wait here
            g_phase = 6;
            SetEvent(g_done);
        } else if (++g_frames_waited > 120) {
            stop("ERROR the copies did not complete (NOT MEASURED)");
        }
    }
}

// [Debug] AutoAlignGrab = N (0 off): N sets taken by themselves in a headset session, the first after AutoAlignDelay
// seconds of submitted frames (30), then 10 s apart, into %LOCALAPPDATA%\RDRVR\align\ (a crop of 1024 x 576): the
// user plays, the sets show whether the eye images leave the mod aligned. A set that met no double frame (a menu,
// the cinema) is taken again, up to 3 N tries.
DWORD WINAPI auto_thread(void*) {
    const int n = config::get_int("Debug", "AutoAlignGrab", 0);
    const int delay = config::get_int("Debug", "AutoAlignDelay", 30);
    char base[MAX_PATH] = "";
    if (!ExpandEnvironmentStringsA("%LOCALAPPDATA%\\RDRVR\\align", base, MAX_PATH)) return 0;
    CreateDirectoryA(base, nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    int taken = 0, tries = 0, submitted_s = 0;
    while (taken < n && tries < 3 * n) {
        Sleep(1000);
        submitted_s = xr::submitting() ? submitted_s + 1 : 0;
        if (submitted_s < delay) continue;
        const std::string pre = std::string(base) + "\\" + stamp + "_" + std::to_string(taken + 1);
        const std::string r = grab(pre, 16, 1024, 576, 15000);
        ++tries;
        log::info("[align] auto set %d of %d (try %d): %s", taken + 1, n, tries, r.c_str());
        if (r.rfind("ERROR", 0) != 0) ++taken;
        submitted_s = delay - 10;  // the next one 10 s later
    }
    return 0;
}

}  // namespace

void init() {
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d3d::add_frame_end_listener(on_frame_end);
    if (config::get_int("Debug", "AutoAlignGrab", 0) > 0) {
        if (HANDLE h = CreateThread(nullptr, 0, auto_thread, nullptr, 0, nullptr)) {
            SetThreadDescription(h, L"RDRVR align capture");
            CloseHandle(h);
        }
        log::info("[align] %d sets will be taken by themselves, the first after %d s of submitted frames", config::get_int("Debug", "AutoAlignGrab", 0),
                  config::get_int("Debug", "AutoAlignDelay", 30));
    }
}

std::string grab(const std::string& prefix, int frames, int crop_w, int crop_h, unsigned timeout_ms) {
    std::lock_guard lock(g_mutex);
    g_prefix = prefix;
    g_frames = frames < 2 ? 2 : frames > kMaxFrames ? kMaxFrames : frames;
    g_crop_w = crop_w > 0 ? crop_w : 1536;
    g_crop_h = crop_h > 0 ? crop_h : 864;
    ResetEvent(g_done);
    g_result.clear();
    g_phase.store(1, std::memory_order_release);
    if (WaitForSingleObject(g_done, timeout_ms) != WAIT_OBJECT_0) {
        int expected = 1;
        g_phase.compare_exchange_strong(expected, 0);
        return "ERROR no frame within the timeout (NOT MEASURED)";
    }
    if (g_phase.load(std::memory_order_acquire) == 6) {  // the copies are done: the files, on this thread
        g_result = write_files();
        for (Src& s : g_src)
            if (s.res) {
                d3d::watch_remove(s.res);
                s.res = nullptr;
            }
        g_phase = 0;
    }
    log::info("[align] %s", g_result.c_str());
    return g_result;
}

}  // namespace rdrvr::align_grab
