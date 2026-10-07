#include "core/ring_probe.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/diag.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::ring_probe {
namespace {

constexpr uint64_t kMinTracked = 256 * 1024;  // rings are large; most texture staging buffers are smaller
constexpr size_t kMaxTracked = 4096;
constexpr uint64_t kNone = ~0ull;

// Occlusion-query pool (rdrvr_DrawVisEntity 0x640120): index = slot * capacity + base + head++, while head < capacity.
constexpr uintptr_t kOccHead = 0x2ac533c, kOccCapacity = 0x2ac5554, kOccSlot = 0x2ac5550;

// Per buffer and frame: uses and the span they cover (lowest offset to highest end). On 2026-10-03 the game created
// ~2,260 CBV descriptors per frame in each of two 16 MB upload buffers at addresses that never advanced, i.e. a
// per-frame linear allocator that restarts each time its buffer comes round, not a continuous ring; the span is
// the per-frame footprint either way.
struct Buf {
    uint64_t va = 0, size = 0;
    const void* res = nullptr;
    const uint8_t* cpu = nullptr;      // persistent CPU mapping (for the matrices checker)
    uint32_t binds = 0, descs = 0;     // this frame
    uint64_t lo = kNone, hi = 0;       // this frame: lowest offset, highest offset + view size
    uint64_t w_used = 0, w_binds = 0, w_descs = 0, w_span = 0, w_max_span = 0, w_max_hi = 0;  // window
};

std::mutex g_mutex;
int g_enabled = -1;
std::vector<Buf> g_bufs;  // sorted by va, non-overlapping
uint64_t g_untracked = 0, g_w_untracked = 0, g_w_frames = 0;
int g_occ_max = 0, g_occ_cap = 0, g_occ_slots_seen = 0, g_occ_last_slot = -1;
std::atomic<int> g_occ_last{0};
char g_status[320] = "off";

using SetRootCbv_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using CreateCbv_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
SetRootCbv_t o_SetGraphicsRootCbv = nullptr, o_SetComputeRootCbv = nullptr;
CreateCbv_t o_CreateCbv = nullptr;

// ---- rage_matrices checker
std::atomic<bool> g_mcheck{false};
std::atomic<int> g_mpass{-2};  // -2 no pass, -1 mono pass, 0 left, 1 right
float g_m_eye[3], g_m_other[3], g_m_centre[3];
struct MCount {
    std::atomic<uint64_t> ok{0}, wrong{0}, centre{0}, other{0};
};
MCount g_mcount[3];  // rage_matrices blocks: mono, left, right
MCount g_mside[3];   // camera matrices at +0xC0 of other constant buffers (shadow lookups and the like)

// rage_matrices is gWorld, gWorldView, gWorldViewProj, gViewInverse: the first two are affine (w column 0,0,0,1, not
// all zero). Other buffers can hold a camera matrix at +0xC0 too: cycle 17's centre-camera block was a tree shader's
// local buffer whose gCameraMatrix is the cascade shadows' reference camera (shared by design).
bool affine(const float* m) {
    if (std::fabs(m[3]) > 1e-4f || std::fabs(m[7]) > 1e-4f || std::fabs(m[11]) > 1e-4f || std::fabs(m[15] - 1.0f) > 1e-4f)
        return false;
    float sum = 0;
    for (int k = 0; k < 12; ++k) sum += std::fabs(m[k]);
    return sum > 1e-3f;
}
std::atomic<int> g_bad_logged[3][2];  // per pass (mono, left, right) and kind (wrong-eye, centre)
std::atomic<bool> g_dumped[2];         // first centre block of each eye pass dumped since the checker was switched on

bool near3(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz < 1e-6f;  // 1 mm
}

void check_matrices(const uint8_t* p) {
    int pass = g_mpass.load(std::memory_order_acquire);
    if (pass < -1) return;
    const float* m = reinterpret_cast<const float*>(p + 0xC0);
    for (int r = 0; r < 3; ++r) {
        float n = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (std::fabs(n - 1.0f) > 1e-3f || m[r * 4 + 3] != 0.0f) return;  // not a camera matrix
    }
    if (m[15] != 1.0f) return;
    const float* pos = m + 12;
    const float* f = reinterpret_cast<const float*>(p);
    bool rage = affine(f) && affine(f + 16);
    MCount& c = rage ? g_mcount[pass + 1] : g_mside[pass + 1];
    const char* bad = nullptr;
    int kind = 0;
    if (near3(pos, g_m_eye)) c.ok.fetch_add(1, std::memory_order_relaxed);
    else if (pass >= 0 && near3(pos, g_m_other)) c.wrong.fetch_add(1, std::memory_order_relaxed), bad = "wrong-eye";
    else if (pass >= 0 && near3(pos, g_m_centre)) c.centre.fetch_add(1, std::memory_order_relaxed), bad = "centre", kind = 1;
    else c.other.fetch_add(1, std::memory_order_relaxed);
    if (bad && rage && g_bad_logged[pass + 1][kind].fetch_add(1) < 12) {
        // Who created this CBV: the engine's call chain identifies the draw (entity, effect or pass).
        void* frames[14];
        USHORT n = RtlCaptureStackBackTrace(1, 14, frames, nullptr);
        char chain[512];
        size_t k = 0;
        uintptr_t lo = anchors::base(), hi = lo + 0x5a5ec600;
        for (USHORT i = 0; i < n && k + 24 < sizeof(chain); ++i) {
            uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
            if (a >= lo && a < hi) k += std::snprintf(chain + k, sizeof(chain) - k, " %#llx", static_cast<unsigned long long>(a - lo));
        }
        chain[k] = 0;
        char vp[200];
        camera_lever::describe_current_viewport(vp, sizeof(vp));
        log::warn("[mcheck] pass %d %s camera (%.3f %.3f %.3f); %s; RDR.exe chain:%s", pass, bad, pos[0], pos[1], pos[2], vp,
                  chain);
        if (kind == 1 && pass >= 0 && !g_dumped[pass].exchange(true)) {
            // The whole block, then where else in memory its camera matrix lives (the cached copy the draw read).
            for (int r = 0; r < 0x140 / 16; ++r)
                log::info("[mcheck] block +%#05x: % .5f % .5f % .5f % .5f", r * 16, f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]);
            alignas(16) float mtx[16];
            std::memcpy(mtx, p + 0xC0, sizeof(mtx));
            diag::find_pattern(mtx, sizeof(mtx), "centre block matrix (64 bytes)", 16);
            diag::find_pattern(mtx + 8, 32, "centre block matrix rows c,d (32 bytes)", 16);
        }
    }
}

