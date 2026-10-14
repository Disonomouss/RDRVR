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

#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/prepass_arrays.h"
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
// The per-draw census (draws per vertex shader, the skips, the vertex capture) runs only after the first "pso" test
// command (2026-10-11, performance for weaker PCs: a mutex and a map update on every draw and pipeline set on the
// playback thread, about 12,000 a frame). [Debug] PsoCensus=1 runs it from the start.
std::atomic<bool> g_census{false};
std::atomic<uint64_t> g_skip{0}, g_skipped{0}, g_graphics{0}, g_stream{0}, g_unhashed{0};
thread_local ID3D12GraphicsCommandList* t_cl = nullptr;
thread_local uint64_t t_vs = 0;
// The frame graph's shader census (single-pass stereo, the pre-pass census): per graphics pipeline its pixel shader's
// hash and what it writes (bits 0-7: render target i written, bit 8: depth written); the list's current one.
struct PsoInfo {
    uint64_t ps = 0;
    uint32_t writes = 0;
};
std::unordered_map<void*, PsoInfo> g_info;  // under g_mutex
thread_local PsoInfo t_info;

// ---- [Stereo] InstancedShaders (2026-10-14, test only): the view-instanced twins (pso.h twin_of)
std::unordered_map<uint64_t, std::vector<uint8_t>> g_vi_vs;  // vertex shader hash -> its rewrite (read-only after load)
std::unordered_set<std::string> g_vi_pairs;                  // "vs ps" (16 hex each) allowed a twin (read-only after load)
std::unordered_map<void*, ID3D12PipelineState*> g_twin;      // the game's pipeline -> its twin, under g_mutex
std::atomic<uint64_t> g_twins_made{0}, g_twins_failed{0}, g_twins_skipped{0};
std::atomic<int64_t> g_twin_ms_max{0};
// run 12 item 12a, "pso ps reset|dump <file>" (single-pass stereo's reader census, a test's): every pixel shader drawn
// since the reset, by its hash (fnv of its bytecode, as the corpus's blobs hash offline), with its draws, a vertex
// shader it drew with and what it writes; per scene, joined offline to the corpus's screen-target samplers
struct PsAcc {
    uint64_t draws = 0, vs = 0;
    uint32_t writes = 0;
};
std::unordered_map<uint64_t, PsAcc> g_ps_acc;  // under g_mutex
std::atomic<bool> g_ps_acc_on{false};

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

// [Debug] ShaderSwap=<folder> (single-pass stereo step 4, test only): at the start, every <16 hex digits>.dxil file there
// is a replacement for the vertex shader of that hash (fnv of its bytecode, as "pso dump" names them); a pipeline made
// with that vertex shader is made with the replacement instead, or with the game's own if that fails. Read-only map
// after load_swaps().
std::unordered_map<uint64_t, std::vector<uint8_t>> g_swap;
std::atomic<uint64_t> g_swaps{0}, g_swap_fails{0};
void load_swaps() {
    const std::string dir = config::get_string("Debug", "ShaderSwap", "");
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*.dxil").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log::warn("[pso] ShaderSwap: no .dxil files in %s", dir.c_str());
        return;
    }
    do {
        const std::string name = fd.cFileName;
        if (name.size() != 16 + 5) continue;
        const uint64_t hash = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        FILE* f = nullptr;
        if (!hash || fopen_s(&f, (dir + "\\" + name).c_str(), "rb") != 0 || !f) continue;
        std::vector<uint8_t> b(fd.nFileSizeLow);
        const size_t got = fread(b.data(), 1, b.size(), f);
        fclose(f);
        if (got == b.size() && b.size() > 32 && std::memcmp(b.data(), "DXBC", 4) == 0) g_swap.emplace(hash, std::move(b));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    log::info("[pso] ShaderSwap: %zu replacement vertex shaders from %s", g_swap.size(), dir.c_str());
}
const std::vector<uint8_t>* swap_for(const D3D12_SHADER_BYTECODE& vs) {
    if (g_swap.empty() || !vs.pShaderBytecode || !vs.BytecodeLength) return nullptr;
    auto it = g_swap.find(fnv(vs.pShaderBytecode, vs.BytecodeLength));
    return it == g_swap.end() ? nullptr : &it->second;
}

