// Single-pass stereo, the converter in C++, step B (run 18 item 3; fork-port-plan.md §5.2): the rewrite of a shader's
// disassembly (dumpbin text) into its view-instanced twin's text, refusing by default. See vi_rewrite.cpp.
#pragma once
#include <string>

namespace vi {
struct Rewrite {
    bool ok = false;      // rewritten: text holds the twin's IR text
    std::string text;
    std::string refused;  // the reason, when not ok (as the Python's Refused; "strict: ..." for this port's own checks)
    int n = 0;            // the row reads redirected (the Python's n, for the report)
    bool approx = false;  // motion vectors through D (rage_prev_world / rage_instancemtxprev)
};
// text: the disassembly as stereo_vs.py sees it ((stdout + stderr).strip(), LF line ends)
// our_layout: refuse ("strict: needs the fork's layout ...") a rewrite that reads what only the fork's runtime writes
// (rage_matrices rows 29-31, the .w index fields of rows 25-27 and 30, the eye-2 descriptor slots from 14960, the D
// matrix in rows 21-24, the velocity pass's $Globals padding); the mod's 0x1d0 layout has rows 17-28 as eye 2's rows
// 4-15 only. Off (the default) is the chain's output, byte for byte.
Rewrite rewrite(const std::string& text, bool our_layout = false);
// the mod's own rewrite (research/sps/tools/vi/vi_rewrite.py, its default "dyn" mode, with its ViewID state: the P0
// twins, built with no -viewid-state pass): each per-eye row r read as r + ViewID x delta; refused when not understood,
// when a per-eye block is read by a dynamic row, or when a camera-dependent member is read that it has no eye-2 copy for;
// a shader with nothing per-eye in it is "no view-dependent block" (used as it is). wvp: it reads gWorldViewProj (eye 2's
// copy is per draw: [Stereo] PrepassWvpMirror)
// our_layout: also refuse a read of rage_prev_world rows 0-3 (eye 2's at rows 5-8: written only by [Stereo]
// PrepassPrevWorldMirror; allowed with prevworld, below)
// instprev_k (run 19 item 2, `--instprev K`; 0, the default, refuses rage_instancemtxprev as before, byte for byte): each
// rage_instancemtxprev load reads row + ViewID x 4K (eye 2's entry i at K + i, written per draw by [Stereo]
// PrepassInstPrevMirror with the same K), the block's declared size grows from N x 64 to (K + N) x 64 (its type and its
// dx.resources entry; at most 64 KB), and the loads' results are ViewID-dependent (dx.viewIdState). *instprev_loads: how
// many loads were redirected (0: the shader does not read the block)
// e2 (run 24, `--eye2-images LB2,SC2[,GUARD]`, P3's read side, route (c1) of research/run20/p3-textures.md §2(c); off,
// the default: byte for byte as before): in a pixel shader, each global_textures index of the Light Buffer (uint 11,
// LightBufferSampler) and of the Shadow Collector (uint 13, ShadowCollectorSampler) read by cbufferLoadLegacy.i32 +
// extractvalue becomes `(ViewID != 0 && uint GUARD == kEye2GuardMagic) ? uint LB2 : uint 11` (SC2 for 13), loaded from
// the same handle; the guard is read once in the entry block; the texture handles that take the select are made
// non-uniform (i1 true: both views' pixels may share a wave); global_textures' declared size grows to 256 B (its type
// and its dx.resources entry). Refused ("eye-2 images: ...") unless every read that can reach uint 11 or 13 is that
// plain form and every use of the index is a texture handle's index. A shader whose reads never reach them is converted
// byte for byte as without e2. *eye2_loads: the indices rewritten (0: the shader reads neither)
struct Eye2Images {
    int lb = -1, sc = -1, guard = -1;  // the three uints (all set, or e2 is off)
    bool on() const { return lb >= 0 || sc >= 0 || guard >= 0; }
};
// prevworld (run 26, GOAL-single-pass item 6a, `--prevworld`; off, the default: byte for byte as before): under our_layout,
// a read of rage_prev_world rows 0-3 (gPrevWorldViewProjSingle) is no longer refused; it takes the rule's existing row map,
// row + ViewID x 5, so view 1 reads rows 5-8, the rows [Stereo] PrepassPrevWorldMirror writes per draw at +0x50 of the CB it
// grows 0x50 -> 0x90 (src/core/prevworld_mirror.cpp). Refused ("strict: ...") unless rage_prev_world's layout is the
// analysed one (gPrevWorldViewProjSingle rows 0-3, gViewport row 4, 80 bytes), every row-0-3 load is static and rewritten,
// and no load reads past row 4. *prevworld_loads: the loads redirected (0: the shader does not read rows 0-3, its output
// byte for byte as without the flag). A pixel shader is never given it (main.cpp)
Rewrite rewrite_ours(const std::string& text, bool* wvp = nullptr, bool our_layout = false, int instprev_k = 0, int* instprev_loads = nullptr,
                     const Eye2Images& e2 = Eye2Images{}, int* eye2_loads = nullptr, bool prevworld = false, int* prevworld_loads = nullptr);
// the uints --eye2-images accepts: past the game's 48 (0-23 the indices, 24-47 the samplers) and below 59 (59-63: the
// sampler-state globals t209..t213 can write them, run 23 images.md §9.5), inside the 256-B grown block; three different
inline constexpr int kEye2UintLo = 48, kEye2UintHi = 58, kEye2GrownBytes = 256;
// the guard word's value (protocol.h): the twin takes eye 2's indices only while uint GUARD holds it
inline constexpr unsigned kEye2GuardMagic = 0x32455652u;  // ("RVE2" in memory order)
// the entry's name (stereo_vs.py's ^define void @(\w+)\(), or "" when there is none
std::string entry_name(const std::string& text);
}  // namespace vi
