// Single-pass stereo, the converter in C++, step B (run 18 item 3; fork-port-plan.md §5.2): a shader's disassembly
// (dumpbin text) rewritten into its view-instanced twin's text, in process, with no Python.
//
// A port to C++ of the rewrite in the chain's converter, tools/sp/dxil/viewid_patch.py of optimumbox's fork
// (github.com/optimumbox/RDRVR, single-pass-stereo branch; MIT, Disonomouss / optimumbox): the same rules and the same
// output text, line for line, so that its result can be checked byte for byte against the Python chain's
// (research/sps/tools/vi_chain.py) over the game's whole corpus. Each Python regex is matched here by hand, with its
// semantics kept (anchors, greedy runs, first or every match, the order the checks refuse in); the function names follow
// the Python's so the two read side by side.
//
// Refuse by default: past the Python's own refusals, this port refuses (reason "strict: ...") a shader whose text it
// does not fully understand, where the Python would pass it over silently: a resource handle or a cbuffer read not in
// dumpbin's canonical form, a cbuffer read through a handle not made by createHandle, a cbuffer range whose metadata is
// not understood or whose id another resource shares, a cbuffer read with no layout to classify it by, more than one
// function. These apply to the shaders the Python would rewrite and to those it would use as they are (safe, shared).
#include "vi_rewrite.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vi {
namespace {
using std::string;
using Lines = std::vector<string>;
using Row4 = std::vector<string>;  // the four component names of a row's load

struct Refused {
    string why;
};
[[noreturn]] void refuse(string why) { throw Refused{std::move(why)}; }
[[noreturn]] void strict(const string& why) { throw Refused{"strict: " + why}; }

bool dig(char c) { return c >= '0' && c <= '9'; }
bool wrd(char c) { return dig(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }  // \w (ASCII)
bool spc(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }  // \s
bool wrd_dollar(char c) { return wrd(c) || c == '$'; }                                                  // [\w$]
bool wrd_dollar_dot(char c) { return wrd(c) || c == '$' || c == '.'; }                                  // [\w$.]
bool tok_char(char c) { return wrd(c) || c == '%' || c == '.'; }                                        // [%\w.]
bool non_space(char c) { return !spc(c); }                                                              // \S

Lines split(const string& t) {  // str.split('\n')
    Lines out;
    size_t a = 0;
    for (;;) {
        const size_t b = t.find('\n', a);
        if (b == string::npos) {
            out.push_back(t.substr(a));
            return out;
        }
        out.push_back(t.substr(a, b - a));
        a = b + 1;
    }
}
string join(const Lines& l) {  // '\n'.join
    size_t n = 0;
    for (const auto& s : l) n += s.size() + 1;
    string out;
    out.reserve(n);
    for (size_t i = 0; i < l.size(); ++i) {
        if (i) out += '\n';
        out += l[i];
    }
    return out;
}
bool starts(const string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
string I(long long v) { return std::to_string(v); }
long long to_int(const string& digits) {  // (digits only, as the callers check; rows and offsets are small)
    if (digits.empty() || digits.size() > 15) strict("a number not understood: " + digits);
    return std::stoll(digits);
}

// one string scanned left to right; each step consumes on success (a copy backtracks)
struct Sc {
    const string* s;
    size_t p;
    explicit Sc(const string& str, size_t pos = 0) : s(&str), p(pos) {}
    char at() const { return p < s->size() ? (*s)[p] : '\0'; }
    bool end() const { return p == s->size(); }
    bool lit(const char* x) {
        const size_t n = std::strlen(x);
        if (s->compare(p, n, x) != 0) return false;
        p += n;
        return true;
    }
    bool lit(const string& x) {
        if (s->compare(p, x.size(), x) != 0) return false;
        p += x.size();
        return true;
    }
    bool run(bool (*ok)(char), string* out = nullptr) {  // one or more, greedy
        size_t q = p;
        while (q < s->size() && ok((*s)[q])) ++q;
        if (q == p) return false;
        if (out) *out = s->substr(p, q - p);
        p = q;
        return true;
    }
    bool num(string* out = nullptr) { return run(dig, out); }  // \d+
    bool ws1() { return run(spc); }                            // \s+
    bool pnum(string* out = nullptr) {                         // %\d+
        if (at() != '%') return false;
        Sc c(*s, p + 1);
        if (!c.num()) return false;
        if (out) *out = s->substr(p, c.p - p);
        p = c.p;
        return true;
    }
};

// the "(%\d+)" just before position `at` (an unanchored "(%\d+) = ..." match starts there), or false
bool pct_before(const string& s, size_t at, string* h, size_t* start) {
    size_t q = at;
    while (q > 0 && dig(s[q - 1])) --q;
    if (q == at || q == 0 || s[q - 1] != '%') return false;
    *h = s.substr(q - 1, at - q + 1);
    *start = q - 1;
    return true;
}

// re.sub(r'%\d+(?![\w.])', alias) over one line
template <class F> string sub_tokens(const string& s, F&& lookup) {
    string out;
    out.reserve(s.size() + 16);
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '%') {
            size_t j = i + 1;
            while (j < s.size() && dig(s[j])) ++j;
            if (j > i + 1 && (j == s.size() || !(wrd(s[j]) || s[j] == '.'))) {
                const string tok = s.substr(i, j - i);
                const string* r = lookup(tok);
                out += r ? *r : tok;
                i = j;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}
// any token of %\d+(?![\w.]) in s that `has` accepts
template <class F> bool any_token(const string& s, F&& has) {
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '%') {
            size_t j = i + 1;
            while (j < s.size() && dig(s[j])) ++j;
            if (j > i + 1 && (j == s.size() || !(wrd(s[j]) || s[j] == '.'))) {
                if (has(s.substr(i, j - i))) return true;
                i = j;
                continue;
            }
        }
        ++i;
    }
    return false;
}

// ---- the IR's line forms (each Python regex, by hand) ----

// ^!\d+ = !\{i32 (\d+), %"?(?:hostlayout\.)?([^"*]+)"?\* undef, !"[^"]*", i32 \d+, i32 \d+, i32 \d+, i32 \d+, null\}
bool m_res(const string& ln, string* id, string* name) {
    Sc c(ln);
    if (!c.lit("!") || !c.num() || !c.lit(" = !{i32 ") || !c.num(id) || !c.lit(", %")) return false;
    if (c.at() == '"') ++c.p;
    const size_t q0 = c.p;
    auto name_at = [&](size_t q, string* out) {  // [^"*]+ (greedy: up to the first '"' or '*'), its end
        size_t e = q;
        while (e < ln.size() && ln[e] != '"' && ln[e] != '*') ++e;
        if (e == q || e == ln.size()) return string::npos;
        *out = ln.substr(q, e - q);
        return e;
    };
    size_t e = string::npos;
    if (ln.compare(q0, 11, "hostlayout.") == 0) e = name_at(q0 + 11, name);
    if (e == string::npos) e = name_at(q0, name);
    if (e == string::npos) return false;
    c.p = e;
    if (c.at() == '"') ++c.p;
    if (!c.lit("* undef, !\"")) return false;
    while (c.p < ln.size() && ln[c.p] != '"') ++c.p;
    if (!c.lit("\"")) return false;
    for (int k = 0; k < 4; ++k)
        if (!c.lit(", i32 ") || !c.num()) return false;
    return c.lit(", null}");
}
// ^!\d+ = !\{i32 (\d+), \[0 x %"class\.Texture2D<[^"]*>"\]\* undef, !"[^"]*", i32 \d+, i32 0, i32 -1, i32 2,
bool m_srv(const string& ln, string* id) {
    Sc c(ln);
    if (!c.lit("!") || !c.num() || !c.lit(" = !{i32 ") || !c.num(id) || !c.lit(", [0 x %\"class.Texture2D<")) return false;
    const size_t q = ln.find('"', c.p);
    if (q == string::npos || q == c.p || ln[q - 1] != '>') return false;  // <[^"]*>" : the '>' right before the first '"'
    c.p = q;
    if (!c.lit("\"]* undef, !\"")) return false;
    while (c.p < ln.size() && ln[c.p] != '"') ++c.p;
    return c.lit("\", i32 ") && c.num() && c.lit(", i32 0, i32 -1, i32 2,");
}
// (%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 (\d+),   (unanchored; every match)
// tail: whether ", i32 (\S+), i1" follows the range id (patch()'s form)
struct H2 {
    string h, rid;
    bool tail;
};
const char* const kH2 = " = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 ";
void m_h2(const string& ln, std::vector<H2>* out) {
    size_t from = 0, last_end = 0;
    for (;;) {
        const size_t k = ln.find(kH2, from);
        if (k == string::npos) return;
        from = k + 1;
        string h;
        size_t st = 0;
        if (!pct_before(ln, k, &h, &st) || st < last_end) continue;
        Sc c(ln, k + std::strlen(kH2));
        string rid;
        if (!c.num(&rid) || c.at() != ',') continue;
        Sc t = c;
        bool tail = false;
        if (t.lit(", i32 ")) {  // (\S+), i1: the non-space run ends in ',' and ", i1" follows from there
            size_t e = t.p;
            while (e < ln.size() && !spc(ln[e])) ++e;
            tail = e - t.p >= 2 && ln[e - 1] == ',' && ln.compare(e - 1, 4, ", i1") == 0;
        }
        out->push_back({h, rid, tail});
        last_end = c.p + 1;
        from = last_end;
    }
}
// @dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, %dx\.types\.Handle (%\d+), i32 ([%\w.]+)\)   (unanchored)
struct CB {
    string h, row;
    size_t rs, re;  // the row token's span
};
void m_cb(const string& ln, std::vector<CB>* out, bool first_only = false) {
    size_t from = 0;
    for (;;) {
        const size_t k = ln.find("@dx.op.cbufferLoadLegacy.", from);
        if (k == string::npos) return;
        from = k + 1;
        Sc c(ln, k + 25);
        string h, row;
        if (!c.run(wrd) || !c.lit("(i32 59, %dx.types.Handle ") || !c.pnum(&h) || !c.lit(", i32 ")) continue;
        const size_t rs = c.p;
        if (!c.run(tok_char, &row) || !c.lit(")")) continue;
        out->push_back({h, row, rs, rs + row.size()});
        if (first_only) return;
        from = c.p;
    }
}
// ^  (%\d+) = call %dx\.types\.CBufRet\.f32 @dx\.op\.cbufferLoadLegacy\.f32\(i32 59, %dx\.types\.Handle (%\d+), i32 X\)
// X: (\d+) (F32_LOAD) or, any_tok, ([%\w.]+) (patch_sky_prev's and patch_prev_world's)
bool m_f32(const string& ln, string* L, string* h, string* row, bool any_tok = false) {
    Sc c(ln);
    if (!c.lit("  ") || !c.pnum(L) ||
        !c.lit(" = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle ") || !c.pnum(h) ||
        !c.lit(", i32 "))
        return false;
    if (!(any_tok ? c.run(tok_char, row) : c.num(row))) return false;
    return c.lit(")");
}
// (%\d+) = call %dx\.types\.CBufRet\.i32 @dx\.op\.cbufferLoadLegacy\.i32\(i32 59, %dx\.types\.Handle (%\d+), i32 (\d+)\)
// (unanchored)
struct I32 {
    string L, h, row;
};
void m_i32(const string& ln, std::vector<I32>* out) {
    static const char* const k32 = " = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle ";
    size_t from = 0, last_end = 0;
    for (;;) {
        const size_t k = ln.find(k32, from);
        if (k == string::npos) return;
        from = k + 1;
        string L, h, row;
        size_t st = 0;
        if (!pct_before(ln, k, &L, &st) || st < last_end) continue;
        Sc c(ln, k + std::strlen(k32));
        if (!c.pnum(&h) || !c.lit(", i32 ") || !c.num(&row) || !c.lit(")")) continue;
        out->push_back({L, h, row});
        last_end = from = c.p;
    }
}
// (%\d+) = extractvalue %dx\.types\.CBufRet\.i32 (%\d+), (\d)   (unanchored; anchored: with "^  " before)
struct Ext {
    string v, L;
    int comp;
};
void m_ext_i32(const string& ln, std::vector<Ext>* out) {
    static const char* const ke = " = extractvalue %dx.types.CBufRet.i32 ";
    size_t from = 0, last_end = 0;
    for (;;) {
        const size_t k = ln.find(ke, from);
        if (k == string::npos) return;
        from = k + 1;
        string v, L;
        size_t st = 0;
        if (!pct_before(ln, k, &v, &st) || st < last_end) continue;
        Sc c(ln, k + std::strlen(ke));
        if (!c.pnum(&L) || !c.lit(", ") || !dig(c.at())) continue;
        out->push_back({v, L, c.at() - '0'});
        last_end = from = c.p + 1;
    }
}
bool m_ext_i32_anchored(const string& ln, Ext* e) {
    Sc c(ln);
    if (!c.lit("  ") || !c.pnum(&e->v) || !c.lit(" = extractvalue %dx.types.CBufRet.i32 ") || !c.pnum(&e->L) ||
        !c.lit(", ") || !dig(c.at()))
        return false;
    e->comp = c.at() - '0';
    return true;
}
// ^  (%\d+) = extractvalue %dx\.types\.CBufRet\.f32 (%\d+), (\d)$   on ln.split('  ;')[0].rstrip()
bool m_ext_f32_line(const string& raw, string* v, string* L, int* comp) {
    string ln = raw.substr(0, raw.find("  ;"));
    while (!ln.empty() && spc(ln.back())) ln.pop_back();
    Sc c(ln);
    if (!c.lit("  ") || !c.pnum(v) || !c.lit(" = extractvalue %dx.types.CBufRet.f32 ") || !c.pnum(L) || !c.lit(", ") ||
        !dig(c.at()))
        return false;
    *comp = c.at() - '0';
    ++c.p;
    return c.end();
}
// ^  (%\d+) = add i32 (%\d+), 0$
bool m_add0(const string& ln, string* a, string* b) {
    Sc c(ln);
    return c.lit("  ") && c.pnum(a) && c.lit(" = add i32 ") && c.pnum(b) && c.lit(", 0") && c.end();
}
// ^  (%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 0, i32 (\d+), i32 (%\d+), i1
bool m_h0(const string& ln, string* h, string* rid, string* idx) {
    Sc c(ln);
    return c.lit("  ") && c.pnum(h) && c.lit(" = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 ") &&
           c.num(rid) && c.lit(", i32 ") && c.pnum(idx) && c.lit(", i1");
}
// ^  (%\d+) =   (its end)
bool m_def(const string& ln, string* v, size_t* end) {
    Sc c(ln);
    if (!c.lit("  ") || !c.pnum(v) || !c.lit(" = ")) return false;
    *end = c.p;
    return true;
}

// ---- the Python's tables ----
struct Block {
    const char* name;
    int rows_lo1, rows_hi1, rows_lo2, rows_hi2;  // the rows that differ per eye: [lo1, hi1) and [lo2, hi2)
    int shift;
};
const Block kViewBlocks[] = {{"rage_matrices", 4, 8, 12, 16, 13}, {"rage_prev_proj", 0, 4, 0, 0, 4}};
const Block* view_block(const string& n) {
    for (const auto& b : kViewBlocks)
        if (n == b.name) return &b;
    return nullptr;
}
bool in_rows(const Block& b, long long r) { return (r >= b.rows_lo1 && r < b.rows_hi1) || (r >= b.rows_lo2 && r < b.rows_hi2); }
const int kWvpLo = 8, kWvpHi = 12, kWv2Row = 17, kCam2Row = 28, kInv2Row = 29;
const std::set<string> kInverseVars = {"WorldViewProjInverse", "WorldViewProjInverseTrees", "InverseViewProj"};
const std::set<string> kSharedBlocks = {"rage_shadowmap_common"};
const std::set<string> kViewNameOk = {"gViewport", "gBiasToCamera", "gCameraDistanceAtMaxDisplacement", "gNearCameraFade"};
const char* const kPrevWorld = "rage_prev_world";
const char* const kPrevInstanced = "rage_instancemtxprev";
const char* const kF32LoadDecl = "declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32)";
const std::set<int> kScreenSlots = {11, 12, 13, 15};
const std::map<int, std::set<string>> kScreenSlotNames = {{11, {"LightBufferSampler"}},
                                                          {12, {"GlobalWaterSampler0"}},
                                                          {13, {"ShadowCollectorSampler"}},
                                                          {15, {"DepthMapTexSampler", "RMPTFX_DepthMapTexSampler"}}};
const int kEye2SlotBase = 14960, kDepthBufferSlot = 16, kDepthIndexRow = 30, kResolveSlot = 15, kResolveIndexRow = 27;
const int kScreenIndexFields[3][3] = {{25, 0, 17}, {25, 14, 18}, {26, 0, 19}};
const int kDeltaRow = 21, kViewIdOpcode = 138;
const unsigned long long kViewIdFlag = 1ull << 21;

// VIEW_NAME = re.compile(r'View(?!port)|Camera|Proj|Eye|FOV', re.I)
bool view_name(const string& n) {
    string s(n);
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    for (size_t k = s.find("view"); k != string::npos; k = s.find("view", k + 1))
        if (s.compare(k + 4, 4, "port") != 0) return true;
    return s.find("camera") != string::npos || s.find("proj") != string::npos || s.find("eye") != string::npos ||
           s.find("fov") != string::npos;
}

// ---- the text's tables ----
struct Res {
    string id, name;
};
std::vector<Res> resources(const Lines& L) {
    std::vector<Res> out;
    for (const auto& ln : L) {
        string id, name;
        if (m_res(ln, &id, &name)) out.push_back({id, name});
    }
    return out;
}
std::map<string, string> range_ids(const Lines& L) {  // {block type name: CBV range id} (the last wins)
    std::map<string, string> out;
    for (const auto& r : resources(L)) out[r.name] = r.id;
    return out;
}
std::vector<H2> handles2(const Lines& L) {
    std::vector<H2> out;
    for (const auto& ln : L)
        if (ln.find(kH2) != string::npos) m_h2(ln, &out);
    return out;
}
std::vector<string> cb_handles(const Lines& L, const string& rid) {  // sorted by number
    std::map<long long, string> s;
    for (const auto& h : handles2(L))
        if (h.rid == rid) s[to_int(h.h.substr(1))] = h.h;
    std::vector<string> out;
    for (auto& kv : s) out.push_back(kv.second);
    return out;
}
bool has(const std::vector<string>& v, const string& x) {
    for (const auto& s : v)
        if (s == x) return true;
    return false;
}

struct LRow {
    long long a, b;
    string name;
};
// layouts(): {block type name: [(first row, last row, variable name)]} from dumpbin's buffer definitions
// v: ^;\s+(?:row_major |column_major )?[\w$.]+\s+([\w$]+)(\[\d+\])?;\s+; Offset:\s+(\d+)
bool m_layout_var(const string& ln, string* name, long long* off) {
    Sc c(ln);
    if (!c.lit(";") || !c.ws1()) return false;
    const size_t p0 = c.p;
    for (int attempt = 0; attempt < 2; ++attempt) {
        Sc d(ln, p0);
        if (attempt == 0 && !(d.lit("row_major ") || d.lit("column_major "))) continue;
        string n, o;
        if (!d.run(wrd_dollar_dot) || !d.ws1() || !d.run(wrd_dollar, &n)) continue;
        Sc b = d;
        if (b.lit("[") && b.num() && b.lit("]") && b.lit(";"))
            d = b;
        else if (!d.lit(";"))
            continue;
        if (!d.ws1() || !d.lit("; Offset:") || !d.ws1() || !d.num(&o)) continue;
        *name = n;
        *off = to_int(o);
        return true;
    }
    return false;
}
// e: ^;\s+\} [\w$]+;\s+; Offset:\s+\d+ Size:\s+(\d+)
bool m_layout_end(const string& ln, long long* size) {
    Sc c(ln);
    string s;
    if (!c.lit(";") || !c.ws1() || !c.lit("} ") || !c.run(wrd_dollar) || !c.lit(";") || !c.ws1() || !c.lit("; Offset:") ||
        !c.ws1() || !c.num() || !c.lit(" Size:") || !c.ws1() || !c.num(&s))
        return false;
    *size = to_int(s);
    return true;
}
std::map<string, std::vector<LRow>> layouts(const Lines& L) {
    std::map<string, std::vector<LRow>> out;
    string cur;
    std::vector<std::pair<long long, string>> entries;
    for (const auto& ln : L) {
        if (starts(ln, "; cbuffer ") && ln.size() > 10) {  // ^; cbuffer (\S+)$
            bool ok = true;
            for (size_t k = 10; k < ln.size(); ++k)
                if (spc(ln[k])) ok = false;
            if (ok) {
                cur = ln.substr(10);
                entries.clear();
                continue;
            }
        }
        if (!cur.empty()) {
            string n;
            long long off = 0, size = 0;
            if (m_layout_var(ln, &n, &off)) entries.push_back({off, n});
            if (m_layout_end(ln, &size)) {
                std::vector<LRow> rows;
                for (size_t i = 0; i < entries.size(); ++i) {
                    const long long o = entries[i].first;
                    const long long e = i + 1 < entries.size() ? entries[i + 1].first - 1 : size - 1;
                    rows.push_back({o / 16, (o > e ? o : e) / 16, entries[i].second});
                }
                out[cur] = rows;
                cur.clear();
            }
        }
    }
    return out;
}
// a section: from the line `open` (whole) to the first later line `close` accepts: re's (.*?) group ("\n" + the lines
// between, each with its "\n"), or false
template <class F> bool section(const Lines& L, const char* open, F&& close, string* out, size_t* close_line = nullptr) {
    for (size_t i = 0; i < L.size(); ++i) {
        if (L[i] != open) continue;
        for (size_t j = i + 1; j < L.size(); ++j)
            if (close(L[j])) {
                string s = "\n";
                for (size_t k = i + 1; k < j; ++k) s += L[k] + "\n";
                *out = s;
                if (close_line) *close_line = j;
                return true;
            }
    }
    return false;
}
// ^;\s+\} <name>;
bool m_close(const string& ln, const char* name) {
    Sc c(ln);
    return c.lit(";") && c.ws1() && c.lit("} ") && c.lit(name) && c.lit(";");
}
// {global_textures slot: the shader's name for it}: findall(r'uint (\w+);\s+; Offset:\s+(\d+)') in the section
std::map<long long, string> slot_names(const Lines& L) {
    std::map<long long, string> out;
    string s;
    if (!section(L, "; cbuffer global_textures", [](const string& ln) { return m_close(ln, "global_textures"); }, &s))
        return out;
    size_t from = 0;
    for (;;) {
        const size_t k = s.find("uint ", from);
        if (k == string::npos) break;
        Sc c(s, k + 5);
        string n, o;
        if (c.run(wrd, &n) && c.lit(";") && c.ws1() && c.lit("; Offset:") && c.ws1() && c.num(&o)) {
            out[to_int(o) / 4] = n;
            from = c.p;
        } else {
            from = k + 1;
        }
    }
    return out;
}

// ---- the rewrite (viewid_patch.py's functions, in its order) ----
struct Found {
    bool wvp = false;
    std::vector<std::pair<string, std::vector<long long>>> inverse;  // (block, first rows of its INVERSE_VARS read)
    std::vector<long long>& inv(const string& b) {
        for (auto& kv : inverse)
            if (kv.first == b) return kv.second;
        inverse.push_back({b, {}});
        return inverse.back().second;
    }
};

bool check_reads(const Lines& L, Found* found) {
    const auto lay = layouts(L);
    std::map<string, string> ranges;
    for (const auto& r : resources(L)) ranges[r.id] = r.name;
    std::map<string, string> handles;
    for (const auto& h : handles2(L)) {
        auto it = ranges.find(h.rid);
        handles[h.h] = it == ranges.end() ? "?" : it->second;
    }
    bool prev_world = false;
    std::vector<CB> loads;
    for (const auto& ln : L)
        if (ln.find("@dx.op.cbufferLoadLegacy.") != string::npos) m_cb(ln, &loads);
    for (const auto& m : loads) {
        auto hit = handles.find(m.h);
        const string block = hit == handles.end() ? "?" : hit->second;
        const bool dyn = !m.row.empty() && m.row[0] == '%';
        auto row = [&]() {
            for (char ch : m.row)
                if (!dig(ch)) strict("a row index not understood: " + m.row);  // (the Python: int() raises)
            return to_int(m.row);
        };
        if (view_block(block)) {
            if (block == "rage_matrices" && !dyn) {
                const long long r = row();
                if (r >= kWvpLo && r < kWvpHi) found->wvp = true;
            }
            continue;
        }
        if (kSharedBlocks.count(block)) continue;
        if (block == kPrevInstanced) {
            prev_world = true;
            continue;
        }
        auto lit = lay.find(block);
        if (lit == lay.end()) continue;
        std::vector<std::pair<long long, string>> names;
        for (const auto& e : lit->second) {
            if (dyn) {
                names.push_back({e.a, e.name});
            } else {
                const long long r = row();
                if (e.a <= r && r <= e.b) names.push_back({e.a, e.name});
            }
        }
        for (const auto& an : names) {
            const string& n = an.second;
            if (kInverseVars.count(n)) {
                if (dyn) refuse("dynamic row index into " + block + "." + n);
                auto& v = found->inv(block);
                bool seen = false;
                for (long long x : v) seen = seen || x == an.first;
                if (!seen) v.push_back(an.first);
                continue;
            }
            if (view_name(n) && !kViewNameOk.count(n)) {
                if (block == kPrevWorld && !dyn) {
                    const long long r = row();
                    if (r >= 0 && r < 4) {
                        prev_world = true;
                        continue;
                    }
                }
                refuse("reads " + block + "." + n + " (camera-dependent, no eye-2 copy)");
            }
        }
    }
    return prev_world;
}

struct Screen {  // a screen slot's handle: its index value and slot
    string idx;
    int slot;
};
std::map<string, Screen> screen_slot_reads(const Lines& L) {
    string rid;
    for (const auto& r : resources(L))
        if (r.name == "global_textures") rid = r.id;
    if (rid.empty()) return {};
    std::set<string> hs;
    for (const auto& h : handles2(L))
        if (h.rid == rid) hs.insert(h.h);
    std::map<string, long long> loads;
    std::vector<I32> i32s;
    for (const auto& ln : L)
        if (ln.find("@dx.op.cbufferLoadLegacy.i32(") != string::npos) m_i32(ln, &i32s);
    for (const auto& m : i32s)
        if (hs.count(m.h)) loads[m.L] = to_int(m.row);
    const auto names = slot_names(L);
    std::map<string, int> slot_of;
    std::vector<Ext> exts;
    for (const auto& ln : L)
        if (ln.find(" = extractvalue %dx.types.CBufRet.i32 ") != string::npos) m_ext_i32(ln, &exts);
    for (const auto& m : exts) {
        auto it = loads.find(m.L);
        if (it == loads.end()) continue;
        const long long slot = it->second * 4 + m.comp;
        if (!kScreenSlots.count(static_cast<int>(slot))) continue;
        auto nm = names.find(slot);
        if (nm != names.end() && kScreenSlotNames.at(static_cast<int>(slot)).count(nm->second))
            slot_of[m.v] = static_cast<int>(slot);
    }
    if (slot_of.empty()) return {};
    for (const auto& ln : L) {
        string a, b;
        if (m_add0(ln, &a, &b) && slot_of.count(b)) slot_of[a] = slot_of[b];
    }
    std::map<string, Screen> out;
    for (const auto& ln : L) {
        string a, b, h, rid0, idx, v;
        size_t e = 0;
        if (m_add0(ln, &a, &b) && slot_of.count(b)) continue;
        if (m_h0(ln, &h, &rid0, &idx) && slot_of.count(idx)) {
            out[h] = {idx, slot_of[idx]};
            continue;
        }
        if (m_def(ln, &v, &e)) {
            const string rest = ln.substr(e);
            const string head = rest.substr(0, rest.find("  ;"));
            if (any_token(head, [&](const string& t) { return slot_of.count(t) != 0; }))
                refuse("a screen slot's index used other than as a texture handle's index");
        }
    }
    return out;
}

void replace_once(string* s, const string& from, const string& to) {
    const size_t k = s->find(from);
    if (k != string::npos) s->replace(k, from.size(), to);
}

Lines patch_screen_slots(const Lines& lines, const std::map<string, Screen>& screen, int* n) {
    *n = 0;
    if (screen.empty()) return lines;
    Lines out;
    for (string ln : lines) {
        string h, rid, idx;
        if (m_h0(ln, &h, &rid, &idx)) {
            auto it = screen.find(h);
            if (it != screen.end()) {
                ++*n;
                const string t = "%scr" + I(*n) + ".sp";
                out.push_back("  " + t + ".v1 = icmp ne i32 %vid.sp, 0");
                out.push_back("  " + t + " = select i1 " + t + ".v1, i32 " + I(kEye2SlotBase + it->second.slot) + ", i32 " +
                              it->second.idx);
                replace_once(&ln, ", i32 " + it->second.idx + ", i1", ", i32 " + t + ", i1");
            }
        }
        out.push_back(ln);
    }
    return out;
}

Row4 emit_row(Lines& out, const string& t, const string& h, long long row, bool past_end = false, const char* kind = "f32") {
    string idx = I(row);
    if (past_end) {
        out.push_back("  " + t + ".i = select i1 %isv1.sp, i32 " + I(row) + ", i32 0");
        idx = t + ".i";
    }
    out.push_back("  " + t + " = call %dx.types.CBufRet." + kind + " @dx.op.cbufferLoadLegacy." + kind +
                  "(i32 59, %dx.types.Handle " + h + ", i32 " + idx + ")");
    Row4 r;
    for (int k = 0; k < 4; ++k) {
        out.push_back("  " + t + ".c" + I(k) + " = extractvalue %dx.types.CBufRet." + kind + " " + t + ", " + I(k));
        r.push_back(t + ".c" + I(k));
    }
    return r;
}
// v x M: v's empty entries are zero
Row4 emit_vec_mat(Lines& out, const string& t, const Row4& v, const std::vector<Row4>& rows) {
    Row4 res;
    for (int k = 0; k < 4; ++k) {
        string acc;
        for (int j = 0; j < 4; ++j) {
            if (v[j].empty()) continue;
            const string m = t + ".m" + I(j) + "c" + I(k);
            out.push_back("  " + m + " = fmul float " + v[j] + ", " + rows[j][k]);
            if (acc.empty()) {
                acc = m;
            } else {
                const string a = t + ".a" + I(j) + "c" + I(k);
                out.push_back("  " + a + " = fadd float " + acc + ", " + m);
                acc = a;
            }
        }
        if (acc.empty()) strict("an empty product");  // (the Python would print None)
        res.push_back(acc);
    }
    return res;
}
void select_rows(Lines& out, const string& t, const string& L, const Row4& r) {
    for (int k = 0; k < 4; ++k) {
        out.push_back("  " + t + ".e" + I(k) + " = extractvalue %dx.types.CBufRet.f32 " + L + ", " + I(k));
        out.push_back("  " + t + ".s" + I(k) + " = select i1 %isv1.sp, float " + r[k] + ", float " + t + ".e" + I(k));
    }
}
// the original component reads of each renamed load now read t.s0..s3 (the reads stay, unused)
Lines alias_lines(const Lines& out, const std::map<string, string>& rename) {
    std::map<string, string> alias;
    std::vector<bool> defs(out.size(), false);
    for (size_t i = 0; i < out.size(); ++i) {
        string v, L;
        int comp = 0;
        if (m_ext_f32_line(out[i], &v, &L, &comp)) {
            auto it = rename.find(L);
            if (it != rename.end()) {
                alias[v] = it->second + ".s" + I(comp);
                defs[i] = true;
            }
        }
    }
    Lines res;
    res.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i)
        res.push_back(defs[i] ? out[i] : sub_tokens(out[i], [&](const string& t) -> const string* {
            auto it = alias.find(t);
            return it == alias.end() ? nullptr : &it->second;
        }));
    return res;
}
string alias_loads(const Lines& out, const std::map<string, string>& rename) { return join(alias_lines(out, rename)); }

string need_id(const std::map<string, string>& rid, const string& name) {
    auto it = rid.find(name);
    if (it == rid.end()) strict("no range for " + name);  // (the Python: KeyError)
    return it->second;
}
string first_handle(const Lines& L, const string& rid) {
    auto hs = cb_handles(L, rid);
    if (hs.empty()) strict("no handle for range " + rid);  // (the Python: IndexError)
    return hs[0];
}

string patch_wvp(const string& text, int* n) {
    const Lines L = split(text);
    const auto hs = cb_handles(L, need_id(range_ids(L), "rage_matrices"));
    if (hs.empty()) strict("no rage_matrices handle");
    const string hm = hs[0];
    Lines out;
    std::map<string, string> rename;
    *n = 0;
    for (const auto& ln : L) {
        string Ld, h, row;
        const bool m = m_f32(ln, &Ld, &h, &row);
        out.push_back(ln);
        if (!m || !has(hs, h)) continue;
        const long long r = to_int(row);
        if (r < kWvpLo || r >= kWvpHi) continue;
        ++*n;
        const string t = "%wvp" + I(*n) + ".sp";
        const Row4 w = emit_row(out, t + ".w", hm, r - kWvpLo);
        const Row4 c2 = emit_row(out, t + ".cam", hm, kCam2Row, true);
        std::vector<Row4> wv2;
        for (int j = 0; j < 4; ++j) wv2.push_back(emit_row(out, t + ".wv" + I(j), hm, kWv2Row + j, true));
        Row4 v;
        for (int k = 0; k < 3; ++k) {
            out.push_back("  " + t + ".o" + I(k) + " = fmul float " + w[3] + ", " + c2[k]);
            out.push_back("  " + t + ".v" + I(k) + " = fsub float " + w[k] + ", " + t + ".o" + I(k));
            v.push_back(t + ".v" + I(k));
        }
        v.push_back(w[3]);
        select_rows(out, t, Ld, emit_vec_mat(out, t + ".x", v, wv2));
        rename[Ld] = t;
    }
    return *n ? alias_loads(out, rename) : text;
}

string patch_inverse(const string& text, const Found& found, int* n) {
    const Lines L = split(text);
    const auto rid = range_ids(L);
    if (!rid.count("rage_matrices")) refuse("reads an inverse view-projection without rage_matrices");
    const string hm = first_handle(L, rid.at("rage_matrices"));
    std::map<string, const std::vector<long long>*> firsts;
    for (const auto& bf : found.inverse)
        for (const auto& h : cb_handles(L, need_id(rid, bf.first))) firsts[h] = &bf.second;
    Lines out;
    std::map<string, string> rename;
    *n = 0;
    for (const auto& ln : L) {
        string Ld, h, rows;
        const bool m = m_f32(ln, &Ld, &h, &rows);
        out.push_back(ln);
        if (!m) continue;
        auto fit = firsts.find(h);
        if (fit == firsts.end()) continue;
        const long long row = to_int(rows);
        long long first = -1;
        for (long long f : *fit->second)
            if (f <= row && row < f + 4) {
                first = f;
                break;
            }
        if (first < 0) continue;
        ++*n;
        const string t = "%inv" + I(*n) + ".sp";
        const long long i = row - first;
        std::vector<Row4> x1, wv1;
        for (int j = 0; j < 4; ++j) x1.push_back(emit_row(out, t + ".x" + I(j), h, first + j));
        for (int j = 0; j < 4; ++j) wv1.push_back(emit_row(out, t + ".wv" + I(j), hm, 4 + j));
        Row4 v;
        if (i == 0 || i == 1) {
            const Row4 pr = emit_row(out, t + ".p", hm, kInv2Row + i, true);
            v = {pr[0], pr[1], pr[2], ""};
        } else {
            const Row4 pr = emit_row(out, t + ".p", hm, i == 2 ? kInv2Row : kInv2Row + 2, true);
            v = i == 2 ? Row4{"", "", "", pr[3]} : pr;
        }
        const Row4 q = emit_vec_mat(out, t + ".q", v, wv1);
        select_rows(out, t, Ld, emit_vec_mat(out, t + ".y", q, x1));
        rename[Ld] = t;
    }
    return *n ? alias_loads(out, rename) : text;
}

string first_srv(const Lines& L) {
    for (const auto& ln : L) {
        string id;
        if (m_srv(ln, &id)) return id;
    }
    return "";
}

string patch_index_remap(const string& text, const std::set<string>& done, int* n) {
    *n = 0;
    const Lines L = split(text);
    const auto rid = range_ids(L);
    const string srv = first_srv(L);
    const auto hm = rid.count("rage_matrices") ? cb_handles(L, rid.at("rage_matrices")) : std::vector<string>{};
    if (srv.empty() || hm.empty()) return text;
    Lines out;
    for (const auto& ln : L) {
        string h, r, idx;
        if (!m_h0(ln, &h, &r, &idx) || r != srv || done.count(h)) {
            out.push_back(ln);
            continue;
        }
        ++*n;
        const string t = "%rmp" + I(*n) + ".sp";
        const Row4 dr = emit_row(out, t + ".dr", hm[0], kDepthIndexRow, true);
        out.push_back("  " + t + ".di = bitcast float " + dr[3] + " to i32");
        out.push_back("  " + t + ".qd = icmp eq i32 " + idx + ", " + t + ".di");
        out.push_back("  " + t + ".bd = and i1 " + t + ".qd, %isv1.sp");
        out.push_back("  " + t + ".nd = select i1 " + t + ".bd, i32 " + I(kEye2SlotBase + kDepthBufferSlot) + ", i32 " + idx);
        const Row4 rr = emit_row(out, t + ".rr", hm[0], kResolveIndexRow, true);
        out.push_back("  " + t + ".ri = bitcast float " + rr[3] + " to i32");
        out.push_back("  " + t + ".qr = icmp eq i32 " + idx + ", " + t + ".ri");
        out.push_back("  " + t + ".br = and i1 " + t + ".qr, %isv1.sp");
        out.push_back("  " + t + ".nr = select i1 " + t + ".br, i32 " + I(kEye2SlotBase + kResolveSlot) + ", i32 " + t + ".nd");
        string cur = t + ".nr";
        std::map<int, string> rows;
        for (int k = 0; k < 3; ++k) {
            const int row = kScreenIndexFields[k][0], shift = kScreenIndexFields[k][1], slot = kScreenIndexFields[k][2];
            if (!rows.count(row)) rows[row] = emit_row(out, t + ".f" + I(row), hm[0], row, true, "i32")[3];
            out.push_back("  " + t + ".s" + I(k) + " = lshr i32 " + rows[row] + ", " + I(shift));
            out.push_back("  " + t + ".v" + I(k) + " = and i32 " + t + ".s" + I(k) + ", 16383");
            out.push_back("  " + t + ".q" + I(k) + " = icmp eq i32 " + idx + ", " + t + ".v" + I(k));
            out.push_back("  " + t + ".b" + I(k) + " = and i1 " + t + ".q" + I(k) + ", %isv1.sp");
            out.push_back("  " + t + ".n" + I(k) + " = select i1 " + t + ".b" + I(k) + ", i32 " + I(kEye2SlotBase + slot) +
                          ", i32 " + cur);
            cur = t + ".n" + I(k);
        }
        string l2 = ln;
        replace_once(&l2, ", i32 " + idx + ", i1", ", i32 " + cur + ", i1");
        out.push_back(l2);
    }
    return join(out);
}

// the velocity pass's $Globals (patch_ndc)
const std::pair<const char*, long long> kNdcGlobals[] = {{"NDCToPrevNDC", 128}, {"DepthMapSampler", 68}, {"TexelSize", 32}};
const long long kNdcSize = 228;
const int kNdcCols[3] = {0, 1, 3};
const int kNdcSpare[12][2] = {{15, 0}, {15, 1}, {15, 2}, {15, 3}, {7, 1}, {7, 2}, {7, 3}, {14, 1}, {14, 2}, {14, 3}, {12, 2}, {12, 3}};
const int kNdcDepthRow = 1;
const int kNdcDepthSlots[2] = {16, 15};

// ^;\s+\} \$Globals;\s+; Offset:\s+0 Size:\s+(\d+)
bool m_globals_end(const string& ln, long long* size) {
    Sc c(ln);
    string s;
    if (!c.lit(";") || !c.ws1() || !c.lit("} $Globals;") || !c.ws1() || !c.lit("; Offset:") || !c.ws1() || !c.lit("0 Size:") ||
        !c.ws1() || !c.num(&s))
        return false;
    *size = to_int(s);
    return true;
}
bool ndc_layout(const Lines& L) {
    string s;
    long long size = -1;
    if (!section(L, "; cbuffer $Globals", [&](const string& ln) { return m_globals_end(ln, &size); }, &s)) return false;
    if (size != kNdcSize) return false;
    std::map<string, long long> offs;  // findall(r' (\w+);\s+; Offset:\s+(\d+)')
    size_t from = 0;
    for (;;) {
        const size_t k = s.find(' ', from);
        if (k == string::npos) break;
        Sc c(s, k + 1);
        string n, o;
        if (c.run(wrd, &n) && c.lit(";") && c.ws1() && c.lit("; Offset:") && c.ws1() && c.num(&o)) {
            offs[n] = to_int(o);
            from = c.p;
        } else {
            from = k + 1;
        }
    }
    for (const auto& g : kNdcGlobals) {
        auto it = offs.find(g.first);
        if (it == offs.end() || it->second != g.second) return false;
    }
    return true;
}

string patch_ndc(const string& text, int* n_total) {
    *n_total = 0;
    const Lines L = split(text);
    if (!ndc_layout(L)) return text;
    const auto rid = range_ids(L);
    const string srv = first_srv(L);
    const auto hg = rid.count("$Globals") ? cb_handles(L, rid.at("$Globals")) : std::vector<string>{};
    if (hg.empty()) return text;
    const long long base = 128 / 16;
    Lines out;
    std::map<string, string> rename;
    int n = 0;
    for (const auto& ln : L) {
        string Ld, h, rows;
        const bool m = m_f32(ln, &Ld, &h, &rows);
        out.push_back(ln);
        if (!m || !has(hg, h)) continue;
        const long long r = to_int(rows);
        if (!(base <= r && r < base + 4)) continue;
        const long long j = r - base;
        ++n;
        const string t = "%ndc" + I(n) + ".sp";
        std::map<int, string> sel;
        for (int k = 0; k < 3; ++k) {
            const int row = kNdcSpare[j * 3 + k][0], comp = kNdcSpare[j * 3 + k][1];
            const Row4 v = emit_row(out, t + ".r" + I(k), h, row, row * 16 >= kNdcSize);
            sel[kNdcCols[k]] = v[comp];
        }
        for (int c = 0; c < 4; ++c) {
            out.push_back("  " + t + ".e" + I(c) + " = extractvalue %dx.types.CBufRet.f32 " + Ld + ", " + I(c));
            if (sel.count(c))
                out.push_back("  " + t + ".s" + I(c) + " = select i1 %isv1.sp, float " + sel[c] + ", float " + t + ".e" + I(c));
            else
                out.push_back("  " + t + ".s" + I(c) + " = select i1 %isv1.sp, float 0.000000e+00, float " + t + ".e" + I(c));
        }
        rename[Ld] = t;
    }
    out = alias_lines(out, rename);
    int n_depth = 0;
    if (!srv.empty()) {
        Lines res;
        for (const auto& ln : out) {
            string h, r, idx;
            if (!m_h0(ln, &h, &r, &idx) || r != srv) {
                res.push_back(ln);
                continue;
            }
            ++n_depth;
            const string t = "%ndd" + I(n_depth) + ".sp";
            string cur = idx;
            const Row4 d = emit_row(res, t + ".d", hg[0], kNdcDepthRow, false, "i32");
            for (int s = 0; s < 2; ++s) {
                res.push_back("  " + t + ".v" + I(s) + " = sub i32 " + d[1 + s] + ", 1");
                res.push_back("  " + t + ".q" + I(s) + " = icmp eq i32 " + idx + ", " + t + ".v" + I(s));
                res.push_back("  " + t + ".b" + I(s) + " = and i1 " + t + ".q" + I(s) + ", %isv1.sp");
                res.push_back("  " + t + ".n" + I(s) + " = select i1 " + t + ".b" + I(s) + ", i32 " +
                              I(kEye2SlotBase + kNdcDepthSlots[s]) + ", i32 " + cur);
                cur = t + ".n" + I(s);
            }
            string l2 = ln;
            replace_once(&l2, ", i32 " + idx + ", i1", ", i32 " + cur + ", i1");
            res.push_back(l2);
        }
        out = res;
    }
    *n_total = n + n_depth;
    return join(out);
}

const std::pair<const char*, const char*> kGlobalsDepthPs[] = {
    {"PS_lightglow", "DepthBufferSampler"}, {"PSFootprintCollector", "DepthBufferSampler"}, {"PSFootprintColor", "DepthBufferSampler"}};

string patch_globals_depth(const string& text, int* n) {
    *n = 0;
    const Lines L = split(text);
    const string e = entry_name(text);
    const char* name = nullptr;
    for (const auto& g : kGlobalsDepthPs)
        if (!e.empty() && e == g.first) name = g.second;
    const auto rid = range_ids(L);
    if (!name || !rid.count("$Globals") || rid.count("rage_matrices")) return text;
    string s;
    const bool m = section(L, "; cbuffer $Globals", [](const string& ln) { return m_close(ln, "$Globals"); }, &s);
    long long off = -1;
    if (m) {  // re.search(' ' + name + r';\s+; Offset:\s+(\d+)', section)
        const string key = string(" ") + name + ";";
        for (size_t k = s.find(key); k != string::npos; k = s.find(key, k + 1)) {
            Sc c(s, k + key.size());
            string o;
            if (c.ws1() && c.lit("; Offset:") && c.ws1() && c.num(&o)) {
                off = to_int(o);
                break;
            }
        }
    }
    if (off < 0) refuse(string("no $Globals ") + name);
    const long long row = off / 16, comp = off % 16 / 4;
    const auto hgv = cb_handles(L, rid.at("$Globals"));
    const std::set<string> hg(hgv.begin(), hgv.end());
    std::set<string> loads, idx;
    std::vector<I32> i32s;
    for (const auto& ln : L)
        if (ln.find("@dx.op.cbufferLoadLegacy.i32(") != string::npos) m_i32(ln, &i32s);
    for (const auto& q : i32s)
        if (hg.count(q.h) && to_int(q.row) == row) loads.insert(q.L);
    for (const auto& ln : L) {
        Ext x;
        if (m_ext_i32_anchored(ln, &x) && loads.count(x.L) && x.comp == comp) idx.insert(x.v);
    }
    for (const auto& ln : L) {
        string a, b;
        if (m_add0(ln, &a, &b) && idx.count(b)) idx.insert(a);
    }
    Lines out;
    for (const auto& ln0 : L) {
        string ln = ln0, h, r, ix;
        if (m_h0(ln, &h, &r, &ix) && idx.count(ix)) {
            ++*n;
            const string t = "%gd" + I(*n) + ".sp";
            out.push_back("  " + t + " = select i1 %isv1.sp, i32 " + I(kEye2SlotBase + kDepthBufferSlot) + ", i32 " + ix);
            replace_once(&ln, ", i32 " + ix + ", i1", ", i32 " + t + ", i1");
        }
        out.push_back(ln);
    }
    if (!*n) refuse(string(name) + " not read as a texture index");
    return join(out);
}

// the sky's previous position (patch_sky_prev)
string patch_sky_prev(const string& text, int* n) {
    *n = 0;
    const Lines lines = split(text);
    const auto rid = range_ids(lines);
    if (!rid.count("rage_prev_proj") || !rid.count("rage_matrices") || rid.count(kPrevWorld) || rid.count(kPrevInstanced))
        return text;
    const auto hpp = cb_handles(lines, rid.at("rage_prev_proj"));
    const auto hm = cb_handles(lines, rid.at("rage_matrices"));
    if (hpp.empty() || hm.empty()) return text;
    std::map<string, string> base;  // ^  (%row\d+\.sp) = add i32 %row\d+\.sp\.k, (\d+)$
    for (const auto& ln : lines) {
        Sc c(ln);
        string a, d;
        if (!c.lit("  %row") || !c.num() || !c.lit(".sp = add i32 %row") || !c.num() || !c.lit(".sp.k, ") || !c.num(&d) || !c.end())
            continue;
        a = ln.substr(2, ln.find(" = ") - 2);
        base[a] = d;
    }
    auto based = [&](const string& tok) {
        auto it = base.find(tok);
        return it == base.end() ? tok : it->second;
    };
    string row0;
    for (const auto& ln : lines) {
        string Ld, h, row;
        if (m_f32(ln, &Ld, &h, &row, true) && has(hpp, h) && based(row) == "0") {
            row0 = Ld;
            break;
        }
    }
    if (row0.empty()) return text;
    string c0;
    for (const auto& ln : lines) {  // ^  (%\d+) = extractvalue %dx\.types\.CBufRet\.f32 <row0>, 0$
        Sc c(ln);
        string v;
        if (c.lit("  ") && c.pnum(&v) && c.lit(" = extractvalue %dx.types.CBufRet.f32 ") && c.lit(row0) && c.lit(", 0") && c.end()) {
            c0 = v;
            break;
        }
    }
    if (c0.empty()) return text;
    size_t xi = string::npos;
    string px;
    for (size_t i = 0; i < lines.size(); ++i) {  // ^  (%\d+) = fmul fast float (%\d+), <c0>$
        Sc c(lines[i]);
        string a, b;
        if (c.lit("  ") && c.pnum(&a) && c.lit(" = fmul fast float ") && c.pnum(&b) && c.lit(", ") && c.lit(c0) && c.end()) {
            xi = i;
            px = b;
            break;
        }
    }
    if (xi == string::npos) return text;
    if (xi + 2 >= lines.size()) strict("the sky's multiply runs off the text");  // (the Python: IndexError)
    // ^  (%\d+) = call float @dx\.op\.tertiary\.f32\(i32 46, float (%\d+), float (%\d+), float (%\d+)\)
    auto fm = [](const string& ln, string* a2) {
        Sc c(ln);
        return c.lit("  ") && c.pnum() && c.lit(" = call float @dx.op.tertiary.f32(i32 46, float ") && c.pnum(a2) &&
               c.lit(", float ") && c.pnum() && c.lit(", float ") && c.pnum() && c.lit(")");
    };
    string py, pz;
    if (!fm(lines[xi + 1], &py) || !fm(lines[xi + 2], &pz)) return text;
    // not when the previous position is already relative to the camera ("fsub %a, <rage_matrices row 15 .x>")
    std::map<string, string> defs;  // ^  (%\d+) = (.*)$
    for (const auto& ln : lines) {
        string v;
        size_t e = 0;
        if (m_def(ln, &v, &e)) defs[v] = ln.substr(e);
    }
    auto def = [&](const string& v) {
        auto it = defs.find(v);
        return it == defs.end() ? string() : it->second;
    };
    {
        const string dpx = def(px);  // fsub (?:fast )?float %\d+, (%\d+)$
        Sc c(dpx);
        string src_v;
        bool ext = c.lit("fsub ");
        if (ext) {
            c.lit("fast ");
            ext = c.lit("float ") && c.pnum() && c.lit(", ") && c.pnum(&src_v) && c.end();
        }
        if (ext) {
            const string dsrc = def(src_v);  // extractvalue %dx\.types\.CBufRet\.f32 (%\d+), 0$
            Sc s(dsrc);
            string ldv;
            if (s.lit("extractvalue %dx.types.CBufRet.f32 ") && s.pnum(&ldv) && s.lit(", 0") && s.end()) {
                string Ld, h, row;
                if (m_f32("  " + ldv + " = " + def(ldv), &Ld, &h, &row, true) && has(hm, h) && based(row) == "15") return text;
            }
        }
    }
    const size_t stop = xi + 16 < lines.size() ? xi + 16 : lines.size();
    const string t = "%sky.sp";
    Lines pre;
    const Row4 c1 = emit_row(pre, t + ".c1", hm[0], 15);
    pre.push_back("  " + t + ".c2.i = select i1 %isv1.sp, i32 " + I(kCam2Row) + ", i32 15");
    pre.push_back("  " + t + ".c2 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle " + hm[0] +
                  ", i32 " + t + ".c2.i)");
    Row4 c2;
    for (int k = 0; k < 4; ++k) {
        pre.push_back("  " + t + ".c2.c" + I(k) + " = extractvalue %dx.types.CBufRet.f32 " + t + ".c2, " + I(k));
        c2.push_back(t + ".c2.c" + I(k));
    }
    std::map<string, string> newv;
    const string pv[3] = {px, py, pz};
    for (int k = 0; k < 3; ++k) {
        pre.push_back("  " + t + ".d" + I(k) + " = fsub float " + c1[k] + ", " + c2[k]);
        pre.push_back("  " + t + ".p" + I(k) + " = fadd float " + pv[k] + ", " + t + ".d" + I(k));
        newv[pv[k]] = t + ".p" + I(k);
    }
    Lines out(lines.begin(), lines.begin() + xi);
    out.insert(out.end(), pre.begin(), pre.end());
    for (size_t i = xi; i < stop; ++i)
        out.push_back(sub_tokens(lines[i], [&](const string& tk) -> const string* {
            auto it = newv.find(tk);
            return it == newv.end() ? nullptr : &it->second;
        }));
    out.insert(out.end(), lines.begin() + stop, lines.end());
    *n = 1;
    return join(out);
}

string patch_prev_world(const string& text) {
    const Lines lines = split(text);
    const auto rid = range_ids(lines);
    if (!rid.count("rage_matrices") || (!rid.count(kPrevWorld) && !rid.count(kPrevInstanced)))
        refuse("reads rage_prev_world or rage_instancemtxprev without rage_matrices");
    auto handles_of = [&](const char* block) {
        std::set<string> s;
        auto it = rid.find(block);
        if (it == rid.end()) return s;
        for (const auto& h : handles2(lines))
            if (h.rid == it->second) s.insert(h.h);
        return s;
    };
    const auto hp = handles_of(kPrevWorld), hi = handles_of(kPrevInstanced);
    if (hp.empty() && hi.empty()) refuse("rage_prev_world / rage_instancemtxprev handle not found");
    const auto hms = cb_handles(lines, rid.at("rage_matrices"));
    if (hms.empty()) refuse("rage_matrices handle not found");
    const string hm = hms[0];
    Lines out;
    std::map<string, string> rename;
    int n = 0;
    for (const auto& ln : lines) {
        string Ld, h, row;
        const bool m = m_f32(ln, &Ld, &h, &row, true);
        out.push_back(ln);
        if (!m) continue;
        if (hp.count(h)) {
            if (row[0] == '%') continue;
            for (char ch : row)
                if (!dig(ch)) strict("a row index not understood: " + row);
            const long long r = to_int(row);
            if (r < 0 || r >= 4) continue;
        } else if (!hi.count(h)) {
            continue;
        }
        ++n;
        const string t = "%pw" + I(n) + ".sp";
        Row4 e;
        for (int k = 0; k < 4; ++k) e.push_back(t + ".e" + I(k));
        for (int k = 0; k < 4; ++k) out.push_back("  " + e[k] + " = extractvalue %dx.types.CBufRet.f32 " + Ld + ", " + I(k));
        for (int j = 0; j < 4; ++j) {
            out.push_back("  " + t + ".r" + I(j) + " = select i1 %isv1.sp, i32 " + I(kDeltaRow + j) + ", i32 " + I(j));
            out.push_back("  " + t + ".d" + I(j) + " = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle " +
                          hm + ", i32 " + t + ".r" + I(j) + ")");
            for (int k = 0; k < 4; ++k)
                out.push_back("  " + t + ".d" + I(j) + "c" + I(k) + " = extractvalue %dx.types.CBufRet.f32 " + t + ".d" + I(j) + ", " + I(k));
        }
        for (int k = 0; k < 4; ++k) {
            out.push_back("  " + t + ".m0c" + I(k) + " = fmul float " + e[0] + ", " + t + ".d0c" + I(k));
            for (int j = 1; j < 4; ++j) {
                out.push_back("  " + t + ".m" + I(j) + "c" + I(k) + " = fmul float " + e[j] + ", " + t + ".d" + I(j) + "c" + I(k));
                const string prev = j == 1 ? t + ".m0c" + I(k) : t + ".a" + I(j - 1) + "c" + I(k);
                out.push_back("  " + t + ".a" + I(j) + "c" + I(k) + " = fadd float " + prev + ", " + t + ".m" + I(j) + "c" + I(k));
            }
            out.push_back("  " + t + ".s" + I(k) + " = select i1 %isv1.sp, float " + t + ".a3c" + I(k) + ", float " + e[k]);
        }
        rename[Ld] = t;
    }
    if (!n) return text;
    return alias_loads(out, rename);
}

// ---- this port's own checks (refuse by default) ----
void strict_checks(const Lines& L) {
    int defines = 0;
    std::map<string, size_t> meta;  // "!N" -> its line
    for (size_t i = 0; i < L.size(); ++i) {
        const string& ln = L[i];
        if (starts(ln, "define ")) ++defines;
        if (!ln.empty() && ln[0] == '!') {
            Sc c(ln, 1);
            if (c.num() && c.lit(" = ")) meta[ln.substr(0, c.p - 3)] = i;
        }
    }
    if (defines != 1) strict("not one function");
    // the CBV ranges, from !dx.resources = !{!R}, !R = !{srv, uav, cbv, sampler}, cbv = !{!a, !b, ...}
    std::set<string> cbv_ids;
    auto meta_body = [&](const string& id) -> string {  // "!N = !{<body>}" -> body
        auto it = meta.find(id);
        if (it == meta.end()) strict("no metadata " + id);
        const string& ln = L[it->second];
        const size_t k = ln.find(" = !{");
        if (k == string::npos || ln.back() != '}') strict("metadata " + id + " not understood");
        return ln.substr(k + 5, ln.size() - k - 6);
    };
    auto list_of = [](const string& body) {  // "!a, !b, null" -> items
        std::vector<string> out;
        size_t a = 0;
        for (;;) {
            const size_t b = body.find(", ", a);
            out.push_back(body.substr(a, b == string::npos ? string::npos : b - a));
            if (b == string::npos) return out;
            a = b + 2;
        }
    };
    string res_id;
    for (const auto& ln : L)
        if (starts(ln, "!dx.resources = !{")) res_id = ln.substr(18, ln.size() - 19);
    std::set<string> res_line_ids;  // every resource entry the Python's range pattern reads, by range id
    if (!res_id.empty()) {
        const auto classes = list_of(meta_body(res_id));
        if (classes.size() != 4) strict("!dx.resources not understood");
        if (classes[2] != "null") {
            for (const auto& e : list_of(meta_body(classes[2]))) {
                string id, name;
                if (!m_res(L[meta.count(e) ? meta[e] : 0], &id, &name) || !meta.count(e))
                    strict("a cbuffer range's metadata not understood: " + e);
                if (!cbv_ids.insert(id).second) strict("two cbuffer ranges with id " + id);
            }
        }
    }
    {
        std::map<string, int> n_id;
        for (const auto& r : resources(L)) ++n_id[r.id];
        for (const auto& kv : n_id)
            if (kv.second > 1) strict("range id " + kv.first + " shared by two resources");
    }
    // every handle and cbuffer read in dumpbin's canonical form; cbuffer reads only through createHandle's CBV handles
    std::map<string, string> cb_handle_range;
    for (const auto& ln : L) {
        if (starts(ln, "declare ")) continue;
        if (ln.find("@dx.op.createHandle") != string::npos || ln.find("@dx.op.annotateHandle") != string::npos ||
            ln.find("@dx.op.createHandleFrom") != string::npos || ln.find("@dx.op.createHandleForLib") != string::npos) {
            Sc c(ln);
            string h, cls, rid, idx;
            const bool ok = c.lit("  ") && c.pnum(&h) && c.lit(" = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 ") &&
                            c.num(&cls) && c.lit(", i32 ") && c.num(&rid) && c.lit(", i32 ") && (c.num(&idx) || c.pnum(&idx)) &&
                            c.lit(", i1 ") && (c.lit("false") || c.lit("true")) && c.lit(")") && (c.end() || c.lit("  ;"));
            if (!ok) strict("a resource handle not understood: " + ln.substr(0, 120));
            if (cls == "2") {
                if (!cbv_ids.count(rid)) strict("a cbuffer handle of a range not understood: " + rid);
                if (idx[0] == '%') strict("a cbuffer handle with a dynamic index");
                cb_handle_range[h] = rid;
            }
        }
    }
    for (const auto& ln : L) {
        if (starts(ln, "declare ")) continue;
        if (ln.find("@dx.op.cbufferLoad.") != string::npos) strict("a non-legacy cbuffer read");
        if (ln.find("@dx.op.cbufferLoadLegacy.") == string::npos) continue;
        Sc c(ln);
        string L0, k1, k2, h, row;
        const bool ok = c.lit("  ") && c.pnum(&L0) && c.lit(" = call %dx.types.CBufRet.") && c.run(wrd, &k1) &&
                        c.lit(" @dx.op.cbufferLoadLegacy.") && c.run(wrd, &k2) && k1 == k2 &&
                        c.lit("(i32 59, %dx.types.Handle ") && c.pnum(&h) && c.lit(", i32 ") && (c.num(&row) || c.pnum(&row)) &&
                        c.lit(")") && (c.end() || c.lit("  ;"));
        if (!ok) strict("a cbuffer read not understood: " + ln.substr(0, 120));
        if (!cb_handle_range.count(h)) strict("a cbuffer read through a handle not made by createHandle");
    }
    // every cbuffer read classified: a block the rewrite handles by name, or one with a layout to read the variables of
    const auto lay = layouts(L);
    std::map<string, string> ranges;
    for (const auto& r : resources(L)) ranges[r.id] = r.name;
    for (const auto& kv : cb_handle_range) {
        const string& block = ranges.at(kv.second);
        if (view_block(block) || kSharedBlocks.count(block) || block == kPrevInstanced) continue;
        if (!lay.count(block)) strict("no layout for cbuffer " + block);
    }
}

// ---- the mod's own rewrite: vi_rewrite.py, "dyn" mode ----
struct Eye2 {
    const char* name;
    int lo, hi, delta;  // the per-eye rows [lo, hi) and eye 2's offset
};
const Eye2 kEye2[] = {{"rage_matrices", 4, 16, 13}, {"rage_prev_world", 0, 4, 5}, {"rage_prev_proj", 0, 4, 4}};
const Eye2* eye2(const string& n) {
    for (const auto& b : kEye2)
        if (n == b.name) return &b;
    return nullptr;
}
// classify.py's per-view members and camera-dependent names (VIEWY), for this rule's refusals
const std::set<string> kPerView = {"gWorldView", "gWorldViewProj", "gViewInverse", "gPrevWorldViewProjSingle", "gPrevViewProj"};
const std::set<string> kViewyOk = {"rage_matrices", "rage_prev_world", "rage_prev_proj", "rdr2_common", "rage_shadowmap_common"};
bool viewy(const string& n) {  // (View|Proj|Camera|NDC|Eye|ScreenToWorld|WorldToScreen|ClipTo|Frustum), re.I
    string s(n);
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    for (const char* k : {"view", "proj", "camera", "ndc", "eye", "screentoworld", "worldtoscreen", "clipto", "frustum"})
        if (s.find(k) != string::npos) return true;
    return false;
}
// ^; (\S+)\s+cbuffer\s+NA\s+NA\s+CB(\d+)\s+cb(\d+)   (the resource bindings table)
bool m_bind(const string& ln, string* name, string* cb) {
    Sc c(ln);
    return c.lit("; ") && c.run(non_space, name) && c.ws1() && c.lit("cbuffer") && c.ws1() && c.lit("NA") && c.ws1() && c.lit("NA") &&
           c.ws1() && c.lit("CB") && c.num(cb) && c.ws1() && c.lit("cb") && c.num();
}
bool py_blank(const string& s) {  // not s.strip()
    for (char ch : s)
        if (!spc(ch)) return false;
    return true;
}
string tail_line_replace_prefix(const string& tail, const string& prefix, const string& with) {  // re.sub('^' + prefix, with, M)
    Lines L = split(tail);
    for (auto& ln : L)
        if (ln.compare(0, prefix.size(), prefix) == 0) ln = with + ln.substr(prefix.size());
    return join(L);
}
string replace_all(string s, const string& from, const string& to) {
    for (size_t k = s.find(from); k != string::npos; k = s.find(from, k + to.size())) s.replace(k, from.size(), to);
    return s;
}
// fix_viewid_state(): the ViewID-dependent output mask, by taint over the SSA, into dx.viewIdState
string fix_viewid_state(const string& body, string tail, std::set<string> taint) {
    std::set<long long> outmask;
    const Lines T = split(tail);
    auto search_line = [&](const string& prefix) -> const string* {  // the first line starting with prefix
        for (const auto& ln : T)
            if (ln.compare(0, prefix.size(), prefix) == 0) return &ln;
        return nullptr;
    };
    string ep;
    {
        const size_t k = tail.find("!dx.entryPoints = !{!");
        Sc c(tail, k == string::npos ? tail.size() : k + 20);
        string d;
        if (k == string::npos || !c.lit("!") || !c.num(&d) || !c.lit("}")) strict("no dx.entryPoints");
        ep = "!" + d;
    }
    // ^ep = !\{void \(\)\* @\S+, !"\S+", (!\d+),
    string sigs;
    for (const auto& ln : T) {
        Sc c(ln);
        string fn, d;
        if (!c.lit(ep + " = !{void ()* @")) continue;
        // @\S+, !"\S+", : \S+ greedy, then backtracked to the first ", !\"" ... reached through a "\S+" (no spaces)
        const size_t q = ln.find(' ', c.p);
        if (q == string::npos || q == c.p || ln[q - 1] != ',') continue;
        Sc d2(ln, q + 1);
        if (!d2.lit("!\"")) continue;
        const size_t q2 = ln.find(' ', d2.p);
        if (q2 == string::npos || q2 < d2.p + 3 || ln.compare(q2 - 2, 2, "\",") != 0) continue;
        Sc d3(ln, q2 + 1);
        if (d3.lit("!") && d3.num(&d) && d3.lit(",")) {
            sigs = "!" + d;
            break;
        }
    }
    if (sigs.empty()) strict("the entry's signatures not found");
    string outlist;
    {  // ^sigs = !\{(!\d+|null), (!\d+|null), (!\d+|null)\}
        const string* ln = search_line(sigs + " = !{");
        if (!ln) strict("the signature lists not found");
        const string b = ln->substr(sigs.size() + 5);
        const size_t c1 = b.find(", "), c2 = c1 == string::npos ? c1 : b.find(", ", c1 + 2);
        if (c2 == string::npos) strict("the signature lists not understood");
        outlist = b.substr(c1 + 2, c2 - c1 - 2);
    }
    std::map<long long, std::pair<long long, long long>> pack;  // element id -> (start row, start column)
    {
        const string* ln = search_line(outlist + " = !{");
        if (outlist == "null" || !ln || ln->back() != '}') strict("the output signature not understood");
        const string els = ln->substr(outlist.size() + 5, ln->size() - outlist.size() - 6);
        size_t a = 0;
        for (;;) {
            const size_t b = els.find(", ", a);
            const string e = els.substr(a, b == string::npos ? string::npos : b - a);
            // ^e = !\{i32 (\d+), !"[^"]*", i8 \d+, i8 \d+, !\d+, i8 \d+, i32 (\d+), i8 (\d+), i32 (-?\d+), i8 (-?\d+)
            bool found = false;
            for (const auto& l : T) {
                Sc c(l);
                string id, r0, c0;
                if (!c.lit(e + " = !{i32 ") || !c.num(&id) || !c.lit(", !\"")) continue;
                while (c.p < l.size() && l[c.p] != '"') ++c.p;
                auto sn = [&](string* o) {
                    if (c.at() == '-') {
                        ++c.p;
                        if (!c.num(o)) return false;
                        *o = "-" + *o;
                        return true;
                    }
                    return c.num(o);
                };
                if (c.lit("\", i8 ") && c.num() && c.lit(", i8 ") && c.num() && c.lit(", !") && c.num() && c.lit(", i8 ") && c.num() &&
                    c.lit(", i32 ") && c.num() && c.lit(", i8 ") && c.num() && c.lit(", i32 ") && sn(&r0) && c.lit(", i8 ") && sn(&c0)) {
                    pack[to_int(id)] = {std::stoll(r0), std::stoll(c0)};
                    found = true;
                    break;
                }
            }
            if (!found) strict("an output element not understood: " + e);
            if (b == string::npos) break;
            a = b + 2;
        }
    }
    auto tok_ch = [](char ch) { return wrd(ch) || ch == '.'; };
    for (const auto& line : split(body)) {
        const string code = line.substr(0, line.find(';'));
        Sc c(code);  // \s*(%[\w.]+) = (.*)
        while (c.p < code.size() && spc(code[c.p])) ++c.p;
        string v;
        bool def = false;
        if (c.at() == '%') {
            size_t e = c.p + 1;
            while (e < code.size() && tok_ch(code[e])) ++e;
            if (e > c.p + 1 && code.compare(e, 3, " = ") == 0) {
                v = code.substr(c.p, e - c.p);
                c.p = e + 3;
                def = true;
            }
        }
        if (def) {
            for (size_t i = c.p; i < code.size(); ++i) {  // re.findall(r'%[\w.]+', rhs), not %dx.*
                if (code[i] != '%') continue;
                size_t e = i + 1;
                while (e < code.size() && tok_ch(code[e])) ++e;
                if (e == i + 1) continue;
                const string u = code.substr(i, e - i);
                i = e - 1;
                if (u.compare(0, 4, "%dx.") != 0 && taint.count(u)) {
                    taint.insert(v);
                    break;
                }
            }
            continue;
        }
        // @dx\.op\.storeOutput\.\w+\(i32 5, i32 (\d+), i32 (\d+), i8 (\d+), \w+ (%[\w.]+)\)
        for (size_t k = code.find("@dx.op.storeOutput."); k != string::npos; k = code.find("@dx.op.storeOutput.", k + 1)) {
            Sc s(code, k + 19);
            string id, row, col, val;
            if (!s.run(wrd) || !s.lit("(i32 5, i32 ") || !s.num(&id) || !s.lit(", i32 ") || !s.num(&row) || !s.lit(", i8 ") || !s.num(&col) ||
                !s.lit(", ") || !s.run(wrd) || !s.lit(" ") || s.at() != '%')
                continue;
            size_t e = s.p + 1;
            while (e < code.size() && tok_ch(code[e])) ++e;
            if (e == s.p + 1 || e >= code.size() || code[e] != ')') continue;
            val = code.substr(s.p, e - s.p);
            if (taint.count(val)) {
                auto it = pack.find(to_int(id));
                if (it == pack.end()) strict("a store to an output not in the signature");
                outmask.insert((it->second.first + to_int(row)) * 4 + it->second.second + to_int(col));
            }
            break;
        }
    }
    if (body.find(" br ") != string::npos) {  // control flow: every output ViewID-dependent
        outmask.clear();
        for (long long o = 0; o < 4096; ++o) outmask.insert(o);
    }
    // !\{\[(\d+) x i32\] \[(.*?)\]\}
    size_t m0 = string::npos, m1 = string::npos, v0 = 0, v1 = 0;
    for (size_t k = tail.find("!{["); k != string::npos; k = tail.find("!{[", k + 1)) {
        Sc c(tail, k + 3);
        if (!c.num() || !c.lit(" x i32] [")) continue;
        const size_t e = tail.find("]}", c.p);
        if (e == string::npos || tail.find(static_cast<char>(10), c.p) < e) continue;  // ((.*?): on one line)
        m0 = k;
        m1 = e + 2;
        v0 = c.p;
        v1 = e;
        break;
    }
    if (m0 == string::npos) strict("no dx.viewIdState");
    std::vector<long long> vals;
    {
        const string vs = tail.substr(v0, v1 - v0);
        size_t a = 0;
        for (;;) {
            const size_t b = vs.find(", ", a);
            const string x = vs.substr(a, b == string::npos ? string::npos : b - a);
            const size_t sp = x.find(' ');
            if (sp == string::npos) strict("dx.viewIdState not understood");
            vals.push_back(std::stoll(x.substr(sp + 1)));
            if (b == string::npos) break;
            a = b + 2;
        }
    }
    if (vals.size() < 2) strict("dx.viewIdState not understood");
    const long long nin = vals[0], nout = vals[1], words = (nout + 31) / 32;
    std::vector<unsigned long long> mask(static_cast<size_t>(words), 0);
    for (long long o : outmask)
        if (o < nout) mask[static_cast<size_t>(o / 32)] |= 1ull << (o % 32);
    std::vector<string> nv = {I(nin), I(nout)};
    for (auto w : mask) nv.push_back(std::to_string(w));
    for (size_t i = 2; i < vals.size(); ++i) nv.push_back(I(vals[i]));
    string rep = "!{[" + I(static_cast<long long>(nv.size())) + " x i32] [";
    for (size_t i = 0; i < nv.size(); ++i) rep += (i ? ", i32 " : "i32 ") + nv[i];
    rep += "]}";
    return replace_all(tail, tail.substr(m0, m1 - m0), rep);
}

// this rule's reads: refuse what it cannot make right for eye 2 (vi_rewrite.py itself refuses nothing)
// (run 26, --prevworld) rage_prev_world as analysed (research/run19/prevworld.md §2): gPrevWorldViewProjSingle rows 0-3,
// gViewport row 4, 80 bytes; eye 2's rows 5-8 are the mirror's, at +0x50 of the grown CB
void check_prevworld_layout(const std::map<string, std::vector<LRow>>& lay, const Lines& L) {
    auto it = lay.find(kPrevWorld);
    if (it == lay.end()) strict("--prevworld: no layout for rage_prev_world");
    const auto& v = it->second;
    if (v.size() != 2 || v[0].name != "gPrevWorldViewProjSingle" || v[0].a != 0 || v[0].b != 3 || v[1].name != "gViewport" || v[1].a != 4 ||
        v[1].b != 4)
        strict("--prevworld: rage_prev_world's layout is not gPrevWorldViewProjSingle rows 0-3 and gViewport row 4");
    int declared = 0;
    bool in = false;
    for (const auto& ln : L) {  // (layouts()'s own section: "; cbuffer rage_prev_world" to its "} ...; Offset: Size:" line)
        long long size = 0;
        if (ln == string("; cbuffer ") + kPrevWorld) {
            in = true;
        } else if (in && m_layout_end(ln, &size)) {
            in = false;
            if (size != 80) strict("--prevworld: rage_prev_world is not 80 bytes");
            ++declared;
        }
    }
    if (declared != 1) strict("--prevworld: rage_prev_world's layout not found once");
}

void check_reads_ours(const Lines& L, bool* wvp, bool our_layout, int instprev_k, int* ip_reads, bool prevworld = false, int* pw_reads = nullptr) {
    const auto lay = layouts(L);
    std::map<string, string> ranges;
    for (const auto& r : resources(L)) ranges[r.id] = r.name;
    for (const auto& ln : L) {  // the bindings table and the metadata agree on each cbuffer's name
        string name, cb;
        if (m_bind(ln, &name, &cb) && ranges.count(cb) && ranges[cb] != name)
            strict("the bindings table and the metadata disagree on CB" + cb);
    }
    std::map<string, string> handles;
    for (const auto& h : handles2(L)) handles[h.h] = ranges.count(h.rid) ? ranges[h.rid] : "?";
    std::vector<CB> loads;
    for (const auto& ln : L)
        if (ln.find("@dx.op.cbufferLoadLegacy.") != string::npos) m_cb(ln, &loads);
    for (const auto& m : loads) {
        const string block = handles.count(m.h) ? handles[m.h] : "?";
        const bool dyn = m.row[0] == '%';
        const long long row = dyn ? -1 : to_int(m.row);
        if (const Eye2* b = eye2(block)) {
            if (dyn) refuse("dynamic row index into " + block);
            if (block == "rage_matrices" && row >= 8 && row < 12 && wvp) *wvp = true;
            if (our_layout && block == kPrevWorld && row >= b->lo && row < b->hi) {
                if (!prevworld) strict("needs eye 2's rage_prev_world rows 5-8, which the mod does not write");
                ++*pw_reads;  // (run 26, --prevworld: rows 5-8 are [Stereo] PrepassPrevWorldMirror's)
            }
            if (prevworld && block == kPrevWorld && row > 4)  // (rows 5-8 are eye 2's under the flag: never the game's own)
                strict("--prevworld: reads rage_prev_world row " + I(row) + ", past its declared rows");
            if (row >= b->lo && row < b->hi) continue;
        }
        if (block == kPrevInstanced) {
            if (instprev_k > 0) {  // (--instprev K: eye 2's entries at K + i, PrepassInstPrevMirror's; any row, static or not)
                ++*ip_reads;
                continue;
            }
            refuse("reads rage_instancemtxprev (eye 1's previous positions, no eye-2 copy)");
        }
        auto lit = lay.find(block);
        if (lit == lay.end()) {
            if (eye2(block) || kSharedBlocks.count(block)) continue;
            strict("no layout for cbuffer " + block);
        }
        for (const auto& e : lit->second) {
            if (!dyn && !(e.a <= row && row <= e.b)) continue;
            if (kPerView.count(e.name) || (!kViewyOk.count(block) && viewy(e.name) && !kViewNameOk.count(e.name)))
                refuse("reads " + block + "." + e.name + " (camera-dependent, no eye-2 copy)");
        }
    }
}

// (--instprev K) rage_instancemtxprev's declared size grown from N x 64 to (K + N) x 64: its struct type (in the head,
// ^TYPE = type { [N x [4 x <4 x float>]] }$) and its dx.resources entry (in the tail, ^!\d+ = !{..., %TYPE* undef, ...,
// i32 SIZE, null}$), refused when either is not in that form, when they disagree, or past 64 KB
void grow_instprev(string* head, string* tail, int k) {
    Lines T = split(*tail);
    const string kNull = ", null}";
    string ty;
    long long size = -1;
    size_t at = string::npos, q = string::npos;
    for (size_t i = 0; i < T.size(); ++i) {
        const string& ln = T[i];
        if (ln.empty() || ln[0] != '!') continue;
        const size_t u = ln.find("* undef, !\"");
        if (u == string::npos) continue;
        const size_t pc = ln.rfind('%', u);
        if (pc == string::npos) continue;
        const string tok = ln.substr(pc, u - pc);
        string bare = tok.substr(1);
        if (bare.size() >= 2 && bare.front() == '"' && bare.back() == '"') bare = bare.substr(1, bare.size() - 2);
        if (starts(bare, "hostlayout.")) bare = bare.substr(11);
        if (bare != kPrevInstanced) continue;
        if (!ty.empty()) strict("rage_instancemtxprev declared twice");
        if (ln.size() < kNull.size() || ln.compare(ln.size() - kNull.size(), kNull.size(), kNull) != 0)
            strict("rage_instancemtxprev's dx.resources entry not understood");
        q = ln.rfind(", i32 ", ln.size() - kNull.size());
        if (q == string::npos) strict("rage_instancemtxprev's dx.resources entry not understood");
        const string digits = ln.substr(q + 6, ln.size() - kNull.size() - (q + 6));
        if (digits.empty() || digits.size() > 6) strict("rage_instancemtxprev's size not understood");
        for (char ch : digits)
            if (!dig(ch)) strict("rage_instancemtxprev's size not understood");
        ty = tok;
        size = to_int(digits);
        at = i;
    }
    if (ty.empty()) strict("rage_instancemtxprev's dx.resources entry not found");
    Lines H = split(*head);
    const string pre = ty + " = type { [", post = " x [4 x <4 x float>]] }";
    long long count = -1;
    size_t hat = string::npos;
    for (size_t i = 0; i < H.size(); ++i) {
        const string& ln = H[i];
        if (ln.size() <= pre.size() + post.size() || !starts(ln, pre.c_str()) || ln.compare(ln.size() - post.size(), post.size(), post) != 0)
            continue;
        const string digits = ln.substr(pre.size(), ln.size() - pre.size() - post.size());
        bool ok = !digits.empty() && digits.size() <= 5;
        for (char ch : digits) ok = ok && dig(ch);
        if (!ok || hat != string::npos) strict("rage_instancemtxprev's type not understood");
        count = to_int(digits);
        hat = i;
    }
    if (hat == string::npos || count < 1) strict("rage_instancemtxprev's type not understood");
    if (size != count * 64) strict("rage_instancemtxprev's declared size is not its type's");
    const long long grown = count + k;
    if (grown * 64 > 65536) strict("rage_instancemtxprev grown past 64 KB (K + N over 1024)");
    H[hat] = pre + I(grown) + post;
    T[at] = T[at].substr(0, q + 6) + I(grown * 64) + kNull;
    *head = join(H);
    *tail = join(T);
}

// ---- (run 24) eye 2's images, route (c1): global_textures uints 11 and 13 selected by ViewID ----
// research/run20/p3-textures.md §1.2: global_textures (192 B, 48 uints) holds each global texture's bindless index at
// uint r - 150 (0-23) and its sampler state at 24-47; the Light Buffer (t161) is uint 11, the Shadow Collector (t163)
// uint 13, both set by 0x149140. The mod puts eye 2's indices at uints LB2 and SC2 (run 23 images.md §4.3).
const char* const kGlobalTextures = "global_textures";
const int kLbSlot = 11, kScSlot = 13;
const char* slot_label(int s) { return s == kLbSlot ? "uint 11 (LightBufferSampler)" : "uint 13 (ShadowCollectorSampler)"; }
[[noreturn]] void refuse_e2(const string& why) { refuse("eye-2 images: " + why); }

struct E2Pick {
    string handle;  // the global_textures handle the original load read through
    int slot;       // 11 or 13
    long long row;  // eye 2's uint: its row and component
    int comp;
};
struct E2Plan {
    std::map<string, E2Pick> picks;  // {the extract of uint 11 / 13: its pick}
    std::set<string> handles;        // global_textures' handles (the guard's load takes one)
    std::set<string> srv;            // the texture handles whose index is a pick (made non-uniform)
};
// the code of one body line: "" for a comment or label line, else the text before its "  ;" comment
string code_of(const string& ln) {
    size_t a = 0;
    while (a < ln.size() && spc(ln[a])) ++a;
    if (a == ln.size() || ln[a] == ';') return "";
    return ln.substr(0, ln.find("  ;"));
}
// ^  (%\d+) = extractvalue %dx\.types\.CBufRet\.<kind> (%\d+), (\d)$   (on the code, whole)
bool m_ext_whole(const string& code, const string& kind, string* v, string* L, int* comp) {
    Sc c(code);
    if (!c.lit("  ") || !c.pnum(v) || !c.lit(" = extractvalue %dx.types.CBufRet.") || !c.lit(kind) || !c.lit(" ") || !c.pnum(L) ||
        !c.lit(", ") || !dig(c.at()))
        return false;
    *comp = c.at() - '0';
    ++c.p;
    return c.end();
}
// every read that can reach uint 11 or 13 checked, and the extracted indices to select. The refusals come only from a
// read that can reach them (a global_textures row 2 or 3 load, a dynamic row into it, a read of it not understood), so a
// shader whose reads never reach them gets an empty plan: converted byte for byte as without the flag
E2Plan plan_eye2(const Lines& L, const Lines& B, const Eye2Images& e2) {
    E2Plan plan;
    std::set<string> rids;  // (two global_textures ranges: the grow refuses them, only when a pick needs it)
    for (const auto& r : resources(L))
        if (r.name == kGlobalTextures) rids.insert(r.id);
    if (rids.empty()) return plan;
    for (const auto& h : handles2(L))  // its handles (any form; strict_checks refused a dynamic cbuffer handle index)
        if (rids.count(h.rid)) plan.handles.insert(h.h);
    if (plan.handles.empty()) return plan;
    // the uses of each value: the lines (of the code, not the comments) whose right-hand side names it
    std::map<string, std::vector<size_t>> uses;
    for (size_t i = 0; i < B.size(); ++i) {
        const string code = code_of(B[i]);
        if (code.empty()) continue;
        string v;
        size_t e = 0;
        const string rhs = code.substr(m_def(code, &v, &e) ? e : 0);
        std::set<string> seen;
        any_token(rhs, [&](const string& t) {
            if (seen.insert(t).second) uses[t].push_back(i);
            return false;
        });
    }
    // global_textures' row loads that hold uint 11 or 13 (rows 2 and 3): {load value: (handle, row, kind)}
    struct RowLoad {
        string h;
        long long row;
        string kind;
    };
    std::map<string, RowLoad> rows;
    for (size_t i = 0; i < B.size(); ++i) {
        const string code = code_of(B[i]);
        if (code.find("@dx.op.cbufferLoadLegacy.") == string::npos) continue;
        Sc c(code);
        string Ld, k1, k2, h, row;
        const bool ok = c.lit("  ") && c.pnum(&Ld) && c.lit(" = call %dx.types.CBufRet.") && c.run(wrd, &k1) &&
                        c.lit(" @dx.op.cbufferLoadLegacy.") && c.run(wrd, &k2) && k1 == k2 && c.lit("(i32 59, %dx.types.Handle ") &&
                        c.pnum(&h) && c.lit(", i32 ") && (c.num(&row) || c.pnum(&row)) && c.lit(")") && c.end();
        if (!ok) {  // (strict_checks already refuses these; here only one that may read global_textures matters)
            if (any_token(code, [&](const string& t) { return plan.handles.count(t) != 0; }))
                refuse_e2("a global_textures read not understood: " + code.substr(0, 120));
            continue;
        }
        if (!plan.handles.count(h)) continue;
        if (row[0] == '%') refuse_e2("a dynamic row into global_textures (" + row + ")");
        const long long r = to_int(row);
        if (r == kLbSlot / 4 || r == kScSlot / 4) rows[Ld] = {h, r, k1};
    }
    const auto names = slot_names(L);
    auto srv_index = [&](size_t at, const string& val, string* hh) {  // createHandle(i32 57, i8 0, ..., val, ...)
        string rr, idx;
        return m_h0(code_of(B[at]), hh, &rr, &idx) && idx == val;
    };
    for (const auto& kv : rows) {
        const string& Ld = kv.first;
        const RowLoad& rl = kv.second;
        for (size_t i : uses[Ld]) {
            string v, L2;
            int comp = 0;
            if (!m_ext_whole(code_of(B[i]), rl.kind, &v, &L2, &comp) || L2 != Ld)
                refuse_e2("global_textures row " + I(rl.row) + " used other than by extractvalue: " + code_of(B[i]).substr(0, 120));
            const int slot = static_cast<int>(rl.row * 4 + comp);
            if (slot != kLbSlot && slot != kScSlot) continue;  // (another uint of the row: not this rule's)
            if (rl.kind != "i32") refuse_e2(string(slot_label(slot)) + " read as " + rl.kind + ", not i32");
            const char* want = slot == kLbSlot ? "LightBufferSampler" : "ShadowCollectorSampler";
            auto nm = names.find(slot);
            if (nm == names.end()) refuse_e2(string(slot_label(slot)) + " read with no layout name for it");
            if (nm->second != want) refuse_e2("uint " + I(slot) + " is " + nm->second + " in this shader's layout, not " + want);
            // every use a texture handle's index: createHandle(i32 57, i8 0, ...) directly, or through "add i32 v, 0"
            for (size_t j : uses[v]) {
                string hh;
                if (srv_index(j, v, &hh)) {
                    plan.srv.insert(hh);
                    continue;
                }
                string a, b;
                if (m_add0(code_of(B[j]), &a, &b) && b == v) {
                    for (size_t k : uses[a]) {
                        if (!srv_index(k, a, &hh))
                            refuse_e2(string(slot_label(slot)) + " used other than as a texture handle's index: " + code_of(B[k]).substr(0, 120));
                        plan.srv.insert(hh);
                    }
                    continue;
                }
                refuse_e2(string(slot_label(slot)) + " used other than as a texture handle's index: " + code_of(B[j]).substr(0, 120));
            }
            const int u = slot == kLbSlot ? e2.lb : e2.sc;
            plan.picks[v] = {rl.h, slot, u / 4, u % 4};
        }
    }
    return plan;
}
// global_textures' declared size grown to `need` bytes: its type (in the head, ^TYPE = type { i32, ..., i32 }$) and its
// dx.resources entry (in the tail, ^!\d+ = !{..., %TYPE* undef, ..., i32 SIZE, null}$); refused when either is not in
// that form or they disagree. Nothing when it is already that large.
void grow_global_textures(string* head, string* tail, long long need) {
    Lines T = split(*tail);
    const string kNull = ", null}";
    string ty;
    long long size = -1;
    size_t at = string::npos, q = string::npos;
    for (size_t i = 0; i < T.size(); ++i) {
        const string& ln = T[i];
        string id, name;
        if (!m_res(ln, &id, &name) || name != kGlobalTextures) continue;
        if (!ty.empty()) refuse_e2("global_textures declared twice");
        const size_t u = ln.find("* undef, !\"");
        const size_t pc = u == string::npos ? string::npos : ln.rfind('%', u);
        if (pc == string::npos) refuse_e2("global_textures' dx.resources entry not understood");
        q = ln.rfind(", i32 ", ln.size() - kNull.size());
        if (q == string::npos) refuse_e2("global_textures' dx.resources entry not understood");
        const string digits = ln.substr(q + 6, ln.size() - kNull.size() - (q + 6));
        bool ok = !digits.empty() && digits.size() <= 5;
        for (char ch : digits) ok = ok && dig(ch);
        if (!ok) refuse_e2("global_textures' size not understood");
        ty = ln.substr(pc, u - pc);
        size = to_int(digits);
        at = i;
    }
    if (ty.empty()) refuse_e2("global_textures' dx.resources entry not found");
    if (size >= need) return;
    Lines H = split(*head);
    const string pre = ty + " = type { ", post = " }";
    size_t hat = string::npos;
    long long count = 0;
    for (size_t i = 0; i < H.size(); ++i) {
        const string& ln = H[i];
        if (!starts(ln, pre.c_str())) continue;
        if (hat != string::npos) refuse_e2("global_textures' type declared twice");
        if (ln.size() < pre.size() + post.size() || ln.compare(ln.size() - post.size(), post.size(), post) != 0)
            refuse_e2("global_textures' type not understood");
        const string fields = ln.substr(pre.size(), ln.size() - pre.size() - post.size());  // "i32, i32, ..., i32"
        size_t a = 0;
        count = 0;
        for (;;) {
            const size_t b = fields.find(", ", a);
            if (fields.substr(a, b == string::npos ? string::npos : b - a) != "i32") refuse_e2("global_textures' type is not all i32");
            ++count;
            if (b == string::npos) break;
            a = b + 2;
        }
        hat = i;
    }
    if (hat == string::npos) refuse_e2("global_textures' type not found");
    if (size != count * 4) refuse_e2("global_textures' declared size is not its type's");
    string f;
    for (long long k = 0; k < need / 4; ++k) f += k ? ", i32" : "i32";
    H[hat] = pre + f + post;
    T[at] = T[at].substr(0, q + 6) + I(need) + kNull;
    *head = join(H);
    *tail = join(T);
}

}  // namespace