void load_twin_sources() {
    const std::string dir = config::get_string("Stereo", "InstancedShaders", "");
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\vs_*.dxil").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = fd.cFileName;  // vs_<16 hex>.dxil
            if (name.size() != 3 + 16 + 5) continue;
            const uint64_t hash = std::strtoull(name.substr(3, 16).c_str(), nullptr, 16);
            FILE* f = nullptr;
            if (!hash || fopen_s(&f, (dir + "\\" + name).c_str(), "rb") != 0 || !f) continue;
            std::vector<uint8_t> b(fd.nFileSizeLow);
            const size_t got = fread(b.data(), 1, b.size(), f);
            fclose(f);
            if (got == b.size() && b.size() > 32 && std::memcmp(b.data(), "DXBC", 4) == 0) g_vi_vs.emplace(hash, std::move(b));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "\\twins.txt").c_str(), "r") == 0 && f) {
        char line[256], a[64] = {}, b[64] = {};
        while (std::fgets(line, sizeof(line), f))  // "vs ps" per line; '#' lines and anything else skipped
            if (line[0] != '#' && sscanf_s(line, "%63s %63s", a, static_cast<unsigned>(sizeof(a)), b, static_cast<unsigned>(sizeof(b))) == 2 &&
                std::strlen(a) == 16 && std::strlen(b) == 16)
                g_vi_pairs.insert(std::string(a) + " " + b);
        fclose(f);
    }
    log::info("[pso] InstancedShaders: %zu rewritten vertex shaders and %zu twin pairs from %s (test only: twins made, not drawn with)",
              g_vi_vs.size(), g_vi_pairs.size(), dir.c_str());
}

// a stream subobject, as research\sps\tools\vi\psotest.cpp builds it (its 441 twins made offline): the type at an
// 8-aligned offset, the value at +8 when it holds pointers (else +4), the whole rounded to 8
template <typename T>
void push_sub(std::vector<uint8_t>& s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& v, bool ptr) {
    const size_t off = (s.size() + 7) & ~size_t(7);
    s.resize(off, 0);
    const size_t inner = ptr ? 8 : 4;
    s.resize(off + ((inner + sizeof(T) + 7) & ~size_t(7)), 0);
    std::memcpy(s.data() + off, &type, 4);
    std::memcpy(s.data() + off + inner, &v, sizeof(T));
}

