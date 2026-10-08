#include "core/eye_grab.h"

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
#include "core/eye_shape.h"
#include "core/log.h"
#include "core/post_target.h"
#include "core/state.h"

namespace rdrvr::eye_grab {
namespace {

// The post output target: the Post FXAA Target under FXAA, FXAATarget otherwise (post_target.h).

// The engine records its D3D12 commands on a thread of its own, later than the render thread issues them (cycle 14:
// DrawVisList records no draws), so the copies are recorded from the bind tap, on the recording thread, at the game's
// own binds of the target. Each post run binds it once for FXAA (no depth buffer) and the UI binds it once more (with
// a depth buffer); its content just before a bind is the previous phase's output:
//   before the second FXAA bind after a UI bind  -> the first pass's eye (a double frame with the first-eye post run)
//   before the UI bind                            -> the last pass's eye, or the mono image after a single FXAA bind
// Round 2: with the screen overlays (lens drops) drawn for the first eye too, its run binds the target twice (FXAA, then
// the overlays' PostTail), so its final image is before bind m/2 + 1 of the frame's m binds, which only the UI bind
// tells: the content before binds 2..4 is copied into slots 3..5 and the first eye is taken from the one that applies.
struct Slot {
    ID3D12Resource* rb = nullptr;  // readback buffer, kept between grabs
    uint64_t rb_size = 0;
    std::atomic<bool> recorded{false};
    std::string note;              // recording thread writes it before `recorded` is stored
    std::vector<uint8_t> pixels;   // copied out at finish
};
Slot g_slot[6];                    // 0 mono, 1 left, 2 right, 3..5 the content before the 2nd..4th bind
int g_first = 0, g_pick = 0;       // the first eye's slot (1 or 2) is written from slot g_pick (0: none)
const char* const kEyeTag[3] = {"M", "L", "R"};

std::mutex g_mutex;                // one grab at a time
std::string g_prefix, g_result;
HANDLE g_done = nullptr;
// 0 idle, 1 armed (set up at the next frame end), 2 waiting for a UI bind to start a sequence, 3 recording a sequence,
// 4 sequence recorded (one more frame end, then finish)
std::atomic<int> g_phase{0};
int g_frames_waited = 0;           // presenting thread
int g_fxaa_binds = 0;              // recording thread
bool g_swap = false;               // the first pass is the right eye
std::atomic<ID3D12Resource*> g_res{nullptr};
uint64_t g_rtv[8];
int g_nrtv = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp{};
UINT64 g_row_bytes = 0;
uint32_t g_w = 0, g_h = 0, g_fmt = 0;
uint64_t g_frame = 0;
char g_sequence[96] = "";
uint32_t g_rect = 0;               // [XR] EyeShape: the eye rect in the images (w | h << 16; 0: the whole image)
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_value = 0;
HANDLE g_fence_event = nullptr;

ID3D12Resource* make_readback(ID3D12Device* dev, uint64_t bytes) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// Recording thread: the copy of the target's current content into slot `i`, in the list being recorded, from the
// state the barrier watch saw last in that same list.
void record(ID3D12GraphicsCommandList* cl, int i) {
    Slot& s = g_slot[i];
    if (s.recorded.load(std::memory_order_relaxed)) return;
    ID3D12Resource* res = g_res.load();
    ID3D12GraphicsCommandList* wcl = nullptr;
    uint32_t st = 0;
    if (!res || !d3d::watched_state(res, &wcl, &st) || wcl != cl) {
        s.note = "the target's state was not seen in this list";
        return;
    }
    // the footprint is the armed target's: the target as it is now (live: its barrier is in this list) must match, so
    // a frame the game made again at another size since the arm is never copied into it
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Width != g_w || d.Height != g_h || static_cast<uint32_t>(d.Format) != g_fmt) {
        s.note = "the target was made again at another size or format since the arm";
        return;
    }
    const auto state = static_cast<D3D12_RESOURCE_STATES>(st);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;  // explicit even from COMMON: no promotion left behind
    if (transition) cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = s.rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = g_fp;
    sl.pResource = res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    if (transition) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
    }
    s.note = "from state " + std::to_string(st);
    s.recorded.store(true, std::memory_order_release);
}

