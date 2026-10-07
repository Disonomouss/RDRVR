#include "core/pso.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/d3d_hooks.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/ring_probe.h"

namespace rdrvr::pso {
namespace {

using CreateGraphics_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStream_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using SetPipelineState_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using SetVB_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
SetVB_t o_SetVB = nullptr;
thread_local ID3D12GraphicsCommandList* t_vb_cl = nullptr;
thread_local D3D12_VERTEX_BUFFER_VIEW t_vb{};
// Vertex capture ("pso vbcap <vs> <file>"): the next frame's first 16 draws of that vertex shader, 1024 vertices each.
std::atomic<uint64_t> g_cap_vs{0};
std::atomic<int> g_cap_state{0};  // 0 idle, 1 armed, 2 capturing, 3 done
std::string g_cap_file, g_cap_text;
int g_cap_draws = 0;
CreateGraphics_t o_CreateGraphics = nullptr;
CreateStream_t o_CreateStream = nullptr;
SetPipelineState_t o_SetPipelineState = nullptr;

std::mutex g_mutex;
std::unordered_map<void*, uint64_t> g_vs_of;                 // pipeline -> vertex shader hash
std::unordered_map<uint64_t, std::vector<uint8_t>> g_bytecode;  // vertex shader hash -> bytecode
std::unordered_map<uint64_t, uint64_t> g_draws, g_last;     // vertex shader hash -> draws this frame / last frame
std::unordered_set<uint64_t> g_skip_set;  // under g_mutex
std::atomic<bool> g_skip_any{false};
std::atomic<uint64_t> g_skip{0}, g_skipped{0}, g_graphics{0}, g_stream{0}, g_unhashed{0};
thread_local ID3D12GraphicsCommandList* t_cl = nullptr;
thread_local uint64_t t_vs = 0;

uint64_t fnv(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

void record(void* pso, const D3D12_SHADER_BYTECODE& vs) {
    if (!pso) return;
    uint64_t h = vs.pShaderBytecode && vs.BytecodeLength ? fnv(vs.pShaderBytecode, vs.BytecodeLength) : 0;
    std::lock_guard lock(g_mutex);
    g_vs_of[pso] = h;
    if (h && !g_bytecode.count(h)) {
        const uint8_t* b = static_cast<const uint8_t*>(vs.pShaderBytecode);
        g_bytecode.emplace(h, std::vector<uint8_t>(b, b + vs.BytecodeLength));
    }
}

HRESULT STDMETHODCALLTYPE hk_CreateGraphics(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** out) {
    HRESULT hr = o_CreateGraphics(dev, d, riid, out);
    if (SUCCEEDED(hr) && d && out && *out) {
        g_graphics.fetch_add(1, std::memory_order_relaxed);
        record(*out, d->VS);
    }
    return hr;
}

// The inner size of a stream subobject and whether it holds pointers (8-byte aligned after the 4-byte type).
bool subobject(UINT type, size_t* inner, bool* ptr) {
    switch (type) {
        case 0: *inner = 8; *ptr = true; return true;                                     // root signature
        case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25:
            *inner = sizeof(D3D12_SHADER_BYTECODE); *ptr = true; return true;            // VS PS DS HS GS CS AS MS
        case 7: *inner = sizeof(D3D12_STREAM_OUTPUT_DESC); *ptr = true; return true;
        case 8: *inner = sizeof(D3D12_BLEND_DESC); *ptr = false; return true;
        case 9: case 13: case 14: case 16: case 18: case 20: *inner = 4; *ptr = false; return true;
        case 10: *inner = sizeof(D3D12_RASTERIZER_DESC); *ptr = false; return true;
        case 11: case 21: *inner = sizeof(D3D12_DEPTH_STENCIL_DESC); *ptr = false; return true;
        case 12: *inner = sizeof(D3D12_INPUT_LAYOUT_DESC); *ptr = true; return true;
        case 15: *inner = sizeof(D3D12_RT_FORMAT_ARRAY); *ptr = false; return true;
        case 17: *inner = sizeof(DXGI_SAMPLE_DESC); *ptr = false; return true;
        case 19: *inner = sizeof(D3D12_CACHED_PIPELINE_STATE); *ptr = true; return true;
        case 22: *inner = sizeof(D3D12_VIEW_INSTANCING_DESC); *ptr = true; return true;
        default: return false;
    }
}

HRESULT STDMETHODCALLTYPE hk_CreateStream(ID3D12Device* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** out) {
    HRESULT hr = o_CreateStream(dev, d, riid, out);
    if (SUCCEEDED(hr) && d && out && *out && d->pPipelineStateSubobjectStream) {
        g_stream.fetch_add(1, std::memory_order_relaxed);
        const uint8_t* p = static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream);
        const uint8_t* end = p + d->SizeInBytes;
        bool found = false;
        while (p + 4 <= end) {
            UINT type = *reinterpret_cast<const UINT*>(p);
            size_t inner = 0;
            bool ptr = false;
            if (!subobject(type, &inner, &ptr)) break;
            size_t off = ptr ? 8 : 4;
            if (type == 1) {
                record(*out, *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(p + off));
                found = true;
                break;
            }
            p += (off + inner + 7) & ~size_t(7);
        }
        if (!found) g_unhashed.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

void STDMETHODCALLTYPE hk_SetPipelineState(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    uint64_t h = 0;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_vs_of.find(ps);
        if (it != g_vs_of.end()) h = it->second;
    }
    t_cl = cl;
    t_vs = h;
    o_SetPipelineState(cl, ps);
}

void STDMETHODCALLTYPE hk_SetVB(ID3D12GraphicsCommandList* cl, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    if (start == 0 && n >= 1 && v) {
        t_vb_cl = cl;
        t_vb = v[0];
    }
    o_SetVB(cl, start, n, v);
}

void on_frame_end(uint64_t) {
    int st = g_cap_state.load();
    if (st == 1) g_cap_state = 2;
    else if (st == 2) {
        std::lock_guard lock(g_mutex);
        FILE* f = nullptr;
        if (fopen_s(&f, g_cap_file.c_str(), "w") == 0 && f) {
            fputs(g_cap_text.c_str(), f);
            fclose(f);
        }
        g_cap_state = 3;
    }
    std::lock_guard lock(g_mutex);
    g_last.swap(g_draws);
    g_draws.clear();
}

}  // namespace

void install_device_hooks(ID3D12Device* dev) {
    hooks::install("ID3D12Device::CreateGraphicsPipelineState", hooks::vtable_entry(dev, 10), hk_CreateGraphics, &o_CreateGraphics);
    ID3D12Device2* d2 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d2))) && d2) {
        hooks::install("ID3D12Device2::CreatePipelineState", hooks::vtable_entry(d2, 47), hk_CreateStream, &o_CreateStream);
        d2->Release();
    }
    d3d::add_frame_end_listener(on_frame_end);
}

