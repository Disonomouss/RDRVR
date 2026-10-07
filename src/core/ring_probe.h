#pragma once
// R0 measurement for Spike S1's open runtime checks (ENGINE-NOTES 1.2): how far the game's constant upload ring
// advances per frame, and how full the occlusion-query pool gets. A second eye pass roughly doubles both.
//
// Constant ring: renamed shader constants reach the GPU as root CBV addresses (SetGraphics/ComputeRootConstantBuffer
// View) or CBV descriptors (CreateConstantBufferView). Each address is mapped to the upload-heap buffer holding it
// (tracked from resource creation), and per buffer and frame the probe counts uses and the span they cover (lowest
// offset to highest end), so a per-frame allocator's footprint and headroom show directly. Occlusion pool: the head
// (queries allocated this frame, 0x2ac533c) and the per-frame capacity (0x2ac5554) are sampled at every Present.
// [Debug] RingProbe=1.

#include <cstddef>
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct D3D12_RESOURCE_DESC;

namespace rdrvr::ring_probe {

bool enabled();
void install_device_hooks(ID3D12Device* dev);            // CreateConstantBufferView
void install_list_hooks(ID3D12GraphicsCommandList* cl);  // root CBV binds
void on_upload_buffer(ID3D12Resource* res, const D3D12_RESOURCE_DESC* desc);  // from resource creation
void init();                                             // frame-end listener
void status_text(char* out, size_t len);

// R3's rage_matrices checker. While on, every CBV created in a tracked upload buffer is read from its CPU copy; a block
// whose bytes 0xC0..0xFF look like gViewInverse (three unit rows with w 0, then a position with w 1) is classified by
// that camera position against the current pass's expectation: the pass's eye, the other eye, the centre (mono/cover)
// camera, or another view (shadows, reflections, imposters). The camera lever sets the expectation per scene pass.
void matrix_check(bool on);
void matrix_expect(int pass, const float* eye, const float* other, const float* centre);  // pass -1 ends a pass
void matrix_text(char* out, size_t len);
uint64_t thread_cbvs();
// The CPU copy of `bytes` at GPU address `va` when it lies in a tracked (persistently mapped) upload buffer, else null.
const uint8_t* cpu_of(uint64_t va, uint64_t bytes);  // CBVs the calling thread has created so far (the drawlog's per-call attribution)

}  // namespace rdrvr::ring_probe
