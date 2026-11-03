#pragma once
// [Stereo] InstancedShadersAuto=1 (single-pass stereo step C, run 19 item 4; test only, off by default): the
// view-instanced twins made on the player's PC instead of from a folder of them (research\sps\fork-port-plan.md §5,
// research\run19\stepc.md).
//
// - pso.cpp's pipeline hook asks here for each pipeline's vertex and pixel shader. A shader not in the cache and not
//   refused is queued with its bytecode (a copy).
// - One worker thread (never the game's) writes each queued shader to the cache's in\ folder and runs
//   rdrvr_vi_convert.exe on it as a child process: below-normal priority, no window, in a job (killed with the game, no
//   error boxes), with a timeout. A crash or a timeout only marks that shader refused for this session (logged).
// - The cache: <InstancedShadersCache>\<RDR.exe timestamp+size>\<converter version>\ (default %LOCALAPPDATA%\RDRVR\
//   vi_cache): vs_<fnv>.dxil, ps_<fnv>.dxil (run 20: a rewritten PS), ready.txt ("vs <fnv> <the file's fnv> [wvp]
//   [instprev<K>]", "ps <fnv>": the converter's ps_safe, "ps <fnv> <the file's fnv> [wvp]": a rewritten PS) and
//   refused.txt. At start each listed twin is checked (DXBC header, its size, signed, its own fnv); a bad one is deleted.
// - Twins are made only from the cache, by pso.cpp (its builder thread; pso.cpp has the safe point). Nothing here
//   touches the device.
// - (run 20 item 3) With [Stereo] PrepassInstPrevMirror=1 at start and its PrepassInstPrevK=K in 1..768, every VS is
//   converted with `--instprev K`: the converter applies it only to a VS that reads rage_instancemtxprev (byte for byte
//   the same twin otherwise) and says so ("ok <n> instprev"); its ready.txt line carries "instprev<K>". The cache's
//   version folder then ends in "-ip<K>". Off (the default): no K passed, the folder as before, those VS refused.
// - (run 25, GOAL-single-pass item 2; research\run24\c1.md section 3) With [Stereo] PrepassEye2Shaders=1 and
//   PrepassEye2Images=1 at start, every PS is converted with `--eye2-images 58,57,56` (eye2_images' pinned uints): a PS that
//   reads the Light Buffer's or the Shadow Collector's bindless index answers "rewritten <n>[ wvp] eye2img<k>" (its twin
//   reads eye 2's indices by ViewID while uint 56 holds the guard). Its ready.txt line carries "eye2img<k>" and the folder ends
//   in "-e2i58.57.56". Such a twin is its own kind (never wvp): pso.cpp hands it out only while eye2_images::guard_valid() (and
//   that was so at the last frame's end). Off (the default): no flag passed, the folder and everything else as before.
// - (run 26, GOAL-single-pass item 6a) With [Stereo] PrepassPrevWorldTwins=1, PrepassPrevWorldMirror=1|2, PrepassViews=2 and
//   PrepassEye2Shaders=1 (so a PS reading eye 1's images is never "safe" beside such a VS) at start, every VS is converted
//   with `--prevworld`: a VS that reads rage_prev_world rows 0-3 answers "ok <n>[...] prevworld" (view 1 reads rows 5-8,
//   the mirror's). Its ready.txt line carries "prevworld" and the folder ends in "-pw". Its own kind (never wvp): pso.cpp
//   hands none out before item 6c, then only in a frame after one whose pass 1 the mirror wrote eye 2's rows for
//   (prevworld::rows_written, latched at the frame's end). Off: no flag, the folder and the rest as before.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rdrvr::vi_auto {

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
enum class State { Unknown, Queued, Ready, Refused };

// Reads the switch and, when on, loads the cache's index (on the calling thread, once: the device's creation) and
// starts the worker. False: off (everything as before).
bool init();
bool on();
// A vertex shader's state; Ready: *twin is its twin's container, *wvp whether it reads gWorldViewProj and *ip (run 20)
// whether its twin reads eye 2's previous instance matrices at rows K + i (instprev_k()). Unknown shaders are queued
// (code, n: the game's bytecode) and come back Queued; with no converter they come back Refused. queue false: only look
// (no queueing).
// (run 26) *pw: its twin reads eye 2's gPrevWorldViewProjSingle at rage_prev_world rows 5-8 (a "prevworld" twin)
State vs(uint64_t hash, const void* code, size_t n, Bytes* twin, bool* wvp, bool queue = true, bool* ip = nullptr, bool* pw = nullptr);
// (run 20) the K passed as --instprev to the converter (PrepassInstPrevMirror=1 at start with PrepassInstPrevK=K), or 0
int instprev_k();
// A pixel shader's state. Ready: the converter's "safe" (the game's own pixel shader draws both views; *twin empty) or
// (run 20) "rewritten" (*twin is its view-instanced twin PS, *wvp whether it reads gWorldViewProj; (run 25) *e2i whether it
// reads eye 2's image indices: an eye2img twin).
State ps(uint64_t hash, const void* code, size_t n, bool queue = true, Bytes* twin = nullptr, bool* wvp = nullptr, bool* e2i = nullptr);
// (run 25) the converter given --eye2-images (PrepassEye2Shaders=1 and PrepassEye2Images=1 at start): eye2img twins allowed
bool eye2_shaders();
// (run 26) the converter given --prevworld (PrepassPrevWorldTwins=1 with PrepassPrevWorldMirror=1|2, PrepassViews=2 and
// eye2_shaders() at start)
bool prevworld_shaders();
// (run 25) each frame's end with eye2_shaders() (pso.cpp): one warn line when eye2_images is not installed (after a grace of
// frames for the bootstrap's install) or has stood down / refused its grow: every eye2img twin then stays held
void eye2_check();
// Bumped each time a result lands (or the converter stops): pso.cpp's pending pipelines are looked at again.
uint64_t generation();
// The shaders decided this session (converted, refused, failed), in order: those from *cursor on are appended to *out
// and *cursor moves past them (one lock). A shader is decided once; the cache's at start are not in it.
struct Result {
    bool ps = false;
    uint64_t hash = 0;
    State st = State::Unknown;
    bool wvp = false;
    bool ip = false;  // (run 20) an instprev twin (vs only)
    bool e2i = false;  // (run 25) an eye2img twin (ps only)
    bool pw = false;   // (run 26) a prevworld twin (vs only)
};
void results_since(size_t* cursor, std::vector<Result>* out);
// "pso auto" / "pso auto reset" (the counters since the reset: the converter's processes, CPU and wall time, hits)
std::string status();
void reset_counters();
// (GOAL-single-pass-default.md item 2) the menu's line: on (the switch read and the cache found), the converter running
// (or why not), the queue and the shaders converted, refused and failed since the start (any thread)
struct Progress {
    bool on = false, converter = false;
    size_t queue = 0, ready = 0;  // (ready: the converted shaders known now, the cache's from earlier starts included)
    uint64_t converted = 0, refused = 0, failed = 0;  // (this session's)
    std::string why;
};
Progress progress();

}  // namespace rdrvr::vi_auto