void on_draw_args(ID3D12GraphicsCommandList* cl, int64_t first_vertex, uint32_t count) {
    if (g_cap_state.load(std::memory_order_relaxed) != 2 || cl != t_cl || t_vs != g_cap_vs.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mutex);
    if (g_cap_draws >= 16) return;
    ++g_cap_draws;
    char buf[256];
    D3D12_VERTEX_BUFFER_VIEW vb = cl == t_vb_cl ? t_vb : D3D12_VERTEX_BUFFER_VIEW{};
    std::snprintf(buf, sizeof(buf), "draw %d: vb %#llx size %u stride %u, first vertex %lld, count %u\n", g_cap_draws,
                  static_cast<unsigned long long>(vb.BufferLocation), vb.SizeInBytes, vb.StrideInBytes,
                  static_cast<long long>(first_vertex), count);
    g_cap_text += buf;
    if (!vb.BufferLocation || !vb.StrideInBytes || first_vertex < 0) return;
    uint32_t n = count < 1024 ? count : 1024;
    uint64_t at = vb.BufferLocation + static_cast<uint64_t>(first_vertex) * vb.StrideInBytes;
    const uint8_t* p = ring_probe::cpu_of(at, static_cast<uint64_t>(n) * vb.StrideInBytes);
    if (!p) {
        g_cap_text += "  (not in a tracked upload buffer)\n";
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const float* f = reinterpret_cast<const float*>(p + static_cast<size_t>(i) * vb.StrideInBytes);
        int nf = static_cast<int>(vb.StrideInBytes / 4);
        std::string line = "  v" + std::to_string(i) + ":";
        for (int k = 0; k < nf && k < 12; ++k) {
            std::snprintf(buf, sizeof(buf), " %.4f", f[k]);
            line += buf;
        }
        g_cap_text += line + "\n";
    }
}

void install_list_hooks(ID3D12GraphicsCommandList* cl) {
    hooks::install("CL::IASetVertexBuffers", hooks::vtable_entry(cl, 44), hk_SetVB, &o_SetVB);
    hooks::install("CL::SetPipelineState", hooks::vtable_entry(cl, 25), hk_SetPipelineState, &o_SetPipelineState);
}

