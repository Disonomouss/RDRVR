#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

// Pipeline census and per-shader draw skip (round 3b: find the shader that draws the distant trees that turn with the
// head). Graphics pipelines are recorded with a hash of their vertex shader's bytecode (FNV-1a 64 over the container,
// the same bytes as the .dxbc blobs under research\shaders); draws are counted per vertex shader each frame, and the
// draws of one vertex shader can be skipped. A test aid; it only reads and (when asked) skips draws.
namespace rdrvr::pso {

void install_device_hooks(ID3D12Device* dev);
void install_list_hooks(ID3D12GraphicsCommandList* cl);
// From the draw hooks: true = skip this draw. Counts the draw against the list's current pipeline.
bool on_draw(ID3D12GraphicsCommandList* cl);
// After a draw that was not skipped: its first vertex and count (for the vertex capture).
void on_draw_args(ID3D12GraphicsCommandList* cl, int64_t first_vertex, uint32_t count);
// "pso census [n]" / "pso skip <hash>|off" / "pso dump <dir>" (the vertex shaders drawn last frame, as .dxbc).
std::string command(const std::string& line);

}  // namespace rdrvr::pso
