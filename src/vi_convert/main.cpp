// Single-pass stereo, the converter in C++ (fork-port-plan.md §5): the game's own shaders to view-instanced twins, in
// process through one dxcompiler.dll (our open-source build of DXC v1.9.2609), no Python and no child process.
//
// Step A (run 14 item 7): what stereo_vs.py does with its child processes (dxa, dxa -extractpart=module, dxopt
// -viewid-state, dxa, dxv), for each rewritten shader's LLVM IR text:
//   1. IDxcAssembler::AssembleToContainer (the text parsed by dxcompiler's own parseIR, as dxa does);
//   2. the DXIL part's bitcode (past its program header, as dxa's "module");
//   3. IDxcOptimizer::RunOptimizer with -hlsl-dxilload -viewid-state -hlsl-dxilemit (the ViewID dependence tables);
//   4. AssembleToContainer again (from the bitcode);
//   5. IDxcValidator::Validate with DxcValidatorFlags_InPlaceEdit: validated and signed (the open-source hash).
// Step B (run 18 item 3): the rest of the chain too: the disassembly (IDxcCompiler::Disassemble, what dxc -dumpbin
// calls), the rewrite (vi_rewrite.cpp: viewid_patch.py ported, refusing by default) and stereo_vs.py's sorting of each
// shader (rewritten, safe, shared as is, no twin, refused) with its output files (report.txt, ps_safe.txt, no_twin.txt).
//
// Build: build.bat (cl /EHsc /O2 vi_convert.cpp vi_rewrite.cpp ole32.lib)
// Run:   vi_convert <dxcompiler.dll> <dir of .ll> <out dir>                      step A: <out>/<kind>_<fnv>.dxil
//        vi_convert <dxcompiler.dll> --dxbc <dir of .dxbc> <out dir> [--rule viewid_patch|vi_rewrite] [--ref <dir>]
//                   [--position report|refuse] [--layout fork|ours] [--instprev K] [--eye2-images LB2,SC2[,GUARD]] [--prevworld]
//          step B, from the game's containers (<kind>_<fnv>.dxbc) as stereo_vs.py takes them; writes stereo_vs.py's
//          outputs plus timing.csv (per shader: the disassembly, the rewrite, the build, in ms);
//          --rule: viewid_patch (the default: the chain's, vi_chain.py / stereo_vs.py) or vi_rewrite (the mod's own,
//          the P0 twins: vi_rewrite.py's dyn mode, its own ViewID state, no -viewid-state pass);
//          --layout ours: refuse the rewrites that read what the mod's runtime does not write (the viewid_patch
//          rule: the fork's rows 21-31 fields and eye-2 slots; the vi_rewrite rule: rage_prev_world rows 5-8);
//          --ref <dir>: ref_texts.py's (or ref_ours.py's) output (dis/, ll/): each shader's disassembly and rewritten
//          text compared with the Python's, text for text;
//          --position: the twin's SV_Position checked for a ViewID dependence (from its own disassembly): report it,
//          or refuse a vertex shader whose position depends on ViewID in no component.
//          --eye2-images LB2,SC2[,GUARD] (run 24; the vi_rewrite rule, pixel shaders only): as --one's.
//          --prevworld (run 26; the vi_rewrite rule with --layout ours, vertex shaders only): as --one's.
//        vi_convert --rewrite|--rewrite-vi <text>...                  the rewrite alone on disassembly texts (tests)
//        vi_convert <dxcompiler.dll> --build-ll <in.ll> <out.dxil> [--no-viewid-state]      one text built
// Step C (run 19 item 4): the product, rdrvr_vi_convert.exe (CMake, next to dinput8.dll), run by the mod
// (src/core/vi_auto.cpp) as a child process for each new shader it sees ([Stereo] InstancedShadersAuto, test only):
//        rdrvr_vi_convert --version
//        rdrvr_vi_convert --one <vs|ps> <in.dxbc> <out.dxil> <result.txt> [--dxc <dll>] [--crash-test|--hang-test]
//                         [--instprev K] [--eye2-images LB2,SC2[,GUARD]] [--prevworld]
//          one container in, its twin (or the verdict) out, always the mod's own rule (vi_rewrite, --layout ours);
//          (run 20) a PS with per-eye reads the rule redirects gets its rewritten twin PS ("rewritten <n>[ wvp]");
//          --instprev K (run 19 item 2, test only; run 20: the mod passes it for every VS when [Stereo]
//          PrepassInstPrevMirror=1 at start, its PrepassInstPrevK): rage_instancemtxprev read at row + ViewID x 4K
//          ([Stereo] PrepassInstPrevMirror's layout, the same K), the result line "ok <n> instprev[ wvp]"; a VS that
//          does not read that block is converted byte for byte as without it;
//          --eye2-images LB2,SC2[,GUARD] (run 24, P3's read side, test only, ps only; off by default): global_textures
//          uints 11 (the Light Buffer's bindless index) and 13 (the Shadow Collector's) read as
//          (ViewID != 0 && uint GUARD == magic) ? uint LB2 : uint 11 (SC2 for 13; vi_rewrite.h, protocol.h), the result
//          line "rewritten <n>[ wvp] eye2img<k>"; a PS that reads them in any form the rule does not understand is
//          "refused eye-2 images: ..."; one whose reads never reach them is converted byte for byte as without it;
//          --prevworld (run 26, GOAL-single-pass item 6a, test only, vs only; off by default): rage_prev_world rows 0-3
//          (gPrevWorldViewProjSingle) read at row + ViewID x 5, eye 2's rows 5-8 that [Stereo] PrepassPrevWorldMirror writes
//          (vi_rewrite.h), the result line "ok <n>[ instprev][ wvp] prevworld"; a VS that does not read those rows is
//          converted byte for byte as without it;
//          --text-input (tests only): <in> is a disassembly text, not a container (tests/offline/vi_eye2_images.py);
//          protocol.h has the result lines. The DLL defaults to rdrvr_dxcompiler.dll next to this exe, loaded by full
//          path (never the game's own dxcompiler.dll). The sources moved here from research\sps\tools\vi_convert (one
//          copy); its build.bat and Python checks build and run these same files.
#include <windows.h>
#include <dxcapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "protocol.h"
#include "vi_rewrite.h"