void bind_tap(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL,
              const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    int ph = g_phase.load(std::memory_order_acquire);
    if ((ph != 2 && ph != 3) || n == 0 || !rts) return;
    bool target = false;
    for (int i = 0; i < g_nrtv; ++i) target = target || rts[0].ptr == g_rtv[i];
    if (!target) return;
    bool ui = ds != nullptr;
    if (ph == 2) {  // start at a sequence boundary
        if (ui) {
            g_fxaa_binds = 0;
            g_phase.store(3, std::memory_order_release);
        }
        return;
    }
    if (!ui) {
        int i = g_fxaa_binds++;
        if (i >= 1 && i <= 3) record(cl, 2 + i);
        return;
    }
    int k = g_fxaa_binds / 2;
    if (k > 3) k = 3;
    if (g_fxaa_binds >= 2) {
        record(cl, g_swap ? 1 : 2);
        g_first = g_swap ? 2 : 1;
        g_pick = 2 + k;
    } else if (g_fxaa_binds == 1) {
        record(cl, 0);
    }
    uint32_t cw = 0, ch = 0;
    g_rect = eye_shape::frame_rect(&cw, &ch) ? (cw & 0xffffu) | (ch << 16) : 0u;
    std::snprintf(g_sequence, sizeof(g_sequence), "%d FXAA bind(s) then the UI bind%s", g_fxaa_binds,
                  g_fxaa_binds >= 2 ? (k == 1 ? ", first eye before bind 2" : k == 2 ? ", first eye before bind 3" : ", first eye before bind 4") : "");
    g_fxaa_binds = 0;
    g_phase.store(4, std::memory_order_release);
}

// Presenting thread: find the live target and its RTVs, watch its barriers, and make sure each slot has a readback
// buffer, so the tap only records commands.
std::string arm(uint64_t frame) {
    char* postfx = *reinterpret_cast<char**>(anchors::addr(anchors::Id::PostFxSingleton));
    if (!postfx) return "ERROR no PostFx object (NOT MEASURED)";
    const char* tname = post_target::name(postfx);
    void* obj = *reinterpret_cast<void**>(postfx + post_target::field(postfx));
    char where[64] = "?";
    const void* found = d3d::resource_in_object(obj, 0x100, tname, where, sizeof(where));
    if (!found) {
        found = d3d::unique_resource(tname);
        std::snprintf(where, sizeof(where), " (not in it: the one resource with that name)");
    }
    auto* res = static_cast<ID3D12Resource*>(const_cast<void*>(found));
    if (!res) return std::string("ERROR no single live resource named \"") + tname + "\" (NOT MEASURED)";
    g_nrtv = d3d::rtv_handles_of(res, g_rtv, 8);
    if (g_nrtv == 0) return std::string("ERROR no render-target view of ") + tname + " seen (NOT MEASURED)";
    D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 || d.DepthOrArraySize != 1 ||
        d.SampleDesc.Count != 1)
        return std::string("ERROR ") + tname + " is not a single-subresource 2D texture (NOT MEASURED)";
    ID3D12Device* dev = nullptr;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return "ERROR no device";
    UINT rows = 0;
    UINT64 total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &g_fp, &rows, &g_row_bytes, &total);
    bool ok = true;
    for (Slot& s : g_slot) {
        if (!s.rb || s.rb_size < total) {
            if (s.rb) s.rb->Release();
            s.rb = make_readback(dev, total);
            s.rb_size = s.rb ? total : 0;
        }
        ok = ok && s.rb;
        s.recorded = false;
        s.note.clear();
    }
    if (!g_fence) dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev->Release();
    if (!ok || !g_fence) return "ERROR could not create the readback objects";
    g_w = static_cast<uint32_t>(d.Width);
    g_h = d.Height;
    g_fmt = static_cast<uint32_t>(d.Format);
    g_swap = camera_lever::swap_order();
    g_first = g_pick = 0;
    g_frame = frame + 1;
    g_sequence[0] = 0;
    g_rect = 0;
    g_res = res;
    d3d::watch_add(res);
    log::info("[eyegrab] armed at frame %llu: %s at object%s, %d RTV(s), %ux%u format %u", static_cast<unsigned long long>(frame),
              tname, where, g_nrtv, g_w, g_h, g_fmt);
    return {};
}