// A pipeline made at an address a released one had: its old twin forgotten (leaked, as the GPU may still use it), so
// twin_of never hands back another pipeline's twin (2026-10-14, the review).
void forget_twin(void* game) {
    if (g_vi_vs.empty() || !game) return;
    std::lock_guard lock(g_mutex);
    g_twin.erase(game);
}
void make_twin(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, ID3D12PipelineState* game) {
    if (g_vi_vs.empty() || !d->VS.pShaderBytecode || !d->VS.BytecodeLength || !d->pRootSignature) return;
    const uint64_t vs = fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength);
    const uint64_t ps = d->PS.pShaderBytecode && d->PS.BytecodeLength ? fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength) : 0;
    auto it = g_vi_vs.find(vs);
    if (it == g_vi_vs.end()) return;
    char key[40];
    std::snprintf(key, sizeof(key), "%016llx %016llx", static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps));
    if (!g_vi_pairs.count(key)) {
        g_twins_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (d->StreamOutput.NumEntries || d->DS.BytecodeLength || d->HS.BytecodeLength || d->GS.BytecodeLength) {
        g_twins_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ID3D12Device2* d2 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d2))) || !d2) return;
    std::vector<uint8_t> st;
    ID3D12RootSignature* rs = d->pRootSignature;
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, rs, true);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE{it->second.data(), it->second.size()}, true);
    if (ps) push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, d->PS, true);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, d->BlendState, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, d->SampleMask, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, d->RasterizerState, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, d->DepthStencilState, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, d->InputLayout, true);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, d->IBStripCutValue, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, d->PrimitiveTopologyType, false);
    D3D12_RT_FORMAT_ARRAY fa{};
    fa.NumRenderTargets = d->NumRenderTargets;
    for (int k = 0; k < 8; ++k) fa.RTFormats[k] = d->RTVFormats[k];
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, fa, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, d->DSVFormat, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, d->SampleDesc, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, d->NodeMask, false);
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, d->Flags, false);
    static const D3D12_VIEW_INSTANCE_LOCATION kLoc[2] = {{0, 0}, {0, 1}};  // (viewport 0, slices 0 and 1: psotest's array0)
    const D3D12_VIEW_INSTANCING_DESC vi{2, kLoc, D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING};
    push_sub(st, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, vi, true);
    const D3D12_PIPELINE_STATE_STREAM_DESC sd{st.size(), st.data()};
    ID3D12PipelineState* twin = nullptr;
    const DWORD t0 = GetTickCount();
    // the original stream creation (not the hook: no ShaderSwap, no census of the twin)
    const HRESULT hr = o_CreateStream ? o_CreateStream(d2, &sd, IID_PPV_ARGS(&twin)) : d2->CreatePipelineState(&sd, IID_PPV_ARGS(&twin));
    const int64_t ms = static_cast<int64_t>(GetTickCount() - t0);
    d2->Release();
    if (ms > g_twin_ms_max.load()) g_twin_ms_max = ms;
    if (FAILED(hr) || !twin) {
        g_twins_failed.fetch_add(1, std::memory_order_relaxed);
        log::warn("[pso] twin of vs %016llx ps %016llx refused (%#lx): that pipeline stays single", static_cast<unsigned long long>(vs),
                  static_cast<unsigned long long>(ps), static_cast<unsigned long>(hr));
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        g_twin.insert_or_assign(game, twin);  // (an old entry, if any, leaked: see forget_twin)
    }
    if (g_twins_made.fetch_add(1, std::memory_order_relaxed) < 3)
        log::info("[pso] twin made: vs %016llx ps %016llx (%lld ms)", static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
                  static_cast<long long>(ms));
}

// [Debug] PsoDump=<folder> (single-pass stereo step 5, milestone 1, test only): every graphics pipeline the game makes
// written to <folder>\\pso_<n>.bin (its desc, its shaders and its root signature's serialized blob), for creating its
// view-instanced twin outside the game (research\\sps\\tools\\vi\\psotest.cpp). The root signatures' blobs come
// from a CreateRootSignature hook. Format "PSO1", little-endian: each field below in order, blobs as u32 length + bytes.
using CreateRootSig_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
CreateRootSig_t o_CreateRootSig = nullptr;
std::string g_dump_dir;
std::mutex g_dump_mutex;
std::unordered_map<void*, std::vector<uint8_t>> g_root_blobs;  // under g_dump_mutex
int g_dumped = 0;
HRESULT STDMETHODCALLTYPE hk_CreateRootSig(ID3D12Device* dev, UINT mask, const void* blob, SIZE_T len, REFIID riid, void** out) {
    const HRESULT hr = o_CreateRootSig(dev, mask, blob, len, riid, out);
    if (SUCCEEDED(hr) && out && *out && blob && len) {
        std::lock_guard lock(g_dump_mutex);
        const uint8_t* b = static_cast<const uint8_t*>(blob);
        g_root_blobs[*out] = std::vector<uint8_t>(b, b + len);
    }
    return hr;
}
void put_blob(FILE* f, const void* p, size_t n) {
    const uint32_t len = static_cast<uint32_t>(p ? n : 0);
    fwrite(&len, 4, 1, f);
    if (len) fwrite(p, 1, len, f);
}
void dump_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    std::vector<uint8_t> root;
    int n = 0;
    {
        std::lock_guard lock(g_dump_mutex);
        auto it = g_root_blobs.find(d->pRootSignature);
        if (it != g_root_blobs.end()) root = it->second;
        n = g_dumped++;
    }
    char path[600];
    std::snprintf(path, sizeof(path), "%s\\pso_%04d.bin", g_dump_dir.c_str(), n);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    fwrite("PSO1", 1, 4, f);
    put_blob(f, d->VS.pShaderBytecode, d->VS.BytecodeLength);
    put_blob(f, d->PS.pShaderBytecode, d->PS.BytecodeLength);
    put_blob(f, d->DS.pShaderBytecode, d->DS.BytecodeLength);
    put_blob(f, d->HS.pShaderBytecode, d->HS.BytecodeLength);
    put_blob(f, d->GS.pShaderBytecode, d->GS.BytecodeLength);
    put_blob(f, root.data(), root.size());
    const uint32_t so = d->StreamOutput.NumEntries;
    fwrite(&so, 4, 1, f);
    fwrite(&d->BlendState, sizeof(d->BlendState), 1, f);
    fwrite(&d->SampleMask, 4, 1, f);
    fwrite(&d->RasterizerState, sizeof(d->RasterizerState), 1, f);
    fwrite(&d->DepthStencilState, sizeof(d->DepthStencilState), 1, f);
    const uint32_t ne = d->InputLayout.pInputElementDescs ? d->InputLayout.NumElements : 0;
    fwrite(&ne, 4, 1, f);
    for (uint32_t i = 0; i < ne; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = d->InputLayout.pInputElementDescs[i];
        put_blob(f, e.SemanticName, e.SemanticName ? std::strlen(e.SemanticName) : 0);
        const uint32_t v[6] = {e.SemanticIndex, static_cast<uint32_t>(e.Format), e.InputSlot, e.AlignedByteOffset,
                               static_cast<uint32_t>(e.InputSlotClass), e.InstanceDataStepRate};
        fwrite(v, 4, 6, f);
    }
    const uint32_t tail[5] = {static_cast<uint32_t>(d->IBStripCutValue), static_cast<uint32_t>(d->PrimitiveTopologyType), d->NumRenderTargets,
                              static_cast<uint32_t>(d->DSVFormat), d->NodeMask};
    fwrite(tail, 4, 5, f);
    for (int i = 0; i < 8; ++i) {
        const uint32_t fmt = static_cast<uint32_t>(d->RTVFormats[i]);
        fwrite(&fmt, 4, 1, f);
    }
    const uint32_t sd[3] = {d->SampleDesc.Count, d->SampleDesc.Quality, static_cast<uint32_t>(d->Flags)};
    fwrite(sd, 4, 3, f);
    const uint64_t vs_hash = d->VS.pShaderBytecode && d->VS.BytecodeLength ? fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength) : 0;
    fwrite(&vs_hash, 8, 1, f);
    fclose(f);
}