Rewrite rewrite_ours(const string& t, bool* wvp_out, bool our_layout, int instprev_k, int* instprev_out, const Eye2Images& e2,
                     int* eye2_out, bool prevworld, int* prevworld_out) {
    Rewrite r;
    bool wvp = false;
    int ip_loads = 0, e2_loads = 0, pw_loads = 0;
    try {
        if (t.find("@dx.op.viewID") != string::npos) refuse("already uses ViewID");
        if (instprev_k < 0 || instprev_k > 768) strict("--instprev K out of 1..768");
        if (prevworld && !our_layout) strict("--prevworld needs the mod's layout");  // (run 26: its rows are the mod's mirror's)
        const bool e2_on = e2.on();
        if (e2_on) {
            auto in = [](int u) { return u >= kEye2UintLo && u <= kEye2UintHi; };
            if (!in(e2.lb) || !in(e2.sc) || !in(e2.guard) || e2.lb == e2.sc || e2.lb == e2.guard || e2.sc == e2.guard)
                strict("--eye2-images LB2,SC2,GUARD: three different uints in 48..58");
        }
        std::set<string> e2_handles;  // (the guard's load: a global_textures handle made at the entry's start)
        const Lines L = split(t);
        strict_checks(L);
        int ip_reads = 0, pw_reads = 0;
        check_reads_ours(L, &wvp, our_layout, instprev_k, &ip_reads, prevworld, &pw_reads);
        if (pw_reads) check_prevworld_layout(layouts(L), L);  // (run 26, --prevworld: only a shader that reads rows 0-3)
        if (instprev_k > 0 && ip_reads && t.find(kF32LoadDecl) == string::npos)
            strict("rage_instancemtxprev read, but no cbufferLoadLegacy.f32 declared (the guard's load)");
        std::map<long long, string> binds;
        for (const auto& ln : L) {
            string name, cb;
            if (m_bind(ln, &name, &cb)) binds[to_int(cb)] = name;
        }
        const size_t d = t.find("define void @");
        if (d == string::npos) strict("no entry function");
        string head = t.substr(0, d);  // (--instprev K: rage_instancemtxprev's type grown)
        string rest = t.substr(d + 13);
        const size_t nl = rest.find('\n');
        if (nl == string::npos) strict("no entry function");
        const string fname = rest.substr(0, nl);
        rest = rest.substr(nl + 1);
        const size_t be = rest.find("\n}\n");
        if (be == string::npos) strict("the entry function's end not found");
        string body = rest.substr(0, be), tail = rest.substr(be + 3);
        // handles: %(\d+) = call ... createHandle\(i32 57, i8 2, i32 (\d+), i32 \S+, i1 false\)   (in the body)
        std::map<string, string> handles;  // value number -> block
        const Lines B = split(body);
        for (const auto& ln : B) {
            for (size_t k = ln.find(kH2); k != string::npos; k = ln.find(kH2, k + 1)) {
                string h;
                size_t st = 0;
                if (!pct_before(ln, k, &h, &st)) continue;
                Sc c(ln, k + std::strlen(kH2));
                string rid;
                if (!c.num(&rid) || !c.lit(", i32 ")) continue;
                size_t e = c.p;
                while (e < ln.size() && !spc(ln[e])) ++e;
                if (e - c.p < 2 || ln[e - 1] != ',' || ln.compare(e, 10, " i1 false)") != 0) continue;
                auto it = binds.find(to_int(rid));
                handles[h.substr(1)] = it == binds.end() ? "" : it->second;
            }
        }
        Lines out;
        std::set<string> seeds = {"%vi_view"};
        int n = 0, fresh = 0;
        for (string line : B) {
            // (\s*%(\d+) = call %dx\.types\.CBufRet\.(\w+) @dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, %dx\.types\.Handle %(\d+), i32 )(\d+)(\).*)
            Sc c(line);
            while (c.p < line.size() && spc(line[c.p])) ++c.p;
            string v, h, row;
            if (c.pnum(&v) && c.lit(" = call %dx.types.CBufRet.") && c.run(wrd) && c.lit(" @dx.op.cbufferLoadLegacy.") && c.run(wrd) &&
                c.lit("(i32 59, %dx.types.Handle %") && c.num(&h) && c.lit(", i32 ")) {
                const size_t g5 = c.p;
                Sc c2(line, g5);
                string rv;
                auto ip = instprev_k > 0 ? handles.find(h) : handles.end();
                if (ip != handles.end() && ip->second == kPrevInstanced && (c2.num(&rv) || c2.pnum(&rv)) && c2.at() == ')') {
                    // (--instprev K) a rage_instancemtxprev load, its row static or an SSA value (SV_InstanceID x 4 + r, shared
                    // with rage_instancemtx's loads, which stay as they are): row + ViewID x 4K, as a new value of its own
                    // The guard: eye 2's row-3 .w of entry K + i read first; 0 (the CBV only n x 64, read past its end: the
                    // mirror did not grow this draw) reads eye 1's entry i instead, as before the rewrite, never 0's
                    ++n;
                    ++ip_loads;
                    seeds.insert(v);
                    const string off = "%vi_ipoff" + I(++fresh), r3 = "%vi_ipr3" + I(++fresh), wrow = "%vi_ipwrow" + I(++fresh),
                                 wl = "%vi_ipw" + I(++fresh), ww = "%vi_ipww" + I(++fresh), nz = "%vi_ipnz" + I(++fresh),
                                 sel = "%vi_ipsel" + I(++fresh), idx = "%vi_iprow" + I(++fresh);
                    out.push_back("  " + off + " = mul i32 %vi_view, " + I(4LL * instprev_k));
                    if (rv[0] == '%') {
                        out.push_back("  " + r3 + " = or i32 " + rv + ", 3");
                        out.push_back("  " + wrow + " = add i32 " + off + ", " + r3);
                    } else {
                        out.push_back("  " + wrow + " = add i32 " + off + ", " + I(to_int(rv) | 3));
                    }
                    out.push_back("  " + wl + " = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %" + h + ", i32 " +
                                  wrow + ")  ; CBufferLoadLegacy(handle,regIndex)");
                    out.push_back("  " + ww + " = extractvalue %dx.types.CBufRet.f32 " + wl + ", 3");
                    out.push_back("  " + nz + " = fcmp une float " + ww + ", 0.000000e+00");
                    out.push_back("  " + sel + " = select i1 " + nz + ", i32 " + off + ", i32 0");
                    out.push_back("  " + idx + " = add i32 " + sel + ", " + rv);
                    line = line.substr(0, g5) + idx + line.substr(c2.p);
                } else if (c.num(&row) && c.at() == ')') {
                    auto it = handles.find(h);
                    const Eye2* b = it == handles.end() ? nullptr : eye2(it->second);
                    const long long rr = to_int(row);
                    if (b && rr >= b->lo && rr < b->hi) {
                        ++n;
                        if (pw_reads && b->name == string(kPrevWorld)) ++pw_loads;  // (run 26: rows 0-3 -> 5-8 for view 1)
                        seeds.insert(v);
                        const string off = "%vi_off" + I(++fresh), idx = "%vi_row" + I(++fresh);
                        out.push_back("  " + off + " = mul i32 %vi_view, " + I(b->delta));
                        out.push_back("  " + idx + " = add i32 " + off + ", " + I(rr));
                        line = line.substr(0, g5) + idx + line.substr(c.p);
                    }
                }
            }
            out.push_back(line);
        }
        if (ip_loads != ip_reads) strict("rage_instancemtxprev: " + I(ip_reads) + " loads read, " + I(ip_loads) + " rewritten");
        if (pw_loads != pw_reads) strict("--prevworld: rage_prev_world rows 0-3 read " + I(pw_reads) + " times, " + I(pw_loads) + " rewritten");
        if (ip_loads) grow_instprev(&head, &tail, instprev_k);
        if (e2_on) {  // (run 24, --eye2-images) uints 11 and 13: ViewID != 0, guarded, reads eye 2's index at LB2 / SC2
            const E2Plan plan = plan_eye2(L, B, e2);
            if (!plan.picks.empty()) {
                std::map<string, string> sel;  // the original extract -> its select
                Lines o2;
                std::vector<bool> keep;        // lines not renamed: the picks' own defs and the lines added here
                o2.reserve(out.size() + 3 * plan.picks.size());
                for (const auto& ln : out) {
                    string v;
                    size_t e = 0;
                    const bool def = m_def(ln, &v, &e) && plan.picks.count(v);
                    o2.push_back(ln);
                    keep.push_back(def);
                    if (!def) continue;
                    const E2Pick& p = plan.picks.at(v);
                    ++e2_loads;
                    const string ld = "%vi_gtr" + I(e2_loads), x = "%vi_gtx" + I(e2_loads), s = "%vi_gts" + I(e2_loads);
                    o2.push_back("  " + ld + " = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle " + p.handle +
                                 ", i32 " + I(p.row) + ")  ; CBufferLoadLegacy(handle,regIndex)");
                    o2.push_back("  " + x + " = extractvalue %dx.types.CBufRet.i32 " + ld + ", " + I(p.comp));
                    o2.push_back("  " + s + " = select i1 %vi_e2, i32 " + x + ", i32 " + v);
                    keep.insert(keep.end(), 3, true);
                    sel[v] = s;
                    seeds.insert(s);
                }
                if (e2_loads != static_cast<int>(plan.picks.size()))
                    strict("eye-2 images: " + I(static_cast<long long>(plan.picks.size())) + " indices found, " + I(e2_loads) + " rewritten");
                int nonuniform = 0;
                for (size_t i = 0; i < o2.size(); ++i) {
                    if (keep[i]) continue;
                    string hh, rr, idx;  // the texture handles that take a select: non-uniform (both views may share a wave)
                    const bool srv = m_h0(o2[i], &hh, &rr, &idx) && plan.srv.count(hh);  // (before the rename: idx is %\d+)
                    o2[i] = sub_tokens(o2[i], [&](const string& tk) -> const string* {
                        auto it = sel.find(tk);
                        return it == sel.end() ? nullptr : &it->second;
                    });
                    if (srv) {  // createHandle(i32 57, i8 0, i32 rid, i32 idx, i1 <flag>): the first ", i1 " past the call
                        const size_t c0 = o2[i].find("@dx.op.createHandle(");
                        const size_t k = c0 == string::npos ? string::npos : o2[i].find(", i1 ", c0);
                        if (k == string::npos) strict("eye-2 images: a texture handle's flag not found");
                        if (o2[i].compare(k + 5, 6, "false)") == 0) o2[i].replace(k + 5, 5, "true");
                        else if (o2[i].compare(k + 5, 5, "true)") != 0) strict("eye-2 images: a texture handle's flag not understood");
                        ++nonuniform;
                    }
                }
                if (nonuniform != static_cast<int>(plan.srv.size())) strict("eye-2 images: the texture handles not all made non-uniform");
                out = std::move(o2);
                n += e2_loads;
                e2_handles = plan.handles;
                grow_global_textures(&head, &tail, kEye2GrownBytes);
            }
        }
        if (!n) refuse("no view-dependent block");
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i].find("@dx.op.createHandle") == string::npos && !py_blank(out[i])) {
                Lines top = {"  %vi_view = call i32 @dx.op.viewID.i32(i32 138)  ; ViewID()", "  %vi_is1 = icmp ne i32 %vi_view, 0"};
                if (e2_loads) {  // the guard, read once here: eye 2's indices only while uint GUARD holds the magic
                    string gh;
                    for (size_t k = 0; k < i && gh.empty(); ++k) {  // (out[0..i): the entry's leading createHandle lines)
                        std::vector<H2> hs;
                        m_h2(out[k], &hs);
                        for (const auto& h : hs)
                            if (e2_handles.count(h.h)) gh = h.h;
                    }
                    if (gh.empty()) refuse_e2("no global_textures handle made at the entry's start (the guard's load)");
                    top.push_back("  %vi_gtg = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle " + gh + ", i32 " +
                                  I(e2.guard / 4) + ")  ; CBufferLoadLegacy(handle,regIndex)");
                    top.push_back("  %vi_gtgx = extractvalue %dx.types.CBufRet.i32 %vi_gtg, " + I(e2.guard % 4));
                    top.push_back("  %vi_gtok = icmp eq i32 %vi_gtgx, " + I(static_cast<int32_t>(kEye2GuardMagic)));
                    top.push_back("  %vi_e2 = and i1 %vi_is1, %vi_gtok");
                }
                out.insert(out.begin() + i, top.begin(), top.end());
                break;
            }
        body = join(out);
        // declarations, SM 6.1, DXIL 1.1, flags
        string grp;
        for (const auto& ln : split(tail)) {
            Sc c(ln);
            string g;
            if (c.lit("attributes #") && c.num(&g) && c.lit(" = { nounwind readnone }")) {
                grp = g;
                break;
            }
        }
        if (grp.empty()) {
            grp = "90";
            const size_t k = tail.find("\n!llvm.ident");
            if (k != string::npos) tail.replace(k, 12, "\nattributes #90 = { nounwind readnone }\n\n!llvm.ident");
        }
        const size_t fa = tail.find("; Function Attrs:");
        if (fa == string::npos) strict("no function attributes");
        tail.insert(fa, "; Function Attrs: nounwind readnone\ndeclare i32 @dx.op.viewID.i32(i32) #" + grp + "\n\n");
        tail = replace_all(tail, "!{!\"vs\", i32 6, i32 0}", "!{!\"vs\", i32 6, i32 1}");
        tail = replace_all(tail, "!{!\"ps\", i32 6, i32 0}", "!{!\"ps\", i32 6, i32 1}");
        string verid;
        {
            const size_t k = tail.find("!dx.version = !{!");
            Sc c(tail, k == string::npos ? tail.size() : k + 16);
            string dd;
            if (k == string::npos || !c.lit("!") || !c.num(&dd) || !c.lit("}")) strict("no dx.version");
            verid = "!" + dd;
        }
        tail = tail_line_replace_prefix(tail, verid + " = !{i32 1, i32 0}", verid + " = !{i32 1, i32 1}");
        string ep;
        {
            const size_t k = tail.find("!dx.entryPoints = !{!");
            Sc c(tail, k == string::npos ? tail.size() : k + 20);
            string dd;
            if (k == string::npos || !c.lit("!") || !c.num(&dd) || !c.lit("}")) strict("no dx.entryPoints");
            ep = "!" + dd;
        }
        string props;
        for (const auto& ln : split(tail)) {  // ^ep = !\{.*, (!\d+|null)\}$
            if (ln.compare(0, ep.size() + 5, ep + " = !{") != 0 || ln.back() != '}') continue;
            const string b = ln.substr(ep.size() + 5, ln.size() - ep.size() - 6);
            const size_t k = b.rfind(", ");
            if (k == string::npos) continue;
            const string last = b.substr(k + 2);
            bool ok = last == "null";
            if (!ok && last.size() > 1 && last[0] == '!') {
                ok = true;
                for (size_t q = 1; q < last.size(); ++q) ok = ok && dig(last[q]);
            }
            if (ok) {
                props = last;
                break;
            }
        }
        if (props.empty()) strict("no entry properties line");
        {  // ^(props) = !\{i32 0, i64 (\d+)(.*)$ -> the ViewID flag
            Lines TL = split(tail);
            for (auto& ln : TL) {
                Sc c(ln);
                string dd;
                if (!c.lit(props + " = !{i32 0, i64 ") || !c.num(&dd)) continue;
                const size_t a = c.p - dd.size();
                ln = ln.substr(0, a) + std::to_string(std::stoull(dd) | kViewIdFlag) + ln.substr(c.p);
            }
            tail = join(TL);
        }
        tail = fix_viewid_state(body, tail, seeds);
        r.ok = true;
        r.n = n;
        r.text = head + "define void @" + fname + "\n" + body + "\n}\n" + tail;
    } catch (const Refused& e) {
        r = Rewrite{};
        r.refused = e.why;
    } catch (const std::bad_alloc&) {
        throw;  // (out of memory: not the shader's fault; the converter exits without a result)
    } catch (const std::exception& e) {
        r = Rewrite{};
        r.refused = string("strict: ") + e.what();
    }
    if (wvp_out) *wvp_out = wvp;
    if (instprev_out) *instprev_out = r.ok ? ip_loads : 0;
    if (eye2_out) *eye2_out = r.ok ? e2_loads : 0;
    if (prevworld_out) *prevworld_out = r.ok ? pw_loads : 0;
    return r;
}

