#include "core/vi_auto.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "core/config.h"
#include "core/eye2_images.h"
#include "core/log.h"
#include "vi_convert/protocol.h"

namespace rdrvr::vi_auto {
namespace {
namespace P = rdrvr::vi_protocol;

// (run 25, protocol.h hazard 7) the converter's --eye2-images defaults are the uints eye2_images pins, and its guard's value
static_assert(P::kEye2DefaultLb == eye2_images::kUintLb && P::kEye2DefaultSc == eye2_images::kUintSc && P::kEye2DefaultGuard == eye2_images::kUintGuard,
              "protocol.h's --eye2-images defaults are eye2_images' pinned uints (58 LB2, 57 SC2, 56 the guard)");
static_assert(P::kEye2DefaultLb != P::kEye2DefaultSc && P::kEye2DefaultLb != P::kEye2DefaultGuard && P::kEye2DefaultSc != P::kEye2DefaultGuard,
              "the three uints are distinct");
static_assert(P::kEye2GuardMagic == eye2_images::kGuardMagic, "the guard's magic is the converter's");

constexpr DWORD kTimeoutMs = 30000;    // a process takes about 20 ms offline (the DLL's load included)
constexpr size_t kMaxQueue = 8192;     // (more waits for the next time the game makes a pipeline with it)
constexpr size_t kMaxFile = 4u << 20;  // a twin larger than this is not a shader of this game

struct Entry {
    State st = State::Unknown;
    Bytes twin;
    bool wvp = false;
    bool ip = false;  // (run 20) its twin reads eye 2's previous instance matrices at rows K + i (g_ip_k)
    int e2i = 0;      // (run 25) a PS twin reading eye 2's image indices: the converter's eye2img<k>, k (> 0)
    bool pw = false;  // (run 26) a VS twin reading eye 2's gPrevWorldViewProjSingle at rage_prev_world rows 5-8: "prevworld"
};
struct Request {
    bool ps = false;
    uint64_t hash = 0;
    std::vector<uint8_t> code;
};

std::mutex g_m;
std::condition_variable g_cv;
std::unordered_map<uint64_t, Entry> g_vs, g_ps;  // under g_m
std::deque<Request> g_queue;                     // under g_m
std::string g_last_fail;                         // under g_m
std::vector<Result> g_results;                   // under g_m: every final state set in the session, in order (results_since)
std::string g_conv_why;                          // under g_m: why there is no converter
std::atomic<bool> g_on{false}, g_converter{false};
std::atomic<uint64_t> g_gen{0};
std::string g_dir, g_exe, g_dxc;  // set by init, read-only after
int g_crash_test = 0;
int g_ip_k = 0;  // (run 20) --instprev K for every VS: PrepassInstPrevMirror=1 at start with PrepassInstPrevK=K; 0: none (set by init)
// (run 25) --eye2-images LB2,SC2,GUARD for every PS: PrepassEye2Shaders=1 and PrepassEye2Images=1 at start (set by init)
bool g_e2i = false;
std::string g_e2i_arg;            // "58,57,56"
size_t s_e2i = 0, s_e2i_bad = 0;  // eye2img twins at start; eye2img lines dropped (the switch off this session)
std::atomic<uint64_t> c_e2i{0}, c_e2i_refused{0};  // eye2img twins converted; "refused eye-2 images"
// (run 26, GOAL-single-pass item 6a) --prevworld for every VS: PrepassPrevWorldTwins=1 with PrepassPrevWorldMirror=1|2,
// PrepassViews=2 and g_e2i at start (set by init)
bool g_pw = false;
size_t s_pw = 0, s_pw_bad = 0;  // prevworld twins at start; prevworld lines dropped (the switch off this session)
std::atomic<uint64_t> c_pw{0};  // prevworld twins converted
HANDLE g_job = nullptr;
size_t s_vs = 0, s_ps = 0, s_refused = 0, s_corrupt = 0;  // the cache at start (set by init)
size_t s_ip_vs = 0, s_ip_bad = 0;  // (run 20) instprev twins at start; instprev lines of another K (or none) dropped
std::atomic<uint64_t> c_queued{0}, c_ok{0}, c_shared{0}, c_safe{0}, c_refused{0}, c_failed{0}, c_crashes{0}, c_timeouts{0};
std::atomic<uint64_t> c_ps_rw{0};  // (run 20) pixel shaders rewritten
size_t s_ps_rw = 0;                // (run 20) rewritten pixel shaders in the cache at start
// since "pso auto reset"
std::atomic<uint64_t> c_hits_vs{0}, c_hits_ps{0}, c_procs{0}, c_cpu_us{0}, c_wall_us{0}, c_wall_max_us{0};

uint64_t fnv(const void* p, size_t n) {  // (pso.cpp's: FNV-1a 64 over the container)
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
bool read_file(const std::string& path, std::vector<uint8_t>* out, size_t max = kMaxFile) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0 || static_cast<size_t>(n) > max) {
        std::fclose(f);
        return false;
    }
    out->resize(static_cast<size_t>(n));
    const size_t got = n ? std::fread(out->data(), 1, out->size(), f) : 0;
    std::fclose(f);
    return got == out->size();
}
bool write_file(const std::string& path, const void* p, size_t n) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const size_t w = n ? std::fwrite(p, 1, n, f) : 0;
    const bool ok = std::fclose(f) == 0 && w == n;
    return ok;
}
void append_line(const char* name, const std::string& line) {  // (the worker only: one writer)
    const std::string s = line + "\r\n";
    FILE* f = nullptr;
    if (fopen_s(&f, (g_dir + "\\" + name).c_str(), "ab") != 0 || !f) return;
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}
void make_dirs(const std::string& p) {
    for (size_t i = 3; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '\\' || p[i] == '/') CreateDirectoryA(p.substr(0, i).c_str(), nullptr);
}
std::string hex16(uint64_t h) {
    char b[20];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
    return b;
}