// (run 24) the mod's protocol and the rule agree on the guard and the defaults
static_assert(rdrvr::vi_protocol::kEye2GuardMagic == vi::kEye2GuardMagic, "the guard's magic");
static_assert(rdrvr::vi_protocol::kEye2DefaultLb >= vi::kEye2UintLo && rdrvr::vi_protocol::kEye2DefaultLb <= vi::kEye2UintHi &&
                  rdrvr::vi_protocol::kEye2DefaultSc >= vi::kEye2UintLo && rdrvr::vi_protocol::kEye2DefaultSc <= vi::kEye2UintHi &&
                  rdrvr::vi_protocol::kEye2DefaultGuard >= vi::kEye2UintLo && rdrvr::vi_protocol::kEye2DefaultGuard <= vi::kEye2UintHi,
              "the default uints in 48..58");

namespace {
bool g_safe_dll_search = false;  // (--one sets it, with SetDefaultDllDirectories)
template <class T> struct Ptr {  // a minimal COM pointer
    T* p = nullptr;
    ~Ptr() {
        if (p) p->Release();
    }
    T** put() {
        if (p) p->Release();
        p = nullptr;
        return &p;
    }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};
bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = n > 0 ? fread(out->data(), 1, out->size(), f) : 0;
    fclose(f);
    return got == out->size();
}
bool write_file(const std::string& path, const void* p, size_t n) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const size_t w = fwrite(p, 1, n, f);
    fclose(f);
    return w == n;
}
std::string errors_of(IDxcOperationResult* r) {
    Ptr<IDxcBlobEncoding> e;
    if (FAILED(r->GetErrorBuffer(e.put())) || !e.p || e->GetBufferSize() == 0) return "";
    std::string s(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize());
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s.substr(0, 200);
}
// the DXIL part's bitcode: DxilProgramHeader (version, size in dwords, then the bitcode header at byte 8: 'DXIL',
// version, the bitcode's offset from that header, its size), as hlsl::GetDxilProgramBitcode
bool program_bitcode(const uint8_t* part, size_t n, size_t* off, size_t* len) {
    if (n < 24 || std::memcmp(part + 8, "DXIL", 4) != 0) return false;
    uint32_t bc_off = 0, bc_len = 0;
    std::memcpy(&bc_off, part + 16, 4);
    std::memcpy(&bc_len, part + 20, 4);
    *off = 8 + static_cast<size_t>(bc_off);  // (from the bitcode header's start, byte 8: DxilContainer.h GetDxilProgramBitcode)
    *len = bc_len;
    return *off + *len <= n;
}
double now_ms() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

