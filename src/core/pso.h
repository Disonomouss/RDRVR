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
// The census on from now (the frame graph arms it); the list's current pipeline as the census knows it: its vertex and
// pixel shaders' hashes and what it writes (bits 0-7 the render targets, bit 8 depth). False: not known (another list).
void arm_census();
bool current(const ID3D12GraphicsCommandList* cl, uint64_t* vs, uint64_t* ps, uint32_t* writes);
// [Stereo] InstancedShaders=<folder> (single-pass stereo, the pre-pass's milestone 4a step 1; test only, off; 2026-10-14):
// for each game pipeline whose vertex shader has a rewritten vs_<16 hex fnv>.dxil there and whose "vs ps" pair is a line
// of the folder's twins.txt (research\sps\tools\vi\prepass_twins.py's ready list), a view-instanced twin is made next
// to it (2 views on viewport 0, render target slices 0 and 1, masking on: psotest's array0 desc, 441 of 441 made offline
// on the developer's driver). Nothing draws with them yet. nullptr: no twin.
ID3D12PipelineState* twin_of(ID3D12PipelineState* game);
std::string twins_status();
uint64_t twins_made();
// The list's pipeline set without the census or the pre-pass's notes (the instanced pre-pass swaps twins in and out).
void set_raw(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps);

}  // namespace rdrvr::pso