void touch(uint64_t va, uint64_t bytes, bool desc) {
    std::lock_guard lock(g_mutex);
    auto it = std::upper_bound(g_bufs.begin(), g_bufs.end(), va, [](uint64_t v, const Buf& b) { return v < b.va; });
    if (it == g_bufs.begin() || va >= (it - 1)->va + (it - 1)->size) {
        ++g_untracked;
        return;
    }
    Buf& b = *(it - 1);
    uint64_t off = va - b.va;
    if (desc && b.cpu && bytes >= 0x110 && off + 0x110 <= b.size && g_mcheck.load(std::memory_order_relaxed))
        check_matrices(b.cpu + off);
    if (desc) ++b.descs;
    else ++b.binds;
    b.lo = std::min(b.lo, off);
    b.hi = std::max(b.hi, std::min(off + bytes, b.size));
}

void STDMETHODCALLTYPE hk_SetGraphicsRootCbv(ID3D12GraphicsCommandList* cl, UINT root, D3D12_GPU_VIRTUAL_ADDRESS va) {
    touch(va, 256, false);  // a root CBV carries no size; 256 is the minimum CBV granularity
    o_SetGraphicsRootCbv(cl, root, va);
}
void STDMETHODCALLTYPE hk_SetComputeRootCbv(ID3D12GraphicsCommandList* cl, UINT root, D3D12_GPU_VIRTUAL_ADDRESS va) {
    touch(va, 256, false);
    o_SetComputeRootCbv(cl, root, va);
}
thread_local uint64_t t_cbvs = 0;

void STDMETHODCALLTYPE hk_CreateCbv(ID3D12Device* dev, const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    ++t_cbvs;
    if (d) touch(d->BufferLocation, d->SizeInBytes, true);
    o_CreateCbv(dev, d, h);
}

int read_int(uintptr_t rva) { return *reinterpret_cast<const volatile int*>(anchors::base() + rva); }