bool on_draw(ID3D12GraphicsCommandList* cl) {
    uint64_t h = cl == t_cl ? t_vs : 0;
    if (h) {
        std::lock_guard lock(g_mutex);
        ++g_draws[h];
        if (g_skip_any.load(std::memory_order_relaxed) && g_skip_set.count(h)) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    uint64_t skip = g_skip.load(std::memory_order_relaxed);
    if (skip && h == skip) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string word, sub, arg;
    in >> word >> sub >> arg;
    char buf[256];
    if (sub == "vbcap") {
        // pso vbcap <vs hash> <file>: capture the next frame's draws of that vertex shader
        std::string file;
        in >> file;
        if (arg.empty() || file.empty()) return "ERROR usage: pso vbcap <vs hash> <file>";
        {
            std::lock_guard lock(g_mutex);
            g_cap_file = file;
            g_cap_text.clear();
            g_cap_draws = 0;
        }
        g_cap_vs = std::strtoull(arg.c_str(), nullptr, 16);
        g_cap_state = 1;
        for (int i = 0; i < 100 && g_cap_state.load() != 3; ++i) Sleep(20);
        return g_cap_state.load() == 3 ? "captured " + std::to_string(g_cap_draws) + " draws to " + file : "ERROR no capture within 2 s";
    }
    if (sub == "skipset") {
        // pso skipset h1,h2,... | pso skipset off
        std::lock_guard lock(g_mutex);
        g_skip_set.clear();
        if (arg != "off") {
            std::stringstream ss(arg);
            std::string item;
            while (std::getline(ss, item, ',')) if (!item.empty()) g_skip_set.insert(std::strtoull(item.c_str(), nullptr, 16));
        }
        g_skip_any = !g_skip_set.empty();
        return "skip set " + std::to_string(g_skip_set.size()) + " vertex shaders";
    }
    if (sub == "list") {
        // all vertex shaders that drew last frame, comma separated
        std::lock_guard lock(g_mutex);
        std::string out;
        for (auto& [h2, n2] : g_last) {
            std::snprintf(buf, sizeof(buf), "%s%016llx", out.empty() ? "" : ",", static_cast<unsigned long long>(h2));
            out += buf;
        }
        return out;
    }
    if (sub == "skip") {
        uint64_t h = arg == "off" || arg.empty() ? 0 : std::strtoull(arg.c_str(), nullptr, 16);
        g_skip = h;
        log::info("[pso] skip draws of vertex shader %016llx", static_cast<unsigned long long>(h));
        std::snprintf(buf, sizeof(buf), "skipping %016llx (%llu skipped so far)", static_cast<unsigned long long>(h),
                      static_cast<unsigned long long>(g_skipped.load()));
        return buf;
    }
    if (sub == "dump" && !arg.empty()) {
        std::lock_guard lock(g_mutex);
        int n = 0;
        for (auto& [h, draws] : g_last) {
            auto it = g_bytecode.find(h);
            if (it == g_bytecode.end()) continue;
            std::snprintf(buf, sizeof(buf), "%s\\vs_%016llx.dxbc", arg.c_str(), static_cast<unsigned long long>(h));
            FILE* f = nullptr;
            if (fopen_s(&f, buf, "wb") == 0 && f) {
                fwrite(it->second.data(), 1, it->second.size(), f);
                fclose(f);
                ++n;
            }
        }
        return "dumped " + std::to_string(n) + " vertex shaders";
    }
    // census
    int n = std::atoi(arg.c_str());
    if (n <= 0) n = 40;
    std::vector<std::pair<uint64_t, uint64_t>> v;
    {
        std::lock_guard lock(g_mutex);
        v.assign(g_last.begin(), g_last.end());
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::snprintf(buf, sizeof(buf), "pipelines: graphics %llu, stream %llu (unhashed %llu); %zu vertex shaders drew last frame:",
                  static_cast<unsigned long long>(g_graphics.load()), static_cast<unsigned long long>(g_stream.load()),
                  static_cast<unsigned long long>(g_unhashed.load()), v.size());
    std::string out = buf;
    for (int i = 0; i < n && i < static_cast<int>(v.size()); ++i) {
        std::snprintf(buf, sizeof(buf), " %016llx:%llu", static_cast<unsigned long long>(v[i].first),
                      static_cast<unsigned long long>(v[i].second));
        out += buf;
    }
    return out;
}

}  // namespace rdrvr::pso