// A cached container checked before it is trusted: the DXBC header, the size it states, a digest (signed: the
// validator's in-place signing; the game's own are signed too), every part inside the file.
bool valid_dxbc(const std::vector<uint8_t>& b) {
    if (b.size() < 32 || b.size() > kMaxFile || std::memcmp(b.data(), "DXBC", 4) != 0) return false;
    static const uint8_t zero[16] = {};
    if (std::memcmp(b.data() + 4, zero, 16) == 0) return false;
    uint16_t major = 0;
    uint32_t total = 0, count = 0;
    std::memcpy(&major, b.data() + 20, 2);
    std::memcpy(&total, b.data() + 24, 4);
    std::memcpy(&count, b.data() + 28, 4);
    if (major != 1 || total != b.size() || count == 0 || count > 64 || 32ull + 4ull * count > b.size()) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off = 0, len = 0;
        std::memcpy(&off, b.data() + 32 + 4 * i, 4);
        if (off < 32ull + 4ull * count || off + 8ull > b.size()) return false;
        std::memcpy(&len, b.data() + off + 4, 4);
        if (off + 8ull + len > b.size()) return false;
    }
    return true;
}

// The game's build, from RDR.exe's file (not its memory): the PE header's timestamp and the file's size.
std::string build_key() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return "";
    const unsigned long long size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    uint32_t stamp = 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") == 0 && f) {
        uint8_t h[4096] = {};
        const size_t got = std::fread(h, 1, sizeof(h), f);
        std::fclose(f);
        uint32_t pe = 0;
        if (got >= 64) std::memcpy(&pe, h + 0x3c, 4);
        if (got >= 64 && pe + 12 <= got && std::memcmp(h + pe, "PE\0\0", 4) == 0) std::memcpy(&stamp, h + pe + 8, 4);
    }
    char b[64];
    std::snprintf(b, sizeof(b), "rdr_%08x_%llu", stamp, size);
    return b;
}

void bump() { g_gen.fetch_add(1, std::memory_order_relaxed); }
// a shader's final state for this session (under g_m), and its line in the results pso.cpp's drain reads
void decide(bool ps, uint64_t hash, State st, bool wvp = false, Bytes twin = nullptr, bool ip = false, int e2i = 0, bool pw = false) {
    Entry& e = (ps ? g_ps : g_vs)[hash];
    e.st = st;
    e.wvp = wvp;
    e.ip = ip;
    e.e2i = e2i;
    e.pw = pw;
    if (twin) e.twin = std::move(twin);
    g_results.push_back(Result{ps, hash, st, wvp, ip, e2i > 0, pw});
}

// A failure of this session only (a crash, a timeout, no result): that shader refused until the game restarts.
void fail_session(const Request& r, const std::string& why) {
    {
        std::lock_guard lk(g_m);
        decide(r.ps, r.hash, State::Refused);
        g_last_fail = std::string(r.ps ? "ps " : "vs ") + hex16(r.hash) + ": " + why;
    }
    c_failed.fetch_add(1, std::memory_order_relaxed);
    log::limited("vi_auto.fail", 20, "[vi_auto] %s %016llx: %s (refused for this session; the game goes on)", r.ps ? "ps" : "vs",
                 static_cast<unsigned long long>(r.hash), why.c_str());
    bump();
}
// No converter from now on: what is queued is refused for this session (everything stays two-pass).
void stop_converter(const std::string& why) {
    std::deque<Request> left;
    {
        std::lock_guard lk(g_m);
        g_converter = false;
        g_conv_why = why;
        left.swap(g_queue);
        for (const auto& r : left) decide(r.ps, r.hash, State::Refused);
    }
    log::warn("[vi_auto] the converter stopped: %s (%zu queued shaders refused for this session)", why.c_str(), left.size());
    bump();
}

