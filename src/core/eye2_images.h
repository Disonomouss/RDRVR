#pragma once
// [Stereo] PrepassEye2Images (run 23 item 3, research\run23\images.md section 5, the allocator route; test only, off by
// default; needs [Stereo] PrepassEye2Collector=1 and PrepassEye2Light=1 installed and usable). Observe only: P3's third step,
// eye 2's two images (since item 6b eye2_light's SC2L and LB2; before it eye2_collector's SC2) given bindless indices by the game's own descriptor allocator
// and written into global_textures' slack, so a later shader may read them. No shader reads them yet; nothing the game draws
// changes. The uints (images.md 9.6), pinned for the session and for the converter: uint 58 the Light Buffer, uint 57 the
// collector (the free ones nearest the design's 59 / 61 once the effect files' candidates 59..63 are avoided). Every pass 1
// the whole run-time global variable table is rescanned: an entry of any type or flags whose register's SetGlobalTexture
// write (index r - 150, sampler r - 126; FUN_140149140 writes global_textures for any id, unchecked) lands on 58 or 57 is a
// refusal before the grow and a stand-down after it; the pair never moves. Published as kv uint_lb= / uint_sc= (decimal
// masks claimed= runtime_claimed= static_claimed=).
//
//   the playback thread (an op-0x4a marker the render thread appends at pass 1's start, each double frame; where the two
//   modules make and retire their images): once per image (kept for the session, never freed) a record from DescAlloc
//   FUN_140f732b0(dev, &rec, 0 CBV_SRV_UAV, 1 BINDLESS_TEXTURE_2D, name, 1), after the partition's headroom check (capacity
//   dev+0x248 - used dev+0x258 >= 64: the allocator's overflow is a deliberate crash inside the device CS); the record's
//   partition index (+0x14) is the image's bindless index. Then ID3D12Device::CreateShaderResourceView of the image into the
//   record's CPU handle (+0x18): its own format (a typeless one as the game's view: 87 / 91), TEXTURE2D, the default
//   mapping, one mip. A new image (its module's makes moved, or another resource) is written again into the same handle; an
//   image gone gets a null SRV there.
//   the render thread (dual_pass, pass 1's start): once, global_textures (DAT_142ac10b0) grown 0xc0 -> 0x100 (prepass::grow_cb:
//   its storage swapped for a zeroed block, the CBV already 256 B), after the table's checks (two uints free as above; no
//   shader global of that CB at or past 0xc0; again every pass 1); each frame
//   cb_still_grown, then the two uints written with the two indices (both known, none the game's own uint 11 / 13, not equal)
//   and the CB's stamp +0x24 = -1, so its next commit uploads them. Off at run time, and once at a stand-down: 0 written (the
//   NullResource) and the stamp set.
//   the readback (the render thread, pass 1's scene end and pass 2's end): the CB's last upload (+0x24 = this frame's
//   RecorderStamp, not dirtied since), its CBV descriptor (+0x28, a GPU handle in partition 6 -> the CPU handle by the
//   partition's bases dev+0x380 / dev+0x378) -> the CBV's BufferLocation (ring_probe's CBV map) -> its bytes (ring_probe::
//   cpu_of): the LB uint = the LB index, the SC uint = the SC index, uints 0-47 = the storage's. Counted per pass.
// Every call into the game (DescAlloc) and every game read and write is under SEH; a fault stands the module down.
// "prepass e2img [on|off|reset|kv|table|guard on|guard off]"; table (read only, needs only the ini switch and the anchors, so it answers when the
// install refused or the module stood down): one log line per runtime table entry and a summary. With the ini switch off
// nothing is installed: dual_pass's calls check one relaxed flag.
//
// (run 25, GOAL-single-pass item 2; research\run24\c1.md section 3) The guard: uint 56 of global_textures, claimed as 58 / 57
// are (a run-time entry that could write it is a refusal before the grow and a stand-down after it). The converter's eye2img
// twins ([Stereo] PrepassEye2Shaders, vi_auto) read eye 2's uints only while uint 56 holds kGuardMagic (else eye 1's, uints
// 11 / 13: a fallback, not eye 2's images). The magic is written there (since item 6b: only at a frame's rise, below) only
// while the CB is grown and still the grown one, both SRVs view their images, the readback has seen the two indices in an
// upload since they last changed, the module is not stood down and "prepass e2img guard" is on; otherwise 0, written before
// the indices change. A stand-down writes 0 there first, then zeroes the indices.
// guard_valid() is the module's state, not the frame's: the conditions above held at the last pass 1's start and an upload
// confirmed a magic the module wrote; false at once on a stand-down, an SRV nulled, a readback that disagrees, a renderer
// reset, "guard off", and (item 6b) SC2L's or LB2's module not usable. Since item 6b it stays true on a stale frame and from
// pass 2's end to the next rise, while uint 56 is 0 and the twins fall back to eye 1's uints. (Item 6c) pso.cpp latches it with
// fresh_this_frame(): at pass 1's first pre-pass region for twin_of, at pass 1's walk's first draw for main_twin_of.
// "prepass e2img guard flip on|off" (6c's test, off by default): guard_valid() dropped at each pass 1's scene end.
//
// (GOAL-single-pass item 6b) uint 57 views eye2_light's SC2L (eye 2's final collector, after its footprints), not
// eye2_collector's SC2; with eye2_light (or eye2_collector) not usable the guard is not valid (no fallback). And a freshness
// bit per frame: uint 56 is written 0 at every pass 1's start and the magic only at that pass 1's rise, at the end of
// eye2_light's (L) (the render thread, after eye 2's lights were recorded between IN-L and OUT-L and eye 1's call returned;
// before SceneRender's MRT bind 0x5c3e8d, so before every commit of the main walk). The rise also needs this frame's start
// conditions (as above) and its start marker, and appends eye2_light's check marker: on the playback thread its step runs
// after this pass 1's OUT-L and before the walk's draws, and finds OUT-L's LB2 copy made with SC2L final, and both SRVs on
// those images of that generation. A frame without the rise uploads 0 at uint 56 (counted stale at pass 1's scene end). At
// pass 2's end (after its readback) uint 56 is written 0 again, so nothing after the frame (its UI, a mono frame) uploads it.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Resource;
struct D3D12_SHADER_RESOURCE_VIEW_DESC;