struct Dxc {
    DxcCreateInstanceProc create = nullptr;
    Ptr<IDxcUtils> utils;
    Ptr<IDxcAssembler> asmb;
    Ptr<IDxcOptimizer> opt;
    Ptr<IDxcValidator> val;
    Ptr<IDxcCompiler> comp;
    bool init(const char* dll_path) {
        if (g_safe_dll_search) {  // (--one: a full path only; its own folder and System32 for what it imports)
            HMODULE m = LoadLibraryExA(dll_path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            create = m ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(m, "DxcCreateInstance")) : nullptr;
            return create && objects();
        }
        // (a full path: the altered search order looks for the DLL's own dependencies beside it, not in the cwd)
        const bool full = std::strchr(dll_path, ':') || (dll_path[0] == '\\' && dll_path[1] == '\\');
        HMODULE dll = full ? LoadLibraryExA(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : LoadLibraryA(dll_path);
        create = dll ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance")) : nullptr;
        if (!create) return false;
        return objects();
    }
    bool objects() {
        return SUCCEEDED(create(CLSID_DxcUtils, IID_PPV_ARGS(utils.put()))) &&
               SUCCEEDED(create(CLSID_DxcAssembler, IID_PPV_ARGS(asmb.put()))) &&
               SUCCEEDED(create(CLSID_DxcOptimizer, IID_PPV_ARGS(opt.put()))) &&
               SUCCEEDED(create(CLSID_DxcValidator, IID_PPV_ARGS(val.put()))) &&
               SUCCEEDED(create(CLSID_DxcCompiler, IID_PPV_ARGS(comp.put())));
    }
    // the container's disassembly as stereo_vs.py sees dxc -dumpbin's: (stdout + stderr).strip(), universal newlines
    bool disassemble(const std::vector<uint8_t>& c, std::string* text, std::string* why) {
        Ptr<IDxcBlobEncoding> src;
        Ptr<IDxcBlobEncoding> dis;
        if (FAILED(utils->CreateBlob(c.data(), static_cast<UINT32>(c.size()), 0, src.put())) || FAILED(comp->Disassemble(src, dis.put())) ||
            !dis.p) {
            *why = "Disassemble";
            return false;
        }
        const char* p = static_cast<const char*>(dis->GetBufferPointer());
        size_t n = dis->GetBufferSize();
        while (n && p[n - 1] == '\0') --n;  // (a blob's terminator; the console write stops at it)
        std::string s;
        s.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == '\r') {
                s += '\n';
                if (i + 1 < n && p[i + 1] == '\n') ++i;
            } else {
                s += p[i];
            }
        }
        auto ws = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; };
        size_t a = 0, b = s.size();
        while (a < b && ws(s[a])) ++a;
        while (b > a && ws(s[b - 1])) --b;
        *text = s.substr(a, b - a);
        return true;
    }
    // step A: IR text -> a validated, signed container
    // viewid_state false: the text carries its own dx.viewIdState (vi_rewrite.py's rule: dxa, then dxv, as the P0 set)
    bool build(const std::vector<uint8_t>& text, std::vector<uint8_t>* out, std::string* step, std::string* why,
               bool viewid_state = true) {
        Ptr<IDxcBlobEncoding> src;
        Ptr<IDxcOperationResult> r1;
        HRESULT st = E_FAIL;
        *step = "assemble";
        if (FAILED(utils->CreateBlob(text.data(), static_cast<UINT32>(text.size()), CP_UTF8, src.put())) ||
            FAILED(asmb->AssembleToContainer(src, r1.put())) || FAILED(r1->GetStatus(&st)) || FAILED(st)) {
            *why = r1.p ? errors_of(r1) : "";
            return false;
        }
        Ptr<IDxcBlob> c1;
        r1->GetResult(c1.put());
        if (!viewid_state) return sign(c1, out, step, why);
        Ptr<IDxcContainerReflection> refl;
        UINT32 idx = 0;
        Ptr<IDxcBlob> part;
        size_t off = 0, len = 0;
        *step = "extract";
        if (FAILED(create(CLSID_DxcContainerReflection, IID_PPV_ARGS(refl.put()))) || FAILED(refl->Load(c1)) ||
            FAILED(refl->FindFirstPartKind(DXC_PART_DXIL, &idx)) || FAILED(refl->GetPartContent(idx, part.put())) ||
            !program_bitcode(static_cast<const uint8_t*>(part->GetBufferPointer()), part->GetBufferSize(), &off, &len)) {
            *why = "";
            return false;
        }
        Ptr<IDxcBlobEncoding> bc;
        utils->CreateBlob(static_cast<const uint8_t*>(part->GetBufferPointer()) + off, static_cast<UINT32>(len), 0, bc.put());
        LPCWSTR args[3] = {L"-hlsl-dxilload", L"-viewid-state", L"-hlsl-dxilemit"};
        Ptr<IDxcBlob> bc2;
        Ptr<IDxcBlobEncoding> opt_text;
        *step = "viewid-state";
        if (FAILED(opt->RunOptimizer(bc, args, 3, bc2.put(), opt_text.put())) || !bc2.p) {
            *why = "";
            return false;
        }
        Ptr<IDxcBlobEncoding> bc2e;
        utils->CreateBlob(bc2->GetBufferPointer(), static_cast<UINT32>(bc2->GetBufferSize()), 0, bc2e.put());
        Ptr<IDxcOperationResult> r2;
        st = E_FAIL;
        *step = "assemble 2";
        if (FAILED(asmb->AssembleToContainer(bc2e, r2.put())) || FAILED(r2->GetStatus(&st)) || FAILED(st)) {
            *why = r2.p ? errors_of(r2) : "";
            return false;
        }
        Ptr<IDxcBlob> c2;
        r2->GetResult(c2.put());
        return sign(c2, out, step, why);
    }
    // validated and signed in place (a writable copy)
    bool sign(IDxcBlob* c2, std::vector<uint8_t>* out, std::string* step, std::string* why) {
        Ptr<IDxcBlobEncoding> c2w;
        utils->CreateBlob(c2->GetBufferPointer(), static_cast<UINT32>(c2->GetBufferSize()), 0, c2w.put());
        Ptr<IDxcOperationResult> r3;
        HRESULT st = E_FAIL;
        *step = "validate";
        if (FAILED(val->Validate(c2w, DxcValidatorFlags_InPlaceEdit, r3.put())) || FAILED(r3->GetStatus(&st)) || FAILED(st)) {
            *why = r3.p ? errors_of(r3) : "";
            return false;
        }
        const uint8_t* signed_p = static_cast<const uint8_t*>(c2w->GetBufferPointer());
        static const uint8_t zero[16] = {};
        *step = "sign";
        if (c2w->GetBufferSize() < 20 || std::memcmp(signed_p + 4, zero, 16) == 0) {
            *why = "the container digest is empty";
            return false;
        }
        out->assign(signed_p, signed_p + c2w->GetBufferSize());
        return true;
    }
};