void report() {
    std::vector<const Buf*> top;
    for (const Buf& b : g_bufs)
        if (b.w_used) top.push_back(&b);
    std::sort(top.begin(), top.end(), [](const Buf* a, const Buf* b) { return a->w_binds + a->w_descs > b->w_binds + b->w_descs; });
    double f = static_cast<double>(g_w_frames ? g_w_frames : 1);
    log::info("[ring] %llu frames: %zu upload buffers >= 256 KB tracked; %.1f CBV uses/frame outside them",
              static_cast<unsigned long long>(g_w_frames), g_bufs.size(), g_w_untracked / f);
    for (size_t i = 0; i < top.size() && i < 4; ++i) {
        const Buf& b = *top[i];
        const char* name = d3d::resource_name(b.res);
        double u = static_cast<double>(b.w_used);
        log::info("[ring]   %s %.2f MB: used in %.0f%% of frames; per use: root CBV %.0f, CBV descriptors %.0f, span %.1f KB"
                  " (max %.1f KB, highest end %.1f KB = %.1f%% of the buffer)",
                  name ? name : "unnamed", b.size / 1048576.0, 100.0 * u / f, b.w_binds / u, b.w_descs / u,
                  b.w_span / u / 1024.0, b.w_max_span / 1024.0, b.w_max_hi / 1024.0, 100.0 * b.w_max_hi / b.size);
    }
    log::info("[ring] occlusion queries: head max %d of %d per frame (%d frame slots seen)", g_occ_max, g_occ_cap,
              g_occ_slots_seen);
    if (!top.empty()) {
        const Buf& b = *top[0];
        double u = static_cast<double>(b.w_used);
        std::snprintf(g_status, sizeof(g_status),
                      "top %.2f MB, used %.0f%% of frames: descs %.0f + binds %.0f per use, span %.1f KB (max %.1f, end %.1f%%);"
                      " occlusion max %d/%d",
                      b.size / 1048576.0, 100.0 * u / f, b.w_descs / u, b.w_binds / u, b.w_span / u / 1024.0,
                      b.w_max_span / 1024.0, 100.0 * b.w_max_hi / b.size, g_occ_max, g_occ_cap);
    } else {
        std::snprintf(g_status, sizeof(g_status), "no CBV use in tracked buffers (%.0f/f untracked); occlusion max %d/%d",
                      g_w_untracked / f, g_occ_max, g_occ_cap);
    }
    for (Buf& b : g_bufs) b.w_used = b.w_binds = b.w_descs = b.w_span = b.w_max_span = b.w_max_hi = 0;
    g_w_untracked = g_w_frames = 0;
    g_occ_max = 0;
}

void on_frame_end(uint64_t frame) {
    std::lock_guard lock(g_mutex);
    for (Buf& b : g_bufs) {
        if (b.binds + b.descs) {
            uint64_t span = b.hi > b.lo ? b.hi - b.lo : 0;
            ++b.w_used;
            b.w_binds += b.binds;
            b.w_descs += b.descs;
            b.w_span += span;
            b.w_max_span = std::max(b.w_max_span, span);
            b.w_max_hi = std::max(b.w_max_hi, b.hi);
        }
        b.binds = b.descs = 0;
        b.lo = kNone;
        b.hi = 0;
    }
    g_w_untracked += g_untracked;
    g_untracked = 0;
    ++g_w_frames;
    if (!anchors::stand_down()) {
        int head = read_int(kOccHead);
        g_occ_last = head;
        g_occ_max = std::max(g_occ_max, head);
        g_occ_cap = read_int(kOccCapacity);
        int slot = read_int(kOccSlot);
        if (slot != g_occ_last_slot) {
            g_occ_last_slot = slot;
            g_occ_slots_seen = std::max(g_occ_slots_seen, slot + 1);
        }
    }
    if (frame % 600 == 0) report();
}

}  // namespace

bool enabled() {
    if (g_enabled < 0) g_enabled = config::get_bool("Debug", "RingProbe", false) ? 1 : 0;
    return g_enabled == 1;
}

void install_device_hooks(ID3D12Device* dev) {
    if (!enabled()) return;
    hooks::install("ID3D12Device::CreateConstantBufferView", hooks::vtable_entry(dev, 17), hk_CreateCbv, &o_CreateCbv);
}

void install_list_hooks(ID3D12GraphicsCommandList* cl) {
    if (!enabled()) return;
    hooks::install("CL::SetComputeRootConstantBufferView", hooks::vtable_entry(cl, 37), hk_SetComputeRootCbv,
                   &o_SetComputeRootCbv);
    hooks::install("CL::SetGraphicsRootConstantBufferView", hooks::vtable_entry(cl, 38), hk_SetGraphicsRootCbv,
                   &o_SetGraphicsRootCbv);
}