void record_info(void* pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    PsoInfo in;
    in.ps = d->PS.pShaderBytecode && d->PS.BytecodeLength ? fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength) : 0;
    const UINT n = d->NumRenderTargets < 8 ? d->NumRenderTargets : 8;
    for (UINT i = 0; i < n; ++i) {
        const auto& bt = d->BlendState.RenderTarget[d->BlendState.IndependentBlendEnable ? i : 0];
        if (bt.RenderTargetWriteMask && d->RTVFormats[i] != DXGI_FORMAT_UNKNOWN) in.writes |= 1u << i;
    }
    if (d->DepthStencilState.DepthEnable && d->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL) in.writes |= 1u << 8;
    std::lock_guard lock(g_mutex);
    g_info[pso] = in;
}

HRESULT STDMETHODCALLTYPE hk_CreateGraphics(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** out) {
    if (d && !g_dump_dir.empty()) dump_pso(d);
    if (d && out) {
        if (const std::vector<uint8_t>* r = swap_for(d->VS)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d2 = *d;
            d2.VS = {r->data(), r->size()};
            const HRESULT hs = o_CreateGraphics(dev, &d2, riid, out);
            if (SUCCEEDED(hs) && *out) {
                forget_twin(*out);
                g_swaps.fetch_add(1, std::memory_order_relaxed);
                g_graphics.fetch_add(1, std::memory_order_relaxed);
                record(*out, d->VS);  // counted under the game's own hash
                record_info(*out, d);
                return hs;
            }
            g_swap_fails.fetch_add(1, std::memory_order_relaxed);
            log::warn("[pso] ShaderSwap: the replacement for %016llx refused (%#lx): the game's own used",
                      static_cast<unsigned long long>(fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength)), static_cast<unsigned long>(hs));
        }
    }
    HRESULT hr = o_CreateGraphics(dev, d, riid, out);
    if (SUCCEEDED(hr) && d && out && *out) {
        g_graphics.fetch_add(1, std::memory_order_relaxed);
        record(*out, d->VS);
        record_info(*out, d);
        if (!g_vi_vs.empty()) {  // [Stereo] InstancedShaders
            forget_twin(*out);
            make_twin(dev, d, static_cast<ID3D12PipelineState*>(*out));
        }
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
    if (!g_swap.empty() && d && out && d->pPipelineStateSubobjectStream && d->SizeInBytes) {  // [Debug] ShaderSwap: a copy of the stream
        std::vector<uint8_t> copy(static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream),
                                  static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream) + d->SizeInBytes);
        uint8_t* p = copy.data();
        uint8_t* end = p + copy.size();
        D3D12_SHADER_BYTECODE orig{};
        const std::vector<uint8_t>* r = nullptr;
        while (p + 4 <= end) {
            UINT type = *reinterpret_cast<const UINT*>(p);
            size_t inner = 0;
            bool ptr = false;
            if (!subobject(type, &inner, &ptr)) break;
            size_t off = ptr ? 8 : 4;
            if (type == 1) {
                auto* bc = reinterpret_cast<D3D12_SHADER_BYTECODE*>(p + off);
                orig = *bc;
                if ((r = swap_for(*bc)) != nullptr) *bc = {r->data(), r->size()};
                break;
            }
            p += (off + inner + 7) & ~size_t(7);
        }
        if (r) {
            D3D12_PIPELINE_STATE_STREAM_DESC d2{copy.size(), copy.data()};
            const HRESULT hs = o_CreateStream(dev, &d2, riid, out);
            if (SUCCEEDED(hs) && *out) {
                forget_twin(*out);
                g_swaps.fetch_add(1, std::memory_order_relaxed);
                g_stream.fetch_add(1, std::memory_order_relaxed);
                record(*out, orig);
                return hs;
            }
            g_swap_fails.fetch_add(1, std::memory_order_relaxed);
            log::warn("[pso] ShaderSwap: a stream replacement refused (%#lx): the game's own used", static_cast<unsigned long>(hs));
        }
    }
    HRESULT hr = o_CreateStream(dev, d, riid, out);
    if (SUCCEEDED(hr) && d && out && *out && d->pPipelineStateSubobjectStream) {
        forget_twin(*out);
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
    if (prepass::active()) prepass::note_pipeline(cl, ps);  // ([Stereo] PrepassInstanced: the game's pipeline in a region)
    if (!g_census.load(std::memory_order_relaxed)) return o_SetPipelineState(cl, ps);
    uint64_t h = 0;
    PsoInfo in;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_vs_of.find(ps);
        if (it != g_vs_of.end()) h = it->second;
        auto ii = g_info.find(ps);
        if (ii != g_info.end()) in = ii->second;
    }
    t_cl = cl;
    t_vs = h;
    t_info = in;
    o_SetPipelineState(cl, ps);
}