int g_launch_fails = 0, g_nodxc = 0, g_bad_run = 0;  // (the worker's: in a row; bad: crashes and timeouts)
constexpr int kBadRunMax = 5;
void convert(const Request& r) {
    const char* kind = r.ps ? "ps" : "vs";
    const std::string stem = std::string(kind) + "_" + hex16(r.hash);
    const std::string in = g_dir + "\\in\\" + stem + ".dxbc", out = g_dir + "\\in\\" + stem + ".out", res = g_dir + "\\in\\" + stem + ".res";
    DeleteFileA(out.c_str());
    DeleteFileA(res.c_str());
    if (!write_file(in, r.code.data(), r.code.size())) {
        fail_session(r, "its bytecode not written to " + in);
        return;
    }
    std::string cmd = "\"" + g_exe + "\" --one " + kind + " \"" + in + "\" \"" + out + "\" \"" + res + "\"";
    if (!g_dxc.empty()) cmd += " --dxc \"" + g_dxc + "\"";
    cmd += std::string(" --rule ") + P::kRuleHash;  // (a converter of another build exits kExitRuleMismatch)
    // (run 20) the mirror's K for every VS: the converter rewrites rage_instancemtxprev's loads only in a VS that has them
    // (any other VS's twin is byte for byte the one made without it) and answers "ok <n> instprev" for those
    if (g_ip_k && !r.ps) cmd += " --instprev " + std::to_string(g_ip_k);
    // (run 25) eye 2's image indices for every PS (the converter refuses the flag with a VS); a PS whose reads never reach the
    // game's uints 11 / 13 is converted byte for byte as without it
    if (g_e2i && r.ps) cmd += " --eye2-images " + g_e2i_arg;
    // (run 26) eye 2's rage_prev_world rows 5-8 for every VS (the converter refuses the flag with a PS); a VS that does not
    // read rows 0-3 is converted byte for byte as without it
    if (g_pw && !r.ps) cmd += " --prevworld";
    if (g_crash_test == 1) cmd += " --crash-test";
    else if (g_crash_test == 2) cmd += " --hang-test";
    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back(0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::string cwd = g_dir + "\\in";
    const double t0 = log::now_ms();
    // no handles inherited, no window, below normal priority; suspended until it is in the job
    if (!CreateProcessA(g_exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED,
                        nullptr, cwd.c_str(), &si, &pi)) {
        const DWORD e = GetLastError();
        DeleteFileA(in.c_str());
        fail_session(r, "the converter not started (error " + std::to_string(e) + ")");
        if (++g_launch_fails >= 3) stop_converter("it could not be started 3 times in a row (error " + std::to_string(e) + ")");
        return;
    }
    g_launch_fails = 0;
    if (g_job && !AssignProcessToJobObject(g_job, pi.hProcess))
        log::limited("vi_auto.job", 1, "[vi_auto] the converter not put in its job (error %lu): it may outlive the game", GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    const bool timeout = WaitForSingleObject(pi.hProcess, kTimeoutMs) != WAIT_OBJECT_0;
    if (timeout) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    FILETIME ft_c{}, ft_e{}, ft_k{}, ft_u{};
    if (GetProcessTimes(pi.hProcess, &ft_c, &ft_e, &ft_k, &ft_u)) {
        const auto t = [](const FILETIME& x) { return (static_cast<uint64_t>(x.dwHighDateTime) << 32) | x.dwLowDateTime; };
        c_cpu_us.fetch_add((t(ft_k) + t(ft_u)) / 10, std::memory_order_relaxed);
    }
    CloseHandle(pi.hProcess);
    const uint64_t wall = static_cast<uint64_t>((log::now_ms() - t0) * 1000.0);
    c_procs.fetch_add(1, std::memory_order_relaxed);
    c_wall_us.fetch_add(wall, std::memory_order_relaxed);
    if (wall > c_wall_max_us.load(std::memory_order_relaxed)) c_wall_max_us.store(wall, std::memory_order_relaxed);
    std::vector<uint8_t> rb;
    const bool have = !timeout && code == static_cast<DWORD>(P::kExitOk) && read_file(res, &rb, 4096) && !rb.empty();
    std::string verdict(rb.begin(), rb.end());
    while (!verdict.empty() && (verdict.back() == '\n' || verdict.back() == '\r' || verdict.back() == ' ')) verdict.pop_back();
    for (char& ch : verdict)
        if (ch == '\r' || ch == '\n') ch = ' ';
    DeleteFileA(in.c_str());
    DeleteFileA(res.c_str());
    if (timeout) {
        DeleteFileA(out.c_str());
        c_timeouts.fetch_add(1, std::memory_order_relaxed);
        fail_session(r, "the converter timed out (" + std::to_string(kTimeoutMs / 1000) + " s) and was ended");
        if (++g_bad_run >= kBadRunMax) stop_converter("it crashed or timed out " + std::to_string(kBadRunMax) + " times in a row");
        return;
    }
    if (!have) {
        DeleteFileA(out.c_str());
        char b[96];
        std::snprintf(b, sizeof(b), "the converter ended with %#lx and no result", static_cast<unsigned long>(code));
        if (code != static_cast<DWORD>(P::kExitNoDxc) && code != static_cast<DWORD>(P::kExitRuleMismatch))
            c_crashes.fetch_add(1, std::memory_order_relaxed);
        fail_session(r, b);
        if (code == static_cast<DWORD>(P::kExitRuleMismatch)) {
            stop_converter(std::string("its rule is not the mod's (") + P::kRuleHash + "): a converter of another build");
            return;
        }
        if (code == static_cast<DWORD>(P::kExitNoDxc) && ++g_nodxc >= 3) stop_converter("its DXC DLL (" + std::string(P::kDxc) + ") not loaded 3 times in a row");
        else if (code != static_cast<DWORD>(P::kExitNoDxc) && ++g_bad_run >= kBadRunMax)
            stop_converter("it crashed or timed out " + std::to_string(kBadRunMax) + " times in a row");
        return;
    }
    g_nodxc = 0;
    g_bad_run = 0;
    const bool ok = verdict.compare(0, 3, "ok ") == 0, shared = verdict == "shared", safe = verdict == "safe";
    const bool rewritten = verdict.compare(0, 10, "rewritten ") == 0;  // (run 20: a PS's twin)
    // "ok <n>[ instprev][ wvp][ prevworld]" / "rewritten <n>[ wvp][ eye2img<k>]": the tags after the count
    bool wvp = false, ip = false, e2i_bad = false, pw = false;
    std::string unknown;  // (run 26 review) a tag not understood: that shader refused for the session, never held as wvp
    int e2i = 0;
    if (ok || rewritten) {
        size_t at = verdict.find(' ', rewritten ? 10 : 3);
        while (at != std::string::npos) {
            const size_t e = verdict.find(' ', at + 1);
            const std::string tag = verdict.substr(at + 1, e == std::string::npos ? std::string::npos : e - at - 1);
            if (tag.compare(0, 7, "eye2img") == 0) {  // (run 25) its own kind, never wvp (the WVP mirror would release it)
                const std::string k = tag.substr(7);
                const bool digits = !k.empty() && k.size() <= 4 && k.find_first_not_of("0123456789") == std::string::npos;
                if (digits && std::atoi(k.c_str()) > 0) e2i = std::atoi(k.c_str());
                else e2i_bad = true;
            } else if (tag == "prevworld") {  // (run 26) its own kind, never wvp (the WVP mirror would release it)
                pw = true;
            } else if (tag == "wvp") {
                wvp = true;
            } else if (tag == "instprev") {
                ip = true;
            } else if (!tag.empty() && unknown.empty()) {
                unknown = tag;
            }
            at = e;
        }
    }
    if (!unknown.empty()) {  // (never: the converter's tags are wvp, instprev, prevworld and eye2img<k>; a misspelt one could
                             // otherwise be released by another kind's mirror)
        DeleteFileA(out.c_str());
        fail_session(r, "a tag not understood (" + unknown.substr(0, 24) + "): " + verdict.substr(0, 80));
        return;
    }
    if (ip && (r.ps || !g_ip_k)) {  // (never: the converter says instprev only when given a K, for a VS)
        DeleteFileA(out.c_str());
        fail_session(r, "an instprev result without --instprev: " + verdict.substr(0, 80));
        return;
    }
    if ((e2i || e2i_bad) && (!r.ps || !rewritten || !g_e2i || e2i_bad)) {  // (never: eye2img only for a rewritten PS, given the flag)
        DeleteFileA(out.c_str());
        fail_session(r, "an eye2img result not of a PS converted with --eye2-images: " + verdict.substr(0, 80));
        return;
    }
    if (pw && (r.ps || !ok || !g_pw)) {  // (never: prevworld only for a VS twin, given the flag)
        DeleteFileA(out.c_str());
        fail_session(r, "a prevworld result not of a VS converted with --prevworld: " + verdict.substr(0, 80));
        return;
    }
    if (!r.ps && (ok || shared)) {
        auto twin = std::make_shared<std::vector<uint8_t>>();
        const std::string file = g_dir + "\\vs_" + hex16(r.hash) + ".dxil";
        if (!read_file(out, twin.get()) || !valid_dxbc(*twin)) {
            DeleteFileA(out.c_str());
            fail_session(r, "the converter's twin is not a valid signed container (" + verdict + ")");
            return;
        }
        if (!MoveFileExA(out.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(out.c_str());
            fail_session(r, "its twin not moved into the cache (error " + std::to_string(GetLastError()) + ")");
            return;
        }
        append_line("ready.txt", "vs " + hex16(r.hash) + " " + hex16(fnv(twin->data(), twin->size())) + (wvp ? " wvp" : "") +
                                     (ip ? " instprev" + std::to_string(g_ip_k) : "") + (pw ? " prevworld" : ""));  // (run 26: kept over a restart)
        {
            std::lock_guard lk(g_m);
            decide(false, r.hash, State::Ready, wvp, std::move(twin), ip, 0, pw);
        }
        (ok ? c_ok : c_shared).fetch_add(1, std::memory_order_relaxed);
        if (pw) c_pw.fetch_add(1, std::memory_order_relaxed);
    } else if (r.ps && rewritten) {  // (run 20) the twin PS, checked and kept as a VS twin is
        auto twin = std::make_shared<std::vector<uint8_t>>();
        const std::string file = g_dir + "\\ps_" + hex16(r.hash) + ".dxil";
        if (!read_file(out, twin.get()) || !valid_dxbc(*twin)) {
            DeleteFileA(out.c_str());
            fail_session(r, "the converter's twin PS is not a valid signed container (" + verdict + ")");
            return;
        }
        if (!MoveFileExA(out.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(out.c_str());
            fail_session(r, "its twin PS not moved into the cache (error " + std::to_string(GetLastError()) + ")");
            return;
        }
        append_line("ready.txt", "ps " + hex16(r.hash) + " " + hex16(fnv(twin->data(), twin->size())) + (wvp ? " wvp" : "") +
                                     (e2i ? " eye2img" + std::to_string(e2i) : ""));  // (run 25: kept over a restart)
        {
            std::lock_guard lk(g_m);
            decide(true, r.hash, State::Ready, wvp, std::move(twin), false, e2i);
        }
        c_ps_rw.fetch_add(1, std::memory_order_relaxed);
        if (e2i) c_e2i.fetch_add(1, std::memory_order_relaxed);
    } else if (r.ps && safe) {
        append_line("ready.txt", "ps " + hex16(r.hash));
        {
            std::lock_guard lk(g_m);
            decide(true, r.hash, State::Ready);
        }
        c_safe.fetch_add(1, std::memory_order_relaxed);
    } else if (verdict.compare(0, 8, "refused ") == 0 || verdict.compare(0, 7, "failed ") == 0) {
        DeleteFileA(out.c_str());
        if (verdict.compare(0, 21, "refused eye-2 images:") == 0) c_e2i_refused.fetch_add(1, std::memory_order_relaxed);  // (run 25)
        append_line("refused.txt", std::string(kind) + " " + hex16(r.hash) + " " + verdict);
        {
            std::lock_guard lk(g_m);
            decide(r.ps, r.hash, State::Refused);
        }
        c_refused.fetch_add(1, std::memory_order_relaxed);
    } else {
        DeleteFileA(out.c_str());
        fail_session(r, "a result not understood: " + verdict.substr(0, 80));
        return;
    }
    bump();
}

DWORD WINAPI worker(void*) {
    for (;;) {
        Request r;
        {
            std::unique_lock lk(g_m);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            r = std::move(g_queue.front());
            g_queue.pop_front();
        }
        convert(r);
    }
}

// The cache's index at start: refused.txt, then ready.txt (each twin checked; a bad one deleted, the index rewritten)
void load_cache() {
    WIN32_FIND_DATAA fd{};  // (a killed session's leftovers)
    HANDLE h = FindFirstFileA((g_dir + "\\in\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((g_dir + "\\in\\" + fd.cFileName).c_str());
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    char line[512], kind[8], a[32];
    FILE* f = nullptr;
    if (fopen_s(&f, (g_dir + "\\refused.txt").c_str(), "rb") == 0 && f) {
        while (std::fgets(line, sizeof(line), f)) {
            if (sscanf_s(line, "%7s %31s", kind, static_cast<unsigned>(sizeof(kind)), a, static_cast<unsigned>(sizeof(a))) != 2 ||
                std::strlen(a) != 16)
                continue;
            const uint64_t hash = std::strtoull(a, nullptr, 16);
            if (!hash || (std::strcmp(kind, "vs") != 0 && std::strcmp(kind, "ps") != 0)) continue;
            (kind[0] == 'p' ? g_ps : g_vs)[hash].st = State::Refused;
            ++s_refused;
        }
        std::fclose(f);
    }
    std::string keep;
    bool dropped = false;
    if (fopen_s(&f, (g_dir + "\\ready.txt").c_str(), "rb") == 0 && f) {
        while (std::fgets(line, sizeof(line), f)) {
            if (!std::strchr(line, '\n')) {  // (a line cut short: the game ended while it was written; that shader converts again)
                dropped = true;
                continue;
            }
            // "vs <fnv> <the twin's fnv> [wvp] [instprev<K>]" or "ps <fnv>": the fields split on blanks
            std::vector<std::string> t;
            for (const char* p = line; *p;) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
                const char* q = p;
                while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') ++q;
                if (q > p) t.emplace_back(p, q);
                p = q;
            }
            const uint64_t hash = t.size() >= 2 && t[1].size() == 16 ? std::strtoull(t[1].c_str(), nullptr, 16) : 0;
            if (!hash) {
                dropped = true;
                continue;
            }
            if (t[0] == "ps" && t.size() == 2) {  // (the converter's "safe": the game's own PS)
                if (g_ps[hash].st != State::Ready) ++s_ps;
                g_ps[hash].st = State::Ready;
                keep += "ps " + hex16(hash) + "\r\n";
                continue;
            }
            if (t[0] == "ps") {  // (run 20) "ps <fnv> <the twin's fnv> [wvp]": a rewritten PS, its file checked as a VS twin's
                const std::string file = g_dir + "\\ps_" + hex16(hash) + ".dxil";
                bool pwvp = false, punknown = false, pe2i_bad = false;
                int pe2i = 0;
                for (size_t i = 3; i < t.size(); ++i) {
                    if (t[i] == "wvp") {
                        pwvp = true;
                    } else if (t[i].compare(0, 7, "eye2img") == 0) {
                        // (run 25) its own kind, kept only in a session that passes --eye2-images (the folder is keyed by the
                        // uints, so another set is not this folder); otherwise dropped and that PS converted again
                        const std::string k = t[i].substr(7);
                        const bool digits = !k.empty() && k.size() <= 4 && k.find_first_not_of("0123456789") == std::string::npos;
                        if (g_e2i && digits && std::atoi(k.c_str()) > 0) pe2i = std::atoi(k.c_str());
                        else pe2i_bad = true;
                    } else {
                        punknown = true;  // (instprev or anything else: (run 26 review) deleted below, never held as wvp)
                    }
                }
                if (punknown && !pe2i_bad) {  // (never written by this mod: a garbled line) deleted, that PS converted again
                    DeleteFileA(file.c_str());
                    g_ps.erase(hash);
                    ++s_corrupt;
                    dropped = true;
                    log::limited("vi_auto.tagbad", 10, "[vi_auto] ps %016llx: a tag not understood in ready.txt: deleted, converted again",
                                 static_cast<unsigned long long>(hash));
                    continue;
                }
                if (pe2i_bad) {
                    DeleteFileA(file.c_str());
                    g_ps.erase(hash);
                    ++s_e2i_bad;
                    dropped = true;
                    log::limited("vi_auto.e2ibad", 10, "[vi_auto] ps %016llx: an eye2img tag in ready.txt not of this session's --eye2-images: deleted, converted again",
                                 static_cast<unsigned long long>(hash));
                    continue;
                }
                auto twin = std::make_shared<std::vector<uint8_t>>();
                const bool good = t[2].size() == 16 && read_file(file, twin.get()) && valid_dxbc(*twin) &&
                                  fnv(twin->data(), twin->size()) == std::strtoull(t[2].c_str(), nullptr, 16);
                if (!good) {
                    DeleteFileA(file.c_str());
                    g_ps.erase(hash);
                    ++s_corrupt;
                    dropped = true;
                    log::limited("vi_auto.corrupt", 10, "[vi_auto] the cached twin of ps %016llx failed its check: deleted", static_cast<unsigned long long>(hash));
                    continue;
                }
                Entry& e = g_ps[hash];
                if (e.st != State::Ready) {
                    ++s_ps;
                    ++s_ps_rw;
                    if (pe2i) ++s_e2i;
                }
                e.st = State::Ready;
                e.twin = std::move(twin);
                e.wvp = pwvp;
                e.e2i = pe2i;
                keep += "ps " + hex16(hash) + " " + t[2] + (e.wvp ? " wvp" : "") + (pe2i ? " eye2img" + std::to_string(pe2i) : "") + "\r\n";
                continue;
            }
            if (t[0] != "vs" || t.size() < 3 || t[2].size() != 16) {
                dropped = true;
                continue;
            }
            // the tags: "wvp"; "instprev<K>" (run 20) only with this session's K (the folder is keyed by it: another K, or
            // none, is not this folder's line: dropped, that shader converted again); "prevworld" (run 26) likewise; anything
            // else not understood (run 26 review): the line and its file deleted, that shader converted again (never held as
            // wvp, which PrepassWvpMirror would release)
            bool wvp = false, ip = false, bad_ip = false, unknown = false, pw = false, bad_pw = false;
            for (size_t i = 3; i < t.size(); ++i) {
                if (t[i] == "wvp") {
                    wvp = true;
                } else if (t[i] == "prevworld") {  // (run 26) its own kind, kept only in a session that passes --prevworld
                    if (g_pw) pw = true;
                    else bad_pw = true;
                } else if (t[i].compare(0, 8, "instprev") == 0) {
                    const std::string k = t[i].substr(8);
                    const bool digits = !k.empty() && k.size() <= 3 && k.find_first_not_of("0123456789") == std::string::npos;
                    if (g_ip_k && digits && std::atoi(k.c_str()) == g_ip_k) ip = true;
                    else bad_ip = true;
                } else {
                    unknown = true;
                }
            }
            const std::string file = g_dir + "\\vs_" + hex16(hash) + ".dxil";
            if (bad_ip) {
                DeleteFileA(file.c_str());
                g_vs.erase(hash);
                ++s_ip_bad;
                dropped = true;
                log::limited("vi_auto.ipbad", 10, "[vi_auto] vs %016llx: an instprev tag not of this session's K (%d) in ready.txt: deleted, converted again",
                             static_cast<unsigned long long>(hash), g_ip_k);
                continue;
            }
            if (unknown && !bad_pw) {  // (run 26 review; never written by this mod: a garbled line) deleted, never held as wvp
                DeleteFileA(file.c_str());
                g_vs.erase(hash);
                ++s_corrupt;
                dropped = true;
                log::limited("vi_auto.tagbad", 10, "[vi_auto] vs %016llx: a tag not understood in ready.txt: deleted, converted again",
                             static_cast<unsigned long long>(hash));
                continue;
            }
            if (bad_pw) {  // (run 26; never: the folder is keyed "-pw") another session's line: deleted, that VS converted again
                DeleteFileA(file.c_str());
                g_vs.erase(hash);
                ++s_pw_bad;
                dropped = true;
                log::limited("vi_auto.pwbad", 10, "[vi_auto] vs %016llx: a prevworld tag in ready.txt not of a --prevworld session: deleted, converted again",
                             static_cast<unsigned long long>(hash));
                continue;
            }
            auto twin = std::make_shared<std::vector<uint8_t>>();
            const bool good = read_file(file, twin.get()) && valid_dxbc(*twin) && fnv(twin->data(), twin->size()) == std::strtoull(t[2].c_str(), nullptr, 16);
            if (!good) {  // a corrupt entry: deleted, so the shader is converted again
                DeleteFileA(file.c_str());
                g_vs.erase(hash);
                ++s_corrupt;
                dropped = true;
                log::limited("vi_auto.corrupt", 10, "[vi_auto] the cached twin of vs %016llx failed its check: deleted", static_cast<unsigned long long>(hash));
                continue;
            }
            Entry& e = g_vs[hash];
            if (e.st != State::Ready) {
                ++s_vs;
                if (ip) ++s_ip_vs;
                if (pw) ++s_pw;
            }
            e.st = State::Ready;
            e.twin = std::move(twin);
            e.wvp = wvp;
            e.ip = ip;
            e.pw = pw;
            keep += "vs " + hex16(hash) + " " + t[2] + (e.wvp ? " wvp" : "") + (ip ? " instprev" + std::to_string(g_ip_k) : "") +
                    (pw ? " prevworld" : "") + "\r\n";
        }
        std::fclose(f);
    }
    if (dropped) {  // the index without the bad lines (a new file, then moved over the old one)
        const std::string tmp = g_dir + "\\ready.tmp";
        if (write_file(tmp, keep.data(), keep.size()))
            MoveFileExA(tmp.c_str(), (g_dir + "\\ready.txt").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
}

// a.b.c.d of a DLL's version resource, or "none"
std::string file_version(const std::string& path) {
    DWORD h = 0;
    const DWORD n = GetFileVersionInfoSizeA(path.c_str(), &h);
    if (!n) return "none";
    std::vector<uint8_t> b(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoA(path.c_str(), 0, n, b.data()) || !VerQueryValueA(b.data(), "\\", reinterpret_cast<void**>(&fi), &len) || !fi ||
        len < sizeof(VS_FIXEDFILEINFO))
        return "none";
    char v[48];
    std::snprintf(v, sizeof(v), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                  LOWORD(fi->dwFileVersionLS));
    return v;
}
std::string narrow(const wchar_t* w) {
    char b[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_ACP, 0, w, -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

}  // namespace

bool init() {
    static bool done = false;
    if (done) return g_on.load();
    done = true;
    if (!config::get_bool("Stereo", "InstancedShadersAuto", false)) return false;
    std::string root = config::get_string("Stereo", "InstancedShadersCache", "");
    if (root.empty()) {
        char b[MAX_PATH] = {};
        const DWORD n = ExpandEnvironmentStringsA("%LOCALAPPDATA%\\RDRVR\\vi_cache", b, MAX_PATH);
        if (n == 0 || n > MAX_PATH || std::strchr(b, '%')) {
            log::warn("[vi_auto] InstancedShadersAuto: no %%LOCALAPPDATA%%: off");
            return false;
        }
        root = b;
    }
    while (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    const std::string key = build_key();
    if (key.empty()) {
        log::warn("[vi_auto] InstancedShadersAuto: the game's build not read: off");
        return false;
    }
    g_exe = config::get_string("Stereo", "InstancedShadersConverter", "");
    // (GOAL-single-pass-default.md item 2) the install's: the game folder's RDRVR_Converter\ (never its root: RedHook's
    // winmm.dll proxy is there), the DXC DLL beside it
    if (g_exe.empty()) g_exe = narrow(log::game_dir()) + "RDRVR_Converter\\" + P::kExe;
    g_dxc = config::get_string("Stereo", "InstancedShadersDxc", "");
    g_crash_test = config::get_int("Debug", "ViConvertCrashTest", 0);
    // (run 20 item 3) the instprev mirror's K, as pso.cpp lists twins.txt's instprev<K> lines: =1 (not the read-only modes)
    // at start, K in 1..768 (inst_prev_mirror.cpp's range); the VS that read rage_instancemtxprev are refused otherwise
    if (config::get_int("Stereo", "PrepassInstPrevMirror", 0) == 1) {
        const int k = config::get_int("Stereo", "PrepassInstPrevK", 512);
        if (k >= 1 && k <= 768) g_ip_k = k;
        else log::warn("[vi_auto] PrepassInstPrevK=%d not in 1..768: the rage_instancemtxprev VS stay refused", k);
    }
    // (run 25) eye 2's image indices: PrepassEye2Shaders=1 (int, at start) with PrepassEye2Images=1, whose install checks the
    // rest; the twins are drawn only while eye2_images::guard_valid() (pso.cpp), so a refused install holds every one
    const int e2s = config::get_int("Stereo", "PrepassEye2Shaders", 0);
    if (e2s == 1 && config::get_int("Stereo", "PrepassEye2Images", 0) == 1) {
        g_e2i = true;
        g_e2i_arg = std::to_string(eye2_images::kUintLb) + "," + std::to_string(eye2_images::kUintSc) + "," + std::to_string(eye2_images::kUintGuard);
    } else if (e2s != 0) {
        log::warn("[vi_auto] PrepassEye2Shaders=%d: needs =1 with [Stereo] PrepassEye2Images=1: off (no --eye2-images)", e2s);
    }
    // (run 26, GOAL-single-pass item 6a) eye 2's rage_prev_world rows 5-8: PrepassPrevWorldTwins=1 (int, at start) with the
    // mirror writing (PrepassPrevWorldMirror=1 or 2, and its PrepassViews=2); pso.cpp holds the twins unless the mirror wrote
    // eye 2's rows that frame (prevworld::rows_written), so an install refused at run time holds every one. It also needs
    // --eye2-images (research/run25/families.md section 1.3): without it a PS reading the Light Buffer's or the collector's
    // index is "safe" and would pair with a prevworld VS, drawing eye 1's images in view 1; with it every such PS is an
    // eye2img twin or refused, and a "safe" PS reads neither
    const int pwt = config::get_int("Stereo", "PrepassPrevWorldTwins", 0);
    const int pwm = config::get_int("Stereo", "PrepassPrevWorldMirror", 0);
    if (pwt == 1 && (pwm == 1 || pwm == 2) && config::get_int("Stereo", "PrepassViews", 0) == 2 && g_e2i) {
        g_pw = true;
    } else if (pwt != 0) {
        log::warn("[vi_auto] PrepassPrevWorldTwins=%d: needs =1 with [Stereo] PrepassPrevWorldMirror=1|2, PrepassViews=2 and PrepassEye2Shaders=1 "
                  "(PrepassEye2Images=1): off (no --prevworld)", pwt);
    }
    // the version folder: the protocol's, the rule's sources' hash (CMake, at configure) and the DXC DLL's file version,
    // so a changed rule or DLL never trusts an older one's twins
    std::string dll = g_dxc;
    if (dll.empty()) {
        const size_t k = g_exe.find_last_of("\\/");
        dll = (k == std::string::npos ? std::string() : g_exe.substr(0, k + 1)) + P::kDxc;
    }
    g_dir = root + "\\" + key + "\\" + P::kVersion + "-" + P::kRuleHash + "-dxc" + file_version(dll);
    if (g_ip_k) g_dir += "-ip" + std::to_string(g_ip_k);  // (run 20: the twins made with --instprev K in a folder of their own)
    if (g_e2i) {  // (run 25, protocol.h hazard 2) "-e2i58.57.56": another set of uints is another folder
        std::string k = g_e2i_arg;
        for (char& ch : k)
            if (ch == ',') ch = '.';
        g_dir += "-e2i" + k;
    }
    if (g_pw) g_dir += "-pw";  // (run 26) the twins made with --prevworld in a folder of their own
    make_dirs(g_dir + "\\in");
    if (GetFileAttributesA((g_dir + "\\in").c_str()) == INVALID_FILE_ATTRIBUTES) {
        log::warn("[vi_auto] InstancedShadersAuto: the cache folder %s not made: off", g_dir.c_str());
        return false;
    }
    const double t0 = log::now_ms();
    load_cache();
    const double ms = log::now_ms() - t0;
    if (GetFileAttributesA(g_exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_conv_why = "not found at " + g_exe;
        log::warn("[vi_auto] the converter %s: only the cache's twins are used", g_conv_why.c_str());
    } else {
        // the job: the converter dies with the game, and an unhandled exception in it ends it without an error box
        g_job = CreateJobObjectW(nullptr, nullptr);
        if (g_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            li.ProcessMemoryLimit = static_cast<SIZE_T>(1) << 30;
            if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
                CloseHandle(g_job);
                g_job = nullptr;
            }
        }
        HANDLE th = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (th) {
            SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(th);
            g_converter = true;
        } else {
            g_conv_why = "its thread not started";
        }
    }
    g_on = true;
    log::info("[vi_auto] InstancedShadersAuto (test only): cache %s: %zu VS twins and %zu PS ready, %zu refused, %zu corrupt deleted (%.1f ms); "
              "converter %s%s%s; instprev %s (K %d: %zu VS twins at start, %zu lines of another K dropped)",
              g_dir.c_str(), s_vs, s_ps, s_refused, s_corrupt, ms, g_exe.c_str(), g_converter ? "" : " OFF: ", g_converter ? "" : g_conv_why.c_str(),
              g_ip_k ? "on" : "off (PrepassInstPrevMirror not 1 at start: those VS refused)", g_ip_k, s_ip_vs, s_ip_bad);
    if (g_crash_test) log::warn("[vi_auto] [Debug] ViConvertCrashTest=%d: every conversion %s (a test)", g_crash_test, g_crash_test == 2 ? "hangs" : "crashes");
    if (g_e2i)
        log::info("[vi_auto] PrepassEye2Shaders (test only): every PS converted with --eye2-images %s; %zu eye2img twins at start, %zu lines dropped; "
                  "their twins drawn only while eye2_images' guard is valid",
                  g_e2i_arg.c_str(), s_e2i, s_e2i_bad);
    if (g_pw)
        log::info("[vi_auto] PrepassPrevWorldTwins (test only): every VS converted with --prevworld; %zu prevworld twins at start, %zu lines "
                  "dropped; their twins drawn only in a frame whose pass 1 wrote eye 2's rows 5-8 (the PrevWorld mirror)",
                  s_pw, s_pw_bad);
    return true;
}

bool on() { return g_on.load(std::memory_order_relaxed); }

State vs(uint64_t hash, const void* code, size_t n, Bytes* twin, bool* wvp, bool queue, bool* ip, bool* pw) {
    std::lock_guard lk(g_m);
    auto it = g_vs.find(hash);
    if (it != g_vs.end() && it->second.st != State::Unknown) {
        if (it->second.st == State::Ready) {
            if (twin) *twin = it->second.twin;
            if (wvp) *wvp = it->second.wvp;
            if (ip) *ip = it->second.ip;
            if (pw) *pw = it->second.pw;
            if (queue) c_hits_vs.fetch_add(1, std::memory_order_relaxed);
        }
        return it->second.st;
    }
    if (!queue) return State::Unknown;
    if (!g_converter.load(std::memory_order_relaxed) || !code || n < 32) {
        g_vs[hash].st = State::Refused;
        return State::Refused;
    }
    if (g_queue.size() >= kMaxQueue) return State::Refused;  // (not remembered: asked again at its next pipeline)
    g_vs[hash].st = State::Queued;
    const uint8_t* b = static_cast<const uint8_t*>(code);
    g_queue.push_back(Request{false, hash, std::vector<uint8_t>(b, b + n)});
    c_queued.fetch_add(1, std::memory_order_relaxed);
    g_cv.notify_one();
    return State::Queued;
}

State ps(uint64_t hash, const void* code, size_t n, bool queue, Bytes* twin, bool* wvp, bool* e2i) {
    std::lock_guard lk(g_m);
    auto it = g_ps.find(hash);
    if (it != g_ps.end() && it->second.st != State::Unknown) {
        if (it->second.st == State::Ready) {
            if (twin) *twin = it->second.twin;  // (empty for a "safe" PS)
            if (wvp) *wvp = it->second.wvp;
            if (e2i) *e2i = it->second.e2i > 0;
            if (queue) c_hits_ps.fetch_add(1, std::memory_order_relaxed);
        }
        return it->second.st;
    }
    if (!queue) return State::Unknown;
    if (!g_converter.load(std::memory_order_relaxed) || !code || n < 32) {
        g_ps[hash].st = State::Refused;
        return State::Refused;
    }
    if (g_queue.size() >= kMaxQueue) return State::Refused;
    g_ps[hash].st = State::Queued;
    const uint8_t* b = static_cast<const uint8_t*>(code);
    g_queue.push_back(Request{true, hash, std::vector<uint8_t>(b, b + n)});
    c_queued.fetch_add(1, std::memory_order_relaxed);
    g_cv.notify_one();
    return State::Queued;
}

uint64_t generation() { return g_gen.load(std::memory_order_relaxed); }
int instprev_k() { return g_ip_k; }
bool eye2_shaders() { return g_e2i; }
bool prevworld_shaders() { return g_pw; }
namespace {
std::atomic<uint64_t> g_e2i_checks{0};
std::atomic<bool> g_e2i_warned{false};
const char* e2i_health_text(int h) {
    return h > 0 ? "up" : h < 0 ? "STOOD DOWN or its grow refused: every eye2img twin stays held"
                                : "NOT INSTALLED (PrepassEye2Images refused at install, or not yet): every eye2img twin stays held";
}
}  // namespace
void eye2_check() {
    if (!g_e2i || g_e2i_warned.load(std::memory_order_relaxed)) return;
    const uint64_t n = g_e2i_checks.fetch_add(1, std::memory_order_relaxed) + 1;
    const int h = eye2_images::health();
    if (h > 0 || (h == 0 && n < 600)) return;  // (not installed: after about 10 s of frames, the bootstrap's install long done)
    if (!g_e2i_warned.exchange(true))
        log::warn("[vi_auto] PrepassEye2Shaders: eye2_images is %s (%llu frames)", e2i_health_text(h), static_cast<unsigned long long>(n));
}

void results_since(size_t* cursor, std::vector<Result>* out) {
    std::lock_guard lk(g_m);
    for (size_t i = *cursor; i < g_results.size(); ++i) out->push_back(g_results[i]);
    *cursor = g_results.size();
}

void reset_counters() {
    for (auto* c : {&c_hits_vs, &c_hits_ps, &c_procs, &c_cpu_us, &c_wall_us, &c_wall_max_us}) c->store(0);
}

Progress progress() {
    Progress p;
    p.on = on();
    if (!p.on) return p;
    p.converter = g_converter.load();
    {
        std::lock_guard lk(g_m);
        p.queue = g_queue.size();
        p.why = g_conv_why;
        for (const auto& kv : g_vs) p.ready += kv.second.st == State::Ready;
        for (const auto& kv : g_ps) p.ready += kv.second.st == State::Ready;
    }
    p.converted = c_ok.load() + c_shared.load() + c_safe.load() + c_ps_rw.load();
    p.refused = c_refused.load();
    p.failed = c_failed.load();
    return p;
}

std::string status() {
    if (!on()) return "auto off";
    size_t depth = 0, vs_ready = 0, ps_ready = 0, ip_ready = 0, ps_rw_ready = 0, e2i_ready = 0, pw_ready = 0;
    std::string last, why;
    {
        std::lock_guard lk(g_m);
        depth = g_queue.size();
        for (const auto& kv : g_vs) {
            vs_ready += kv.second.st == State::Ready;
            ip_ready += kv.second.st == State::Ready && kv.second.ip;
            pw_ready += kv.second.st == State::Ready && kv.second.pw;
        }
        for (const auto& kv : g_ps) {
            ps_ready += kv.second.st == State::Ready;
            ps_rw_ready += kv.second.st == State::Ready && kv.second.twin != nullptr;
            e2i_ready += kv.second.st == State::Ready && kv.second.e2i > 0;
        }
        last = g_last_fail;
        why = g_conv_why;
    }
    const uint64_t procs = c_procs.load();
    char ipb[200];
    if (g_ip_k) std::snprintf(ipb, sizeof(ipb), "; instprev K %d: %zu VS ready (%zu at start, %zu lines of another K dropped)", g_ip_k, ip_ready, s_ip_vs, s_ip_bad);
    else std::snprintf(ipb, sizeof(ipb), "; instprev off");
    char psb[160];  // (run 20) the rewritten pixel shaders
    std::snprintf(psb, sizeof(psb), "; ps rewritten %llu converted, %zu ready (%zu at start)", static_cast<unsigned long long>(c_ps_rw.load()),
                  ps_rw_ready, s_ps_rw);
    char e2b[400] = "";  // (run 25) the eye2img twins; nothing with the switch off (the line as before)
    if (g_e2i)
        std::snprintf(e2b, sizeof(e2b), "; eye2img --eye2-images %s: %llu converted, %zu ready (%zu at start, %zu lines dropped), %llu refused for the images; "
                      "eye2_images %s",
                      g_e2i_arg.c_str(), static_cast<unsigned long long>(c_e2i.load()), e2i_ready, s_e2i, s_e2i_bad,
                      static_cast<unsigned long long>(c_e2i_refused.load()), e2i_health_text(eye2_images::health()));
    char pwb[200] = "";  // (run 26) the prevworld twins; nothing with the switch off (the line as before)
    if (g_pw)
        std::snprintf(pwb, sizeof(pwb), "; prevworld --prevworld: %llu converted, %zu ready (%zu at start, %zu lines dropped)",
                      static_cast<unsigned long long>(c_pw.load()), pw_ready, s_pw, s_pw_bad);
    char b[2800];
    std::snprintf(b, sizeof(b),
                  "auto on: converter %s%s; queue %zu; queued %llu, converted %llu (twins %llu, shared %llu, ps safe %llu), refused %llu, "
                  "failed %llu (crashes %llu, timeouts %llu); ready now %zu VS, %zu PS; cache at start %zu VS, %zu PS, %zu refused, %zu corrupt "
                  "deleted; cache hits %llu VS, %llu PS; processes %llu, CPU %.0f ms (%.1f ms a shader), wall %.0f ms (max %.0f ms)%s%s%s%s%s%s | %s",
                  g_converter.load() ? "on" : "OFF: ", g_converter.load() ? "" : why.c_str(), depth,
                  static_cast<unsigned long long>(c_queued.load()), static_cast<unsigned long long>(c_ok.load() + c_shared.load() + c_safe.load() + c_ps_rw.load()),
                  static_cast<unsigned long long>(c_ok.load()), static_cast<unsigned long long>(c_shared.load()),
                  static_cast<unsigned long long>(c_safe.load()), static_cast<unsigned long long>(c_refused.load()),
                  static_cast<unsigned long long>(c_failed.load()), static_cast<unsigned long long>(c_crashes.load()),
                  static_cast<unsigned long long>(c_timeouts.load()), vs_ready, ps_ready, s_vs, s_ps, s_refused, s_corrupt,
                  static_cast<unsigned long long>(c_hits_vs.load()), static_cast<unsigned long long>(c_hits_ps.load()),
                  static_cast<unsigned long long>(procs), c_cpu_us.load() / 1000.0, procs ? c_cpu_us.load() / 1000.0 / static_cast<double>(procs) : 0.0,
                  c_wall_us.load() / 1000.0, c_wall_max_us.load() / 1000.0, ipb, psb, e2b, pwb, last.empty() ? "" : "; last failure ", last.c_str(),
                  g_dir.c_str());
    return b;
}

}  // namespace rdrvr::vi_auto