void on_upload_buffer(ID3D12Resource* res, const D3D12_RESOURCE_DESC* desc) {
    if (!enabled() || !res || !desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc->Width < kMinTracked) return;
    uint64_t va = res->GetGPUVirtualAddress();
    if (!va) return;
    std::lock_guard lock(g_mutex);
    // A new buffer can reuse the address range of a released one: drop whatever it overlaps.
    std::erase_if(g_bufs, [&](const Buf& b) { return b.va < va + desc->Width && va < b.va + b.size; });
    if (g_bufs.size() >= kMaxTracked) return;
    Buf nb;
    nb.va = va;
    nb.size = desc->Width;
    nb.res = res;
    void* cpu = nullptr;
    D3D12_RANGE none{0, 0};
    if (SUCCEEDED(res->Map(0, &none, &cpu))) nb.cpu = static_cast<const uint8_t*>(cpu);  // upload heaps stay mapped
    g_bufs.insert(std::upper_bound(g_bufs.begin(), g_bufs.end(), va, [](uint64_t v, const Buf& b) { return v < b.va; }), nb);
    log::limited("ring.track", 64, "[ring] tracking upload buffer %p %.2f MB at %#llx", static_cast<void*>(res),
                 desc->Width / 1048576.0, static_cast<unsigned long long>(va));
}

void init() {
    if (enabled()) d3d::add_frame_end_listener(on_frame_end);
}

uint64_t thread_cbvs() { return t_cbvs; }

const uint8_t* cpu_of(uint64_t va, uint64_t bytes) {
    std::lock_guard lock(g_mutex);
    auto it = std::upper_bound(g_bufs.begin(), g_bufs.end(), va, [](uint64_t v, const Buf& b) { return v < b.va; });
    if (it == g_bufs.begin()) return nullptr;
    const Buf& b = *(it - 1);
    if (!b.cpu || va + bytes > b.va + b.size) return nullptr;
    return b.cpu + (va - b.va);
}

void matrix_check(bool on) {
    for (MCount& c : g_mcount) c.ok = c.wrong = c.centre = c.other = 0;
    for (MCount& c : g_mside) c.ok = c.wrong = c.centre = c.other = 0;
    for (auto& p : g_bad_logged)
        for (auto& k : p) k = 0;
    for (auto& d : g_dumped) d = false;
    g_mcheck = on;
    log::info("[mcheck] rage_matrices checker %s", on ? "on" : "off");
}

void matrix_expect(int pass, const float* eye, const float* other, const float* centre) {
    if (pass < -1) {
        g_mpass.store(-2, std::memory_order_release);
        return;
    }
    for (int k = 0; k < 3; ++k) {
        g_m_eye[k] = eye[k];
        g_m_other[k] = other ? other[k] : eye[k];
        g_m_centre[k] = centre ? centre[k] : eye[k];
    }
    g_mpass.store(pass, std::memory_order_release);
}

void matrix_text(char* out, size_t len) {
    const char* names[3] = {"mono", "left", "right"};
    size_t n = 0;
    n += std::snprintf(out + n, len - n, "%s", g_mcheck.load() ? "on" : "off");
    for (int i = 0; i < 3 && n < len; ++i)
        n += std::snprintf(out + n, len - n, " | %s ok %llu wrong-eye %llu centre %llu other %llu", names[i],
                           static_cast<unsigned long long>(g_mcount[i].ok.load()),
                           static_cast<unsigned long long>(g_mcount[i].wrong.load()),
                           static_cast<unsigned long long>(g_mcount[i].centre.load()),
                           static_cast<unsigned long long>(g_mcount[i].other.load()));
    for (int i = 0; i < 3 && n < len; ++i)
        n += std::snprintf(out + n, len - n, " | %s other buffers: eye %llu wrong-eye %llu centre %llu other %llu", names[i],
                           static_cast<unsigned long long>(g_mside[i].ok.load()),
                           static_cast<unsigned long long>(g_mside[i].wrong.load()),
                           static_cast<unsigned long long>(g_mside[i].centre.load()),
                           static_cast<unsigned long long>(g_mside[i].other.load()));
}

void status_text(char* out, size_t len) {
    std::lock_guard lock(g_mutex);
    std::snprintf(out, len, "%s; occlusion head last frame %d", g_status, g_occ_last.load());
}

}  // namespace rdrvr::ring_probe