std::vector<std::string> list_dir(const std::string& dir, const char* pattern) {  // sorted as Python sorts the names
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    auto low = [](std::string s) {
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) { return low(a) < low(b); });
    return out;
}
bool read_text(const std::string& path, std::string* out) {
    std::vector<uint8_t> b;
    if (!read_file(path, &b)) return false;
    out->assign(b.begin(), b.end());
    return true;
}

// SV_Position's ViewID dependence in a container's disassembly: "all", "some", "none", or "?" (not found)
std::string position_viewid(const std::string& dis) {
    size_t k = dis.find("; Output signature:");
    if (k == std::string::npos) return "?";
    k = dis.find("; ----", k);
    if (k == std::string::npos) return "?";
    int reg = -1;
    for (size_t a = dis.find('\n', k) + 1; a != 0 && a < dis.size();) {
        size_t b = dis.find('\n', a);
        if (b == std::string::npos) b = dis.size();
        const std::string ln = dis.substr(a, b - a);
        if (ln == ";" || ln.compare(0, 2, "; ") != 0) break;
        char name[64] = {}, mask[16] = {};
        int index = 0, r = 0;
        if (sscanf_s(ln.c_str() + 2, "%63s %d %15s %d", name, static_cast<unsigned>(sizeof(name)), &index, mask,
                     static_cast<unsigned>(sizeof(mask)), &r) == 4 &&
            std::strcmp(name, "SV_Position") == 0) {
            reg = r;
            break;
        }
        a = b + 1;
    }
    if (reg < 0) return "?";
    const size_t d = dis.find("; Outputs dependent on ViewId: {");
    if (d == std::string::npos) return "none";
    const size_t e = dis.find('}', d);
    std::vector<int> deps;
    for (size_t p = d + 32; p < e;) {
        while (p < e && (dis[p] < '0' || dis[p] > '9')) ++p;
        if (p >= e) break;
        deps.push_back(atoi(dis.c_str() + p));
        while (p < e && dis[p] >= '0' && dis[p] <= '9') ++p;
    }
    int n = 0;
    for (int c = 0; c < 4; ++c) n += std::count(deps.begin(), deps.end(), reg * 4 + c) ? 1 : 0;
    return n == 4 ? "all" : n ? "some" : "none";
}

// "LB2,SC2[,GUARD]" (--eye2-images): three different uints in vi::kEye2UintLo..kEye2UintHi (48..58), GUARD by default
// protocol.h's kEye2DefaultGuard
bool parse_eye2(const char* s, vi::Eye2Images* e2) {
    int v[3] = {0, 0, rdrvr::vi_protocol::kEye2DefaultGuard}, k = 0;
    const char* p = s;
    for (;; ++k) {
        if (k == 3 || *p < '0' || *p > '9') return false;
        long long x = 0;
        for (; *p >= '0' && *p <= '9'; ++p) {
            x = x * 10 + (*p - '0');
            if (x > vi::kEye2UintHi) return false;
        }
        if (x < vi::kEye2UintLo) return false;
        v[k] = static_cast<int>(x);
        if (*p == '\0') break;
        if (*p++ != ',') return false;
    }
    if (k < 1 || v[0] == v[1] || v[0] == v[2] || v[1] == v[2]) return false;
    e2->lb = v[0];
    e2->sc = v[1];
    e2->guard = v[2];
    return true;
}

const std::map<std::string, std::string> kNoTwinPs = {
    {"PS_Blit", "the refraction copy reads FullScreenCopy through TextureSampler"},
    {"PSMiragerSingle", "the heat haze reads the scene copy through RenderMapBilinearSampler"},
};