void STDMETHODCALLTYPE hk_SetVB(ID3D12GraphicsCommandList* cl, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    if (g_census.load(std::memory_order_relaxed) && start == 0 && n >= 1 && v) {
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
    if (!g_census.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mutex);
    g_last.swap(g_draws);
    g_draws.clear();
}

}  // namespace

void arm_census() {
    if (!g_census.exchange(true)) log::info("[pso] the draw census started (the frame graph)");
}

bool current(const ID3D12GraphicsCommandList* cl, uint64_t* vs, uint64_t* ps, uint32_t* writes) {
    if (cl != t_cl) return false;
    *vs = t_vs;
    *ps = t_info.ps;
    *writes = t_info.writes;
    return true;
}

void install_device_hooks(ID3D12Device* dev) {
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        load_swaps();
        load_twin_sources();
        if (config::get_bool("Debug", "PsoCensus", false)) g_census = true;
        g_dump_dir = config::get_string("Debug", "PsoDump", "");
        if (!g_dump_dir.empty()) {
            CreateDirectoryA(g_dump_dir.c_str(), nullptr);
            hooks::install("ID3D12Device::CreateRootSignature", hooks::vtable_entry(dev, 16), hk_CreateRootSig, &o_CreateRootSig);
            log::info("[pso] PsoDump: the game's pipelines written to %s", g_dump_dir.c_str());
        }
    }
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
    if (!g_census.load(std::memory_order_relaxed)) return false;
    uint64_t h = cl == t_cl ? t_vs : 0;
    if (cl == t_cl && t_info.ps && g_ps_acc_on.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_mutex);
        PsAcc& e = g_ps_acc[t_info.ps];
        ++e.draws;
        e.vs = h;
        e.writes = t_info.writes;
    }
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

