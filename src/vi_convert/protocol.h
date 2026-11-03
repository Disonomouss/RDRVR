// Single-pass stereo, step C (run 19 item 4; research\sps\fork-port-plan.md §5): what rdrvr_vi_convert.exe and the
// mod (src/core/vi_auto.cpp) agree on. Shared by both builds, so the two can never disagree on the cache's folder.
#pragma once

namespace rdrvr::vi_protocol {

// The converter's version: the cache's second folder level (<cache>\<RDR.exe build>\<this>\). Change it whenever the
// rule, the build or the files change, so a newer converter never trusts an older one's results.
inline constexpr const char* kVersion = "c1-vi_rewrite-ours";
// The rule's sources hashed at CMake's configure (src/vi_convert/*: main.cpp, vi_rewrite.cpp/.h, this file), the cache's
// version folder's second part: an edit of the rule makes a new folder even when kVersion was not changed. "dev": a
// build outside CMake (research\sps\tools\vi_convert\build.bat), never the mod's.
#ifndef RDRVR_VI_RULE_HASH
#define RDRVR_VI_RULE_HASH "dev"
#endif
inline constexpr const char* kRuleHash = RDRVR_VI_RULE_HASH;
inline constexpr const char* kExe = "rdrvr_vi_convert.exe";
inline constexpr const char* kDxc = "rdrvr_dxcompiler.dll";  // Microsoft's release dxcompiler.dll (DXC 1.9.2609.5), renamed: never the game's own

// One request: rdrvr_vi_convert.exe --one <vs|ps> <in.dxbc> <out.dxil> <result.txt> [--dxc <dll>] [--rule <kRuleHash>]
//              [--crash-test|--hang-test] [--instprev K] [--eye2-images LB2,SC2[,GUARD]] [--prevworld] [--text-input]
// --prevworld (run 26, GOAL-single-pass item 6a; vs only, test only; the mod passes it only with [Stereo]
// PrepassPrevWorldTwins=1): rage_prev_world rows 0-3 (gPrevWorldViewProjSingle) read at row + ViewID x 5, so view 1 reads
// rows 5-8, which [Stereo] PrepassPrevWorldMirror writes per draw (+0x50 of the CB it grows 0x50 -> 0x90). The result line
// gains " prevworld". A VS that does not read those rows is converted byte for byte as without it. Its hazards:
// - rows 5-8 without the grow are the CB ring's stale padding (no shader-side guard is possible, research/run19/prevworld.md
//   section 2), and with no eye-2 viewport the mirror writes eye 1's rows there: such a twin is drawn only in a frame whose
//   pass 1 wrote eye 2's rows with no fallback (pso.cpp's hold, prevworld::rows_written);
// - vi_auto holds "prevworld" as its own tag (never wvp: the WVP mirror would release it), in ready.txt, and keys the
//   cache folder "-pw".
// --eye2-images LB2,SC2[,GUARD] (run 24, P3's read side, route (c1); ps only, test only; the mod passes nothing today, so
// it is off): global_textures uints 11 (the Light Buffer's bindless index) and 13 (the Shadow Collector's) read as
//     (ViewID != 0 && uint GUARD == kEye2GuardMagic) ? uint LB2 : uint 11      (SC2 for 13)
// - LB2, SC2, GUARD: three different uints in 48..58 (vi::kEye2UintLo..Hi); 59..63 are refused (the sampler-state
//   globals t209..t213 can write them: run 23 images.md §9.5). Defaults below: 58, 57, 56 (GUARD 56 when omitted).
// - The guard is read once in the entry block; the texture handles that take the select are made non-uniform.
// - The twin's global_textures is declared 256 B (the CBV the game binds, and prepass::grow_cb's grown block).
// - A PS whose reads never reach uint 11 or 13 is converted byte for byte as without the flag. The refusals come only
//   from a read that can reach them (a row 2 / 3 load, a dynamic row, a global_textures read not understood).
// --text-input (tests only): <in> is a disassembly text, not a container.
//
// Before the mod passes --eye2-images (the hazards; none of this exists in src/core today):
// 1. vi_auto holds the "eye2img<k>" tag as its own kind (not as wvp, which the WVP mirror would release) and saves it
//    in ready.txt, so a restart keeps it held;
// 2. the cache folder is keyed by LB2/SC2/GUARD (as -ip<K>): another set of uints is another folder;
// 3. such a twin is drawn only after the grown block (256 B) was uploaded for that recording: without the grow, uints
//    48-63 are whatever the CB ring held. The guard covers only the grown case: once grown uploads have been live, ring
//    slots keep the magic at bytes 224..227, so after an un-grow a twin can read the magic with stale LB2/SC2;
// 4. the guard word holds kEye2GuardMagic only while the grow is live and both SRVs at LB2/SC2 are valid;
// 5. a stand-down writes 0 at GUARD first, and the eye-2 descriptors are freed only after the frames in flight;
// 6. a renderer reset (the CB re-made: cb_still_grown false) undoes the grow: the mod itself stops the twins (it must
//    not rely on the guard: stale ring slots may still hold the magic, see 3);
// 7. eye2_images claims GUARD (uint 56) as it claims LB2/SC2: a run-time global landing on it is a refusal or a
//    stand-down; and a static_assert (vi_auto includes this file) ties kEye2DefaultLb/Sc/Guard to eye2_images' pinned
//    uints and asserts the three distinct.
// The result file is written last, one line:
//   "ok <row reads>[ instprev][ wvp][ prevworld]"  vs: <out> is the view-instanced twin, validated and signed (wvp: it reads
//                           gWorldViewProj, whose eye-2 rows only [Stereo] PrepassWvpMirror writes; instprev (run 20, only
//                           with --instprev K): it reads eye 2's previous instance matrices at rows K + i, which only
//                           [Stereo] PrepassInstPrevMirror with the same K writes; prevworld (run 26, only with
//                           --prevworld): it reads eye 2's gPrevWorldViewProjSingle at rage_prev_world rows 5-8)
//   "shared"                vs: nothing per eye; <out> is the game's own container, used as the twin
//   "safe"                  ps: nothing per eye; the game's own pixel shader can draw both views
//   "rewritten <row reads>[ wvp][ eye2img<k>]"  (run 20) ps: <out> is the view-instanced twin PS (its per-eye reads
//                           redirected to eye 2's rows, as the VS's), validated and signed; wvp as for a VS; eye2img<k>
//                           (run 24, only with --eye2-images): k Light Buffer / Shadow Collector indices select eye 2's
//                           uint by ViewID and the guard (counted in <row reads> too): drawn only as the hazards above say
//   "refused eye-2 images: <why>"  (run 24, only with --eye2-images) ps: it reads uint 11 or 13 in a form the rule does
//                           not understand (a dynamic row, a non-i32 read of them, a row load used whole, a use other than
//                           a texture handle's index, a layout name other than LightBufferSampler / ShadowCollectorSampler,
//                           two global_textures, a type not all i32, no global_textures handle at the entry's start): no twin
//   "refused <reason>"      no twin, for good (the rule's refusal: the same answer every time)
//   "failed <step> <why>"   the build failed, for good
// Exit code 0 with a result line; anything else (no result: the DXC DLL not loaded, a crash, the crash test) is a
// failure of this session only, never cached.
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitNoDxc = 4;
inline constexpr int kExitRuleMismatch = 5;  // (--rule <hash>: the mod's rule hash is not this converter's: the mod stops it)
inline constexpr unsigned kExitCrashTest = 0xC0DE0001u;  // ([Debug] ViConvertCrashTest=1: the converter ends itself)
// (run 24) --eye2-images' defaults: the uints eye2_images.cpp pins (58 the Light Buffer's, 57 the Shadow Collector's, 56
// the guard), and the guard's value (vi::kEye2GuardMagic, the same number: 0x32455652, "RVE2" in memory order)
inline constexpr int kEye2DefaultLb = 58, kEye2DefaultSc = 57, kEye2DefaultGuard = 56;
inline constexpr unsigned kEye2GuardMagic = 0x32455652u;

}  // namespace rdrvr::vi_protocol