int step_b(Dxc& dxc, const std::string& in, const std::string& out, const std::string& ref, const std::string& position,
           const std::string& rule, bool our_layout, int instprev_k = 0, const vi::Eye2Images& e2i = vi::Eye2Images{}, bool prevworld = false) {
    const bool ours = rule == "vi_rewrite";
    CreateDirectoryA(out.c_str(), nullptr);
    const auto files = list_dir(in, "*.dxbc");
    if (files.empty()) {
        std::printf("no .dxbc in %s\n", in.c_str());
        return 1;
    }
    std::vector<std::string> report, safe, no_twin;
    std::map<std::string, int> counts;
    std::map<std::string, int> refusals;  // reason -> count
    std::string timing = "shader,verdict,read_ms,disassemble_ms,rewrite_ms,build_ms,position_ms,total_ms,position\n";
    int dis_same = 0, dis_diff = 0, ll_same = 0, ll_diff = 0, ll_missing = 0, ll_extra = 0;
    std::vector<std::string> diffs;
    double t_dis = 0, t_rw = 0, t_build = 0, t_pos = 0, t_read = 0;
    std::vector<double> per;
    const double t0 = now_ms();
    for (const auto& name : files) {
        const double s0 = now_ms();
        const std::string stem = name.substr(0, name.size() - 5);
        const size_t us = stem.find('_');
        const std::string kind = us == std::string::npos ? "vs" : stem.substr(0, us), h = us == std::string::npos ? stem : stem.substr(us + 1);
        std::vector<uint8_t> bytes;
        std::string verdict, pos = "-";
        double d_dis = 0, d_rw = 0, d_build = 0, d_pos = 0;
        const bool rd = read_file(in + "\\" + name, &bytes);
        const double d_read = now_ms() - s0;
        std::string text, why;
        double s1 = now_ms();
        const bool dis_ok = rd && dxc.disassemble(bytes, &text, &why);
        d_dis = now_ms() - s1;
        if (!dis_ok) {
            report.push_back(kind + " " + h + " failed dumpbin: " + (rd ? why : "read"));
            ++counts["failed"];
            verdict = "failed";
        } else {
            if (!ref.empty()) {
                std::string r;
                if (read_text(ref + "\\dis\\" + stem + ".txt", &r)) {
                    if (r == text) ++dis_same;
                    else {
                        ++dis_diff;
                        if (diffs.size() < 20) diffs.push_back("dis " + stem);
                    }
                }
            }
            const std::string entry = vi::entry_name(text);
            auto nt = kNoTwinPs.find(entry);
            if (kind == "ps" && !entry.empty() && nt != kNoTwinPs.end()) {
                report.push_back("ps " + h + " no twin (" + entry + ": " + nt->second + ")");
                no_twin.push_back(h);
                ++counts["no twin"];
                verdict = "no twin";
            } else {
                s1 = now_ms();
                bool wvp = false;
                int e2 = 0, pw = 0;
                const bool e2_this = kind == "ps" && e2i.on();  // (--eye2-images: pixel shaders only)
                const bool pw_this = kind == "vs" && prevworld;  // (run 26, --prevworld: vertex shaders only)
                const vi::Rewrite rw = ours ? vi::rewrite_ours(text, &wvp, our_layout, instprev_k, nullptr, e2_this ? e2i : vi::Eye2Images{}, &e2,
                                                               pw_this, &pw)
                                            : vi::rewrite(text, our_layout);
                d_rw = now_ms() - s1;
                if (!ref.empty()) {
                    std::string r;
                    const bool have = read_text(ref + "\\ll\\" + stem + ".ll", &r);
                    if (have && rw.ok) {
                        if (r == rw.text) ++ll_same;
                        else {
                            ++ll_diff;
                            if (diffs.size() < 20) diffs.push_back("ll " + stem);
                        }
                    } else if (have) {
                        ++ll_missing;
                        if (diffs.size() < 20) diffs.push_back("ll not made " + stem + " (" + rw.refused + ")");
                    } else if (rw.ok) {
                        ++ll_extra;
                        if (diffs.size() < 20) diffs.push_back("ll made, not in the reference " + stem);
                    }
                }
                if (!rw.ok && rw.refused == "no view-dependent block") {
                    if (kind == "ps") {
                        safe.push_back(h);
                        report.push_back("ps " + h + " safe");
                        verdict = "safe";
                    } else {
                        write_file(out + "\\vs_" + h + ".dxil", bytes.data(), bytes.size());
                        report.push_back("vs " + h + " shared as is");
                        verdict = "shared";
                    }
                    ++counts["safe"];
                } else if (!rw.ok) {
                    report.push_back(kind + " " + h + " refused: " + rw.refused);
                    ++counts["refused"];
                    ++refusals[rw.refused];
                    verdict = "refused";
                } else {
                    std::vector<uint8_t> container;
                    std::string step;
                    const std::vector<uint8_t> tb(rw.text.begin(), rw.text.end());
                    s1 = now_ms();
                    const bool built = dxc.build(tb, &container, &step, &why, !ours);
                    d_build = now_ms() - s1;
                    bool refused = false;
                    if (built && kind == "vs" && !position.empty()) {
                        s1 = now_ms();
                        std::string d2;
                        if (dxc.disassemble(container, &d2, &why)) pos = position_viewid(d2);
                        d_pos = now_ms() - s1;
                        if (position == "refuse" && pos != "all" && pos != "some") {
                            const std::string reason = "strict: SV_Position depends on ViewID in no component (" + pos + ")";
                            report.push_back(kind + " " + h + " refused: " + reason);
                            ++counts["refused"];
                            ++refusals[reason];
                            verdict = "refused";
                            refused = true;
                        }
                    }
                    if (refused) {
                    } else if (!built) {
                        report.push_back(kind + " " + h + " failed " + step + ": " + why);
                        ++counts["failed"];
                        verdict = "failed";
                    } else if (!write_file(out + "\\" + kind + "_" + h + ".dxil", container.data(), container.size())) {
                        report.push_back(kind + " " + h + " failed write");
                        ++counts["failed"];
                        verdict = "failed";
                    } else {
                        report.push_back(kind + " " + h + " ok (" + std::to_string(rw.n) + " row reads" +
                                         (rw.approx ? "; motion vectors through D" : "") +
                                         (wvp ? "; gWorldViewProj: eye 2's is per draw, [Stereo] PrepassWvpMirror" : "") +
                                         (e2 ? "; eye 2's images: " + std::to_string(e2) + " indices" : "") +
                                         (pw ? "; rage_prev_world rows 5-8: " + std::to_string(pw) + " loads, [Stereo] PrepassPrevWorldMirror" : "") + ")");
                        ++counts["ok"];
                        verdict = "ok";
                    }
                }
            }
        }
        const double tot = now_ms() - s0;
        t_read += d_read;
        t_dis += d_dis;
        t_rw += d_rw;
        t_build += d_build;
        t_pos += d_pos;
        per.push_back(tot);
        char line[256];
        std::snprintf(line, sizeof(line), "%s,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s\n", stem.c_str(), verdict.c_str(), d_read, d_dis,
                      d_rw, d_build, d_pos, tot, pos.c_str());
        timing += line;
    }
    const double total = now_ms() - t0;
    // (CRLF: stereo_vs.py's write_text in text mode on Windows, so the lists come out byte for byte the same)
    auto lines = [](const std::vector<std::string>& v, bool trailing) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) s += v[i] + (trailing || i + 1 < v.size() ? "\r\n" : "");
        return s;
    };
    const std::string rep = lines(report, false) + "\r\n", ps = lines(safe, true), nt = lines(no_twin, true);
    write_file(out + "\\no_twin.txt", nt.data(), nt.size());
    write_file(out + "\\ps_safe.txt", ps.data(), ps.size());
    write_file(out + "\\report.txt", rep.data(), rep.size());
    write_file(out + "\\timing.csv", timing.data(), timing.size());
    std::sort(per.begin(), per.end());
    std::printf("ok %d, safe %d, no twin %d, refused %d, failed %d -> %s\n", counts["ok"], counts["safe"], counts["no twin"],
                counts["refused"], counts["failed"], out.c_str());
    for (const auto& kv : refusals) std::printf("  refused %3d: %s\n", kv.second, kv.first.c_str());
    const double n = static_cast<double>(files.size());
    std::printf("time: %.0f ms for %zu shaders (%.2f ms a shader; median %.2f, max %.2f); read %.0f, disassemble %.0f, rewrite %.0f, "
                "build %.0f, position %.0f ms\n",
                total, files.size(), total / n, per[per.size() / 2], per.back(), t_read, t_dis, t_rw, t_build, t_pos);
    if (!ref.empty()) {
        std::printf("against the Python's texts: disassembly %d the same, %d differ; rewrite %d the same, %d differ, %d not made "
                    "here, %d made here only\n",
                    dis_same, dis_diff, ll_same, ll_diff, ll_missing, ll_extra);
        for (const auto& d : diffs) std::printf("  %s\n", d.c_str());
    }
    return counts["failed"] ? 1 : 0;
}