uint64_t twins_made() { return g_twins_made.load(std::memory_order_relaxed); }
void set_raw(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    if (o_SetPipelineState) o_SetPipelineState(cl, ps);
    else cl->SetPipelineState(ps);
}
ID3D12PipelineState* twin_of(ID3D12PipelineState* game) {
    if (g_twins_made.load(std::memory_order_relaxed) == 0) return nullptr;
    std::lock_guard lock(g_mutex);
    auto it = g_twin.find(game);
    return it == g_twin.end() ? nullptr : it->second;
}
std::string twins_status() {
    char b[300];
    std::snprintf(b, sizeof(b), "twins %s: %zu rewritten VS, %zu pairs listed; made %llu, refused %llu, not listed %llu; slowest %lld ms",
                  g_vi_vs.empty() ? "off" : "on", g_vi_vs.size(), g_vi_pairs.size(), static_cast<unsigned long long>(g_twins_made.load()),
                  static_cast<unsigned long long>(g_twins_failed.load()), static_cast<unsigned long long>(g_twins_skipped.load()),
                  static_cast<long long>(g_twin_ms_max.load()));
    return b;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string word, sub, arg;
    in >> word >> sub >> arg;
    if (sub == "twins") return twins_status();  // pso twins ([Stereo] InstancedShaders; before the census is armed)
    char buf[256];
    if (!g_census.exchange(true)) {
        Sleep(250);  // a few frames counted (a pipeline set before now counts from its next set)
        log::info("[pso] the draw census started");
    }
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
    if (sub == "ps") {  // pso ps reset | pso ps dump <file> | pso ps off (run 12 item 12a)
        std::string file;
        in >> file;
        if (arg == "reset") {
            {
                std::lock_guard lock(g_mutex);
                g_ps_acc.clear();
            }
            g_ps_acc_on = true;
            return "pixel shader tally reset (on)";
        }
        if (arg == "off") {
            g_ps_acc_on = false;
            return "pixel shader tally off";
        }
        if (arg == "dump" && !file.empty()) {
            FILE* f = nullptr;
            if (fopen_s(&f, file.c_str(), "w") != 0 || !f) return "ERROR cannot write " + file;
            std::lock_guard lock(g_mutex);
            uint64_t total = 0;
            for (auto& [h2, e] : g_ps_acc) {
                std::fprintf(f, "%016llx %016llx %03x %llu\n", static_cast<unsigned long long>(h2), static_cast<unsigned long long>(e.vs), e.writes,
                             static_cast<unsigned long long>(e.draws));
                total += e.draws;
            }
            std::fclose(f);
            return "dumped " + std::to_string(g_ps_acc.size()) + " pixel shaders (" + std::to_string(total) + " draws)";
        }
        std::lock_guard lock(g_mutex);
        return "pixel shader tally " + std::string(g_ps_acc_on.load() ? "on" : "off") + ", " + std::to_string(g_ps_acc.size()) + " pixel shaders";
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
    std::snprintf(buf, sizeof(buf), "pipelines: graphics %llu, stream %llu (unhashed %llu); swapped %llu (refused %llu); %zu vertex shaders drew last frame:",
                  static_cast<unsigned long long>(g_graphics.load()), static_cast<unsigned long long>(g_stream.load()),
                  static_cast<unsigned long long>(g_unhashed.load()), static_cast<unsigned long long>(g_swaps.load()),
                  static_cast<unsigned long long>(g_swap_fails.load()), v.size());
    std::string out = buf;
    for (int i = 0; i < n && i < static_cast<int>(v.size()); ++i) {
        std::snprintf(buf, sizeof(buf), " %016llx:%llu", static_cast<unsigned long long>(v[i].first),
                      static_cast<unsigned long long>(v[i].second));
        out += buf;
    }
    return out;
}

}  // namespace rdrvr::pso