namespace rdrvr::eye2_images {

// the pinned uints of global_textures (vi_auto static_asserts protocol.h's --eye2-images defaults against these)
inline constexpr int kUintLb = 58, kUintSc = 57, kUintGuard = 56;
inline constexpr uint32_t kGuardMagic = 0x32455652u;  // "RVE2" (protocol.h kEye2GuardMagic)

extern std::atomic<bool> g_on_flag;
inline bool on() { return g_on_flag.load(std::memory_order_relaxed); }
// the module's state (any thread; one atomic load): the guard's conditions hold and an upload confirmed the magic. NOT this
// frame's: it stays true on a stale frame and from pass 2's end to the next rise, while uint 56 is 0 (see above)
bool guard_valid();
// (run 25, the family compare's e2img-off control, families.md 3.4; test only) the render thread, pass 1's DVL call 1: zero true
// writes 0 over the risen guard (uint 56) and re-arms the CB, so the walk's draws upload 0 while guard_valid() (the latch's
// answer) is kept; zero false puts the magic back. False when nothing was done (no risen guard this frame, or a fault)
bool guard_zero(bool zero);
// (item 6b, for 6c's latch) this frame's eye-2 images fresh, i.e. uint 56 holds the magic for the walk's uploads and SC2L and
// LB2 are this frame's. Per thread:
//   the render thread: its current frame (pass 1s begun) rose; true from the rise to the next pass 1's start;
//   the playback thread: the frame it plays (the seq its start marker carried), and that frame's check (carrying the same
//     frame's slot, after its OUT-L) found SC2L and LB2 written and the SRVs on them; false from that frame's start marker
//     until the check, and false for a frame whose start marker was not appended (it never reads a previous frame's answer);
//   any other thread: false.
// 6c must hand out an eye2img twin only where fresh_this_frame() AND guard_valid() hold, both read on the thread that hands
// it out (pso's twin_of: the playback thread), latched for the frame.
// (the 6b delta review) the bit is not cleared at pass 2's end: it stays true through pass 2, the UI and a mono frame
// until the next pass 1's start. 6c must consult it only inside pass 1's main walk (where uint 56 also holds the magic)
bool fresh_this_frame();
// (run 25) the module's state for [Stereo] PrepassEye2Shaders' status: 1 installed and up, 0 not installed (yet, or refused at
// install), -1 stood down or its grow refused (for the session)
int health();

void install();  // bootstrap, after eye2_light::install()
// dual_pass, the render thread: 1 pass 1 begins, 2 pass 1's scene ends (before eye 1's post), 4 pass 2 ends
void mark(int point);
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "e2img"

// ---- the allocator route, shared (run 25 item 5: [Stereo] MainSlice's four SRVs, research\run25\slice.md 2.2). Needs only the
// anchors DescAlloc and D3dDeviceSingleton (not this module installed). alloc_p1: one partition-1 (BINDLESS_TEXTURE_2D)
// record from the game's DescAlloc, after the partition's headroom check (>= 64 free: the allocator's overflow is a crash inside
// the device CS), under SEH (a fault leaves the device CS down to the recursion it had); the record checked as analysed
// (type 0, partition 1, the device's increment, index > 0 and under the capacity, CPU = partition 1's base + index x increment).
// The record is the session's (never freed). `label` names it in the reasons. *read: the partition's counts were read (then
// *used / *cap are set). create_srv_at: ID3D12Device::CreateShaderResourceView into a record's CPU handle, under SEH.
struct DescRec {  // the record (0x28 B)
    int32_t type, partition, inc, size;
    int32_t heap_index;  // +0x10 the partition's base index + its slot
    int32_t index;       // +0x14 the slot in the partition: the bindless index
    uint64_t cpu, gpu;   // +0x18, +0x20
};
enum class Alloc : int { kOk, kRefused, kFault, kBadRecord };  // refused: try again later; a fault or a bad record: stand down
Alloc alloc_p1(const char* name, const char* label, DescRec* out, char* why, size_t why_len, bool* read, uint64_t* used, uint64_t* cap);
bool create_srv_at(ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* s, uint64_t cpu);

}  // namespace rdrvr::eye2_images