int step_a(Dxc& dxc, const std::string& in, const std::string& out) {
    CreateDirectoryA(out.c_str(), nullptr);
    const auto files = list_dir(in, "*.ll");
    if (files.empty()) {
        std::printf("no .ll in %s\n", in.c_str());
        return 1;
    }
    int ok = 0, failed = 0;
    const DWORD t0 = GetTickCount();
    for (const auto& name : files) {
        const std::string stem = name.substr(0, name.size() - 3);
        std::vector<uint8_t> text, container;
        std::string step, why;
        if (!read_file(in + "\\" + name, &text)) {
            step = "read";
        } else if (dxc.build(text, &container, &step, &why)) {
            if (write_file(out + "\\" + stem + ".dxil", container.data(), container.size())) {
                ++ok;
                continue;
            }
            step = "write";
        }
        std::printf("%s failed %s %s\n", stem.c_str(), step.c_str(), why.c_str());
        ++failed;
    }
    std::printf("DONE: %d ok, %d failed, %lu ms\n", ok, failed, static_cast<unsigned long>(GetTickCount() - t0));
    return failed ? 1 : 0;
}

// ---- step C: one request from the mod (protocol.h)
std::string exe_dir() {
    char p[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::string s(p, n);
    const size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? "" : s.substr(0, k + 1);
}
// the verdict of one container under the mod's rule (vi_rewrite, our layout): the result line, and *out when the line
// says a file comes with it ("ok", "shared")
std::string convert_one(Dxc& dxc, const std::string& kind, const std::vector<uint8_t>& bytes, std::vector<uint8_t>* out, int instprev_k = 0,
                        const vi::Eye2Images& e2i = vi::Eye2Images{}, const std::string* text_in = nullptr, bool prevworld = false) {
    std::string text, why;
    if (text_in) text = *text_in;  // (--text-input, tests only: the disassembly given)
    else if (!dxc.disassemble(bytes, &text, &why)) return "failed dumpbin " + why;
    const std::string entry = vi::entry_name(text);
    auto nt = kNoTwinPs.find(entry);
    if (kind == "ps" && !entry.empty() && nt != kNoTwinPs.end()) return "refused no twin (" + entry + ": " + nt->second + ")";
    bool wvp = false;
    int ip = 0, e2 = 0, pw = 0;
    const vi::Rewrite rw = vi::rewrite_ours(text, &wvp, true, instprev_k, &ip, e2i, &e2, prevworld && kind == "vs", &pw);
    if (!rw.ok && rw.refused == "no view-dependent block") {
        if (kind == "ps") return "safe";
        *out = bytes;  // (shared as is: the game's own container draws both views the same)
        return "shared";
    }
    if (!rw.ok) return "refused " + rw.refused;
    // (run 20) a pixel shader with per-eye reads the rule redirects (run 14's P0 PS: PSShadowBlendRDR2_Fadeless's camera row
    // 15 -> 28) is rewritten and built as step B's batch builds it (--rule vi_rewrite --layout ours: the same rewrite_ours,
    // the same build without the -viewid-state pass), so its twin PS is byte for byte step B's (and twins_p0's)
    if (kind == "ps" && ip) return "refused rage_instancemtxprev in a pixel shader";  // (never: no K is passed for a PS)
    if (kind == "ps" && pw) return "refused rage_prev_world rows 5-8 in a pixel shader";  // (never: --prevworld is a VS's only)
    std::string step;
    const std::vector<uint8_t> tb(rw.text.begin(), rw.text.end());
    if (!dxc.build(tb, out, &step, &why, false)) return "failed " + step + " " + why;
    if (kind == "ps") return "rewritten " + std::to_string(rw.n) + (wvp ? " wvp" : "") + (e2 ? " eye2img" + std::to_string(e2) : "");
    return "ok " + std::to_string(rw.n) + (ip ? " instprev" : "") + (wvp ? " wvp" : "") + (pw ? " prevworld" : "");
}
int one(int argc, char** argv) {
    namespace P = rdrvr::vi_protocol;
    // (no error boxes, ever: a crash here must end this process quietly, the game never waits on a dialog)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // DLLs only from System32 and the folders added on purpose: never a dxil.dll (or anything) from PATH or the cwd.
    // The DXC DLL itself is loaded by full path with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR (its own folder for its imports).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    g_safe_dll_search = true;
    if (argc < 6) return P::kExitUsage;
    const std::string kind = argv[2], in = argv[3], out = argv[4], result = argv[5];
    if (kind != "vs" && kind != "ps") return P::kExitUsage;
    std::string dll = exe_dir() + P::kDxc;
    bool crash = false, hang = false, text_input = false, prevworld = false;
    int instprev_k = 0;
    vi::Eye2Images e2i;
    for (int i = 6; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dxc") == 0 && i + 1 < argc) dll = argv[++i];
        else if (std::strcmp(argv[i], "--rule") == 0 && i + 1 < argc) {  // the mod's rule hash: another build's converter refuses
            if (std::strcmp(argv[++i], P::kRuleHash) != 0) {
                std::printf("the rule %s is not this converter's %s\n", argv[i], P::kRuleHash);
                return P::kExitRuleMismatch;
            }
        }
        else if (std::strcmp(argv[i], "--crash-test") == 0) crash = true;
        else if (std::strcmp(argv[i], "--hang-test") == 0) hang = true;
        else if (std::strcmp(argv[i], "--instprev") == 0 && i + 1 < argc) {  // (run 19 item 2: the twins_p1 set, made by hand)
            instprev_k = std::atoi(argv[++i]);
            if (instprev_k < 1 || instprev_k > 768) return P::kExitUsage;
        }
        else if (std::strcmp(argv[i], "--eye2-images") == 0 && i + 1 < argc) {  // (run 24: a pixel shader's only)
            if (kind != "ps" || !parse_eye2(argv[++i], &e2i)) return P::kExitUsage;
        }
        else if (std::strcmp(argv[i], "--prevworld") == 0) {  // (run 26: a vertex shader's only)
            if (kind != "vs") return P::kExitUsage;
            prevworld = true;
        }
        else if (std::strcmp(argv[i], "--text-input") == 0) text_input = true;  // (tests only: <in> is a disassembly text)
        else return P::kExitUsage;
    }
    std::vector<uint8_t> bytes;
    if (!read_file(in, &bytes)) return 1;  // (not read, or locked: a failure of this session only, never cached)
    const std::string text_in(bytes.begin(), bytes.end());
    if (!text_input && (bytes.size() < 32 || std::memcmp(bytes.data(), "DXBC", 4) != 0)) {  // (read, and really not a container: for good)
        const std::string line = "refused not a DXBC container";
        return write_file(result, line.data(), line.size()) ? P::kExitOk : 1;
    }
    Dxc dxc;
    if (!dxc.init(dll.c_str())) {
        std::printf("the DXC objects not made from %s\n", dll.c_str());
        return P::kExitNoDxc;
    }
    if (crash) TerminateProcess(GetCurrentProcess(), P::kExitCrashTest);  // [Debug] ViConvertCrashTest=1 (an abnormal end)
    if (hang) Sleep(INFINITE);                                             // [Debug] ViConvertCrashTest=2 (the mod's timeout)
    std::vector<uint8_t> twin;
    std::string line;
    try {  // (vi_rewrite.cpp's own throws are caught inside it; anything else here is a refusal of this shader, for good)
        line = convert_one(dxc, kind, bytes, &twin, instprev_k, e2i, text_input ? &text_in : nullptr, prevworld);
    } catch (const std::bad_alloc&) {
        return 1;  // (out of memory: this session only, never cached)
    } catch (const std::exception& e) {
        twin.clear();
        line = std::string("failed exception ") + e.what();
    } catch (...) {
        twin.clear();
        line = "failed exception (not a std::exception)";
    }
    for (char& ch : line)
        if (ch == '\r' || ch == '\n') ch = ' ';
    if (!twin.empty() && !write_file(out, twin.data(), twin.size())) return 1;
    std::printf("%s %s\n", kind.c_str(), line.c_str());
    return write_file(result, line.data(), line.size()) ? P::kExitOk : 1;  // (last: a result line means the file is whole)
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("rdrvr_vi_convert %s %s\n", rdrvr::vi_protocol::kVersion, rdrvr::vi_protocol::kRuleHash);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--one") == 0) return one(argc, argv);
    if (argc >= 3 && (std::strcmp(argv[1], "--rewrite") == 0 || std::strcmp(argv[1], "--rewrite-vi") == 0)) {
        // the rewrite alone on disassembly texts, one verdict line each (--rewrite-vi: the vi_rewrite rule)
        // (run 24) --rewrite-vi --eye2-images LB2,SC2 <text>...: the rule's eye-2 images too (tests/offline/vi_eye2_images.py)
        const bool ours = std::strcmp(argv[1], "--rewrite-vi") == 0;
        int first = 2;
        vi::Eye2Images e2i;
        if (ours && argc >= 4 && std::strcmp(argv[2], "--eye2-images") == 0) {
            if (!parse_eye2(argv[3], &e2i)) {
                std::printf("--eye2-images LB2,SC2[,GUARD]: three different uints in %d..%d\n", vi::kEye2UintLo, vi::kEye2UintHi);
                return 2;
            }
            first = 4;
        }
        for (int i = first; i < argc; ++i) {
            std::string text;
            if (!read_text(argv[i], &text)) {
                std::printf("%s: not read\n", argv[i]);
                continue;
            }
            int e2 = 0;
            const vi::Rewrite r = ours ? vi::rewrite_ours(text, nullptr, false, 0, nullptr, e2i, &e2) : vi::rewrite(text);
            std::printf("%s: %s\n", argv[i],
                        r.ok ? ("ok (" + std::to_string(r.n) + " row reads" + (e2 ? ", eye2img " + std::to_string(e2) : "") + ")").c_str()
                             : r.refused.c_str());
        }
        return 0;
    }
    if (argc < 4) {
        std::printf("usage: vi_convert <dxcompiler.dll> <dir of .ll> <out dir>\n"
                    "       vi_convert <dxcompiler.dll> --dxbc <dir of .dxbc> <out dir> [--ref <dir>] [--position report|refuse]\n");
        return 2;
    }
    Dxc dxc;
    if (!dxc.init(argv[1])) {
        std::printf("the DXC objects not made from %s\n", argv[1]);
        return 1;
    }
    if (std::strcmp(argv[2], "--dumpbin") == 0 && argc >= 5) {  // (run 24) one container's disassembly, as the rule sees it (tests)
        std::vector<uint8_t> bytes;
        std::string text, why;
        if (!read_file(argv[3], &bytes) || !dxc.disassemble(bytes, &text, &why) || !write_file(argv[4], text.data(), text.size())) {
            std::printf("failed %s\n", why.c_str());
            return 1;
        }
        std::printf("ok %zu bytes\n", text.size());
        return 0;
    }
    if (std::strcmp(argv[2], "--build-ll") == 0 && argc >= 5) {  // one IR text built (step A), --no-viewid-state as the P0 rule
        std::vector<uint8_t> text, container;
        std::string step, why;
        const bool vs = !(argc >= 6 && std::strcmp(argv[5], "--no-viewid-state") == 0);
        if (!read_file(argv[3], &text) || !dxc.build(text, &container, &step, &why, vs) ||
            !write_file(argv[4], container.data(), container.size())) {
            std::printf("failed %s %s\n", step.c_str(), why.c_str());
            return 1;
        }
        std::printf("ok %zu bytes\n", container.size());
        return 0;
    }
    if (std::strcmp(argv[2], "--dxbc") == 0) {
        if (argc < 5) return 2;
        std::string ref, position, rule = "viewid_patch", layout = "fork";
        int instprev_k = 0;
        vi::Eye2Images e2i;
        bool prevworld = false;
        for (int i = 5; i + 1 < argc || (i < argc && std::strcmp(argv[i], "--prevworld") == 0); i += 2) {
            if (std::strcmp(argv[i], "--prevworld") == 0) {  // (run 26: no value)
                prevworld = true;
                --i;
            }
            else if (std::strcmp(argv[i], "--ref") == 0) ref = argv[i + 1];
            else if (std::strcmp(argv[i], "--position") == 0) position = argv[i + 1];
            else if (std::strcmp(argv[i], "--rule") == 0) rule = argv[i + 1];
            else if (std::strcmp(argv[i], "--layout") == 0) layout = argv[i + 1];
            else if (std::strcmp(argv[i], "--instprev") == 0) instprev_k = std::atoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--eye2-images") == 0) {
                if (!parse_eye2(argv[i + 1], &e2i)) return 2;
            }
            else return 2;
        }
        if (!position.empty() && position != "report" && position != "refuse") return 2;
        if ((rule != "viewid_patch" && rule != "vi_rewrite") || (layout != "fork" && layout != "ours")) return 2;
        if (instprev_k && (rule != "vi_rewrite" || instprev_k < 1 || instprev_k > 768)) return 2;  // (the mod's rule only)
        if (e2i.on() && rule != "vi_rewrite") return 2;                                             // (the mod's rule only)
        if (prevworld && (rule != "vi_rewrite" || layout != "ours")) return 2;                      // (run 26: the mod's rule and layout)
        return step_b(dxc, argv[3], argv[4], ref, position, rule, layout == "ours", instprev_k, e2i, prevworld);
    }
    return step_a(dxc, argv[2], argv[3]);
}