string entry_name(const string& text) {  // ^define void @(\w+)\(
    for (const auto& ln : split(text)) {
        Sc c(ln);
        string n;
        if (c.lit("define void @") && c.run(wrd, &n) && c.lit("(")) return n;
    }
    return "";
}

Rewrite rewrite(const string& text, bool our_layout) {
    Rewrite r;
    try {
        try {
            if (text.find("@dx.op.viewID") != string::npos) refuse("already uses ViewID");
            const Lines lines = split(text);
            std::map<string, string> ranges;  // range id -> block (the view-dependent ones)
            for (const auto& x : resources(lines))
                if (view_block(x.name)) ranges[x.id] = x.name;
            Found found;
            r.approx = check_reads(lines, &found);
            std::map<string, string> handles;
            for (const auto& h : handles2(lines))
                if (h.tail && ranges.count(h.rid)) handles[h.h] = ranges[h.rid];
            const auto screen = screen_slot_reads(lines);
            Lines out;
            int n_redirect = 0;
            bool in_entry = false, inserted = false;
            for (string ln : lines) {
                if (starts(ln, "define void @") && !inserted) {
                    out.push_back(ln);
                    out.push_back("  %vid.sp = call i32 @dx.op.viewID.i32(i32 " + I(kViewIdOpcode) + ")");
                    out.push_back("  %isv1.sp = icmp ne i32 %vid.sp, 0");
                    inserted = in_entry = true;
                    continue;
                }
                if (in_entry) {
                    std::vector<CB> m;
                    m_cb(ln, &m, true);
                    if (!m.empty() && handles.count(m[0].h)) {
                        const Block* b = view_block(handles[m[0].h]);
                        const string idx = m[0].row;
                        if (idx[0] == '%') refuse("dynamic row index into " + handles[m[0].h]);
                        for (char ch : idx)
                            if (!dig(ch)) strict("a row index not understood: " + idx);
                        const long long row = to_int(idx);
                        if (in_rows(*b, row)) {
                            ++n_redirect;
                            const string tmp = "%row" + I(n_redirect) + ".sp";
                            out.push_back("  " + tmp + ".k = mul i32 %vid.sp, " + I(b->shift));
                            out.push_back("  " + tmp + " = add i32 " + tmp + ".k, " + I(row));
                            ln = ln.substr(0, m[0].rs) + tmp + ln.substr(m[0].re);
                        }
                    }
                }
                if (starts(ln, "}")) in_entry = false;
                out.push_back(ln);
            }
            if (!inserted) refuse("no entry function");
            int n_screen = 0, n_wvp = 0, n_inv = 0, n_remap = 0, n_ndc = 0, n_sky = 0, n_gd = 0;
            out = patch_screen_slots(out, screen, &n_screen);
            string t = join(out);
            if (found.wvp) t = patch_wvp(t, &n_wvp);
            if (!found.inverse.empty()) t = patch_inverse(t, found, &n_inv);
            std::set<string> done;
            for (const auto& kv : screen) done.insert(kv.first);
            t = patch_index_remap(t, done, &n_remap);
            t = patch_ndc(t, &n_ndc);
            t = patch_sky_prev(t, &n_sky);
            t = patch_globals_depth(t, &n_gd);
            if (our_layout && (n_screen || n_inv || n_remap || n_ndc || n_sky || n_gd || r.approx)) {
                string what;
                const std::pair<int, const char*> parts[] = {{n_screen, "screen slots"}, {n_inv, "inverse"}, {n_remap, "index remap"},
                                                             {n_ndc, "NDC"}, {n_sky, "sky"}, {n_gd, "$Globals depth"},
                                                             {r.approx ? 1 : 0, "previous-frame D"}};
                for (const auto& p : parts)
                    if (p.first) what += (what.empty() ? "" : ", ") + string(p.second);
                strict("needs the fork's layout (" + what + ")");
            }
            n_ndc += n_sky + n_gd;
            if (n_redirect + n_screen + n_wvp + n_inv + n_remap + n_ndc == 0) refuse("no view-dependent block");
            n_redirect += n_screen + n_wvp + n_inv + n_remap + n_ndc;
            Lines tl = split(t);
            // the ViewID op's declaration, before the first declare (^attributes (#\d+) = \{ nounwind readnone \})
            string ro;
            for (const auto& ln : tl) {
                Sc c(ln);
                string g;
                if (c.lit("attributes #") && c.num(&g) && c.lit(" = { nounwind readnone }")) {
                    ro = "#" + g;
                    break;
                }
            }
            if (ro.empty()) refuse("no nounwind readnone attribute group");
            for (size_t i = 0; i < tl.size(); ++i)
                if (starts(tl[i], "declare ")) {
                    const string decl = "declare i32 @dx.op.viewID.i32(i32) " + ro;
                    tl.insert(tl.begin() + i, {decl, ""});
                    break;
                }
            // shader model 6.1 and DXIL 1.1 at least
            string dxver, sm;
            for (const auto& ln : tl) {
                Sc c(ln);
                string v;
                if (dxver.empty() && c.lit("!dx.version = !{!") && c.num(&v) && c.lit("}")) dxver = "!" + v;
                Sc d(ln);
                if (sm.empty() && d.lit("!dx.shaderModel = !{!") && d.num(&v) && d.lit("}")) sm = "!" + v;
            }
            if (dxver.empty() || sm.empty()) refuse("no dx.version or dx.shaderModel");
            auto bump = [&](const string& id, bool shader) {
                for (auto& ln : tl) {
                    Sc c(ln);
                    if (!c.lit(id) || !c.lit(" = !{")) continue;
                    if (shader) {
                        if (!(c.lit("!\"vs\"") || c.lit("!\"ps\"")) || !c.lit(", i32 6, i32 ")) continue;
                    } else if (!c.lit("i32 1, i32 ")) {
                        continue;
                    }
                    const size_t a = c.p;
                    string d;
                    if (!c.num(&d) || !c.lit("}")) continue;
                    const long long v = to_int(d);
                    ln = ln.substr(0, a) + I(v > 1 ? v : 1) + ln.substr(a + d.size());
                }
            };
            bump(dxver, false);
            bump(sm, true);
            // the entry's shader flags (extended properties, tag 0) declare ViewID use
            string ep;
            for (const auto& ln : tl) {
                Sc c(ln);
                string v;
                if (c.lit("!dx.entryPoints = !{!") && c.num(&v) && c.lit("}")) {
                    ep = "!" + v;
                    break;
                }
            }
            string props_id;
            if (!ep.empty())
                for (const auto& ln : tl) {  // ^<ep> = !\{[^\n]*, (!\d+|null)\}$
                    if (ln.compare(0, ep.size() + 5, ep + " = !{") != 0 || ln.back() != '}') continue;
                    const string body = ln.substr(ep.size() + 5, ln.size() - ep.size() - 6);
                    const size_t k = body.rfind(", ");
                    if (k == string::npos) continue;
                    const string last = body.substr(k + 2);
                    bool ok = last == "null";
                    if (!ok && last.size() > 1 && last[0] == '!') {
                        ok = true;
                        for (size_t q = 1; q < last.size(); ++q) ok = ok && dig(last[q]);
                    }
                    if (!ok) continue;
                    props_id = last;
                    break;
                }
            if (props_id.empty() || props_id == "null") refuse("no entry properties");
            size_t pl = string::npos;
            for (size_t i = 0; i < tl.size(); ++i)  // ^<props> = !\{(.*)\}$
                if (tl[i].compare(0, props_id.size() + 5, props_id + " = !{") == 0 && tl[i].back() == '}') {
                    pl = i;
                    break;
                }
            bool flags_ok = false;
            if (pl != string::npos) {
                string& ln = tl[pl];
                const size_t b0 = props_id.size() + 5, b1 = ln.size() - 1;
                for (size_t k = ln.find("i32 0, i64 ", b0); k != string::npos && k < b1; k = ln.find("i32 0, i64 ", k + 1)) {
                    Sc c(ln, k + 11);
                    string d;
                    if (!c.num(&d) || c.p > b1) continue;
                    const unsigned long long v = std::stoull(d) | kViewIdFlag;
                    ln = ln.substr(0, k + 11) + std::to_string(v) + ln.substr(c.p);
                    flags_ok = true;
                    break;
                }
            }
            if (!flags_ok) refuse("no shader flags entry in the entry properties");
            t = join(tl);
            if (r.approx) t = patch_prev_world(t);
            r.ok = true;
            r.text = std::move(t);
            r.n = n_redirect;
        } catch (const Refused& e) {
            if (e.why != "no view-dependent block") throw;
            r.refused = e.why;  // (used as it is: still checked below)
        }
        strict_checks(split(text));
    } catch (const Refused& e) {
        r = Rewrite{};
        r.refused = e.why;
    } catch (const std::bad_alloc&) {
        throw;  // (out of memory: not the shader's fault; the converter exits without a result)
    } catch (const std::exception& e) {
        r = Rewrite{};
        r.refused = string("strict: ") + e.what();
    }
    return r;
}

}  // namespace vi