std::string finish() {
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q || !g_fence) return "ERROR no present queue";
    q->Signal(g_fence, ++g_fence_value);
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    if (WaitForSingleObject(g_fence_event, 2000) != WAIT_OBJECT_0) return "ERROR eye copies timed out";
    int got = 0;
    for (Slot& s : g_slot) {
        if (!s.recorded.load(std::memory_order_acquire)) continue;
        uint8_t* p = nullptr;
        D3D12_RANGE r{0, static_cast<SIZE_T>(s.rb_size)};
        if (FAILED(s.rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) {
            s.recorded = false;
            s.note = "Map failed";
            continue;
        }
        s.pixels.resize(static_cast<size_t>(g_row_bytes) * g_h);
        for (uint32_t y = 0; y < g_h; ++y)
            std::memcpy(s.pixels.data() + static_cast<size_t>(y) * g_row_bytes,
                        p + g_fp.Offset + static_cast<uint64_t>(y) * g_fp.Footprint.RowPitch, static_cast<size_t>(g_row_bytes));
        D3D12_RANGE none{0, 0};
        s.rb->Unmap(0, &none);
        ++got;
    }
    return got ? std::string{} : "ERROR no eye was captured (NOT MEASURED)";
}

void stop(const std::string& result) {
    d3d::set_bind_tap(d3d::kBindTapGrab, nullptr);
    d3d::watch_remove(g_res.load());
    g_res = nullptr;
    g_result = result;
    g_phase = 0;
    SetEvent(g_done);
}

void on_frame_end(uint64_t frame) {
    int ph = g_phase.load(std::memory_order_acquire);
    if (ph == 1) {
        std::string err = arm(frame);
        if (!err.empty()) {
            stop(err);
            return;
        }
        g_frames_waited = 0;
        d3d::set_bind_tap(d3d::kBindTapGrab, bind_tap);
        g_phase.store(2, std::memory_order_release);
    } else if (ph == 2 || ph == 3) {
        if (++g_frames_waited > 12) stop("ERROR the bind sequence of the post output target was not seen in 12 frames (NOT MEASURED)");
    } else if (ph == 4) {
        g_phase = 5;  // the sequence's list is submitted before the next Present
    } else if (ph == 5) {
        d3d::set_bind_tap(d3d::kBindTapGrab, nullptr);
        stop(finish());
    }
}

std::string write_rde(const std::string& path, const Slot& s, int eye) {
    std::wstring wpath(path.begin(), path.end());
    std::wstring tmp = wpath + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return "ERROR cannot open " + path;
    const uint32_t hdr[8] = {0x31454452u /* RDE1 */, g_w, g_h, g_fmt, static_cast<uint32_t>(eye),
                             static_cast<uint32_t>(g_frame), static_cast<uint32_t>(g_row_bytes), g_rect};
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(s.pixels.data(), 1, s.pixels.size(), f);
    bool ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
    return ok ? "wrote " + path : "ERROR writing " + path;
}

}  // namespace

void init() {
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d3d::add_frame_end_listener(on_frame_end);
}

std::string grab(const std::string& prefix, unsigned timeout_ms) {
    std::lock_guard lock(g_mutex);
    g_prefix = prefix;
    ResetEvent(g_done);
    g_phase.store(1, std::memory_order_release);
    if (WaitForSingleObject(g_done, timeout_ms) != WAIT_OBJECT_0) {
        int expected = 1;
        g_phase.compare_exchange_strong(expected, 0);  // never armed: disarm (an armed grab finishes on its own)
        return "ERROR no frame within the timeout (NOT MEASURED)";
    }
    if (!g_result.empty()) {
        log::error("[eyegrab] %s", g_result.c_str());
        return g_result;
    }
    std::string out;
    for (int i = 0; i < 3; ++i) {
        Slot& s = i == g_first && g_pick ? g_slot[g_pick] : g_slot[i];
        if (!s.recorded.load(std::memory_order_acquire)) {
            if (!s.note.empty()) out += std::string(" | ") + kEyeTag[i] + " NOT MEASURED: " + s.note;
            continue;
        }
        out += " | " + write_rde(g_prefix + "-" + kEyeTag[i] + ".rde", s, i - 1) + " (" + s.note + ")";
    }
    for (Slot& s : g_slot) {
        s.pixels.clear();
        s.pixels.shrink_to_fit();
    }
    char head[224];
    std::snprintf(head, sizeof(head), "frame %llu %ux%u format %u, %s%s", static_cast<unsigned long long>(g_frame), g_w, g_h, g_fmt,
                  g_sequence, g_rect ? (", eye rect " + std::to_string(g_rect & 0xffffu) + "x" + std::to_string(g_rect >> 16)).c_str() : "");
    log::info("[eyegrab] %s%s", head, out.c_str());
    return head + out;
}

}  // namespace rdrvr::eye_grab
