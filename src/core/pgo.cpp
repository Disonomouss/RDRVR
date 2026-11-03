#include "core/pgo.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/log.h"

namespace rdrvr::pgo {
namespace {

constexpr wchar_t kModule[] = L"PedGunfightOverhaul.red";
constexpr wchar_t kIni[] = L"PedGunfightOverhaul.ini";
// v1.1.5 (PedGunfightOverhaul.red sha256 e08db1c1...)
constexpr uint32_t kTimeDateStamp = 0x6a074880u, kSizeOfImage = 0x16000u;

// v1.1.5's settings: each global (RVA) and the instruction of its script that reads it (RIP-relative, so the bytes
// hold the global's place): all six as here, else another build. Its settings table (FUN_180001160) gives the types
// and the ranges (Clamp below).
enum Var { kAccuracy, kPedHealth, kPlayerHealth, kCripple, kHeadshot, kInGame, kVars };
struct VarSite {
    uint32_t rva;
    uint32_t insn_rva;
    uint8_t len;
    const char* insn;
};
constexpr VarSite kSite[kVars] = {
    {0x120e8, 0x4890, 8, "\xf3\x0f\x10\x05\x50\xd8\x00\x00"},  // movss xmm0, [PedAccuracy] (float)
    {0x120e4, 0x483c, 6, "\x8b\x05\xa2\xd8\x00\x00"},          // mov eax, [PedHealth] (int)
    {0x120e0, 0x479c, 6, "\x8b\x05\x3e\xd9\x00\x00"},          // mov eax, [PlayerHealth] (int)
    {0x120dc, 0x4f10, 7, "\x44\x8b\x3d\xc5\xd1\x00\x00"},      // mov r15d, [EnableCrippleAtHealth] (int)
    {0x12be8, 0x4e30, 7, "\x80\x3d\xb1\xdd\x00\x00\x00"},      // cmp byte [HeadshotAlwaysKills], 0 (bool)
    {0x120d8, 0x5062, 7, "\x80\x3d\x6f\xd0\x00\x00\x00"},      // cmp byte [InGameControls], 0 (bool)
};
const char* const kKey[kVars] = {"PedAccuracy", "PedHealth", "PlayerHealth", "EnableCrippleAtHealth", "HeadshotAlwaysKills", "InGameControls"};
const char* const kSection[kVars] = {"GENERAL", "GENERAL", "GENERAL", "GENERAL", "GENERAL", "KEYBINDS"};

std::mutex g_mu;
uintptr_t g_checked = 0;  // the module base last checked, and its verdict
bool g_checked_ok = false;
Values g_pending;         // not live: the values its ini gets on Save
Values g_file;            // its ini's, as last read or saved
bool g_file_read = false;
FILETIME g_file_time{};   // its ini's write time then (its own Save rewrites it)
double g_file_look_ms = -1e9, g_installed_ms = -1e9;
bool g_installed = false;

bool raw_read(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool raw_write(uintptr_t a, const void* in, size_t n) {
    __try {
        std::memcpy(reinterpret_cast<void*>(a), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool verify(uintptr_t base) {
    int32_t lfanew = 0;
    uint32_t sig = 0, stamp = 0, size = 0;
    if (!raw_read(base + 0x3c, &lfanew, 4) || lfanew <= 0 || lfanew > 0x1000 || !raw_read(base + lfanew, &sig, 4) || sig != 0x4550 ||
        !raw_read(base + lfanew + 8, &stamp, 4) || !raw_read(base + lfanew + 0x50, &size, 4))
        return false;
    if (stamp != kTimeDateStamp || size != kSizeOfImage) {
        log::info("[pgo] PedGunfightOverhaul.red is not v1.1.5 (TimeDateStamp %#x, SizeOfImage %#x): the Gunfights page edits its ini only", stamp, size);
        return false;
    }
    for (const VarSite& s : kSite) {
        char b[16];
        if (!raw_read(base + s.insn_rva, b, s.len) || std::memcmp(b, s.insn, s.len) != 0) {
            log::info("[pgo] PedGunfightOverhaul.red v1.1.5's code differs at +%#x: the Gunfights page edits its ini only", s.insn_rva);
            return false;
        }
    }
    return true;
}

// under g_mu: its module's base while loaded and v1.1.5, else 0 (*why: the link)
uintptr_t live_base(Link* why = nullptr) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(kModule));
    if (!base) {
        if (why) *why = Link::kNotLoaded;
        return 0;
    }
    if (base != g_checked) {
        g_checked = base;
        g_checked_ok = verify(base);
        if (g_checked_ok) log::info("[pgo] PedGunfightOverhaul.red v1.1.5 loaded at %p: the Gunfights page changes its settings live", reinterpret_cast<void*>(base));
    }
    if (why) *why = g_checked_ok ? Link::kLive : Link::kOtherBuild;
    return g_checked_ok ? base : 0;
}

void clamp(Values* v) {
    if (!std::isfinite(v->ped_accuracy)) v->ped_accuracy = 100.0f;
    v->ped_accuracy = std::round(std::fmin(200.0f, std::fmax(0.0f, v->ped_accuracy)) * 100.0f) / 100.0f;
    auto c = [](int* x, int hi) { *x = *x < 0 ? 0 : *x > hi ? hi : *x; };
    c(&v->ped_health, 1000000);
    c(&v->player_health, 1000000);
    c(&v->cripple_at, 100);
}

bool read_mem(uintptr_t base, Values* v) {
    uint8_t hs = 0, ig = 0;
    bool ok = raw_read(base + kSite[kAccuracy].rva, &v->ped_accuracy, 4) && raw_read(base + kSite[kPedHealth].rva, &v->ped_health, 4) &&
              raw_read(base + kSite[kPlayerHealth].rva, &v->player_health, 4) && raw_read(base + kSite[kCripple].rva, &v->cripple_at, 4) &&
              raw_read(base + kSite[kHeadshot].rva, &hs, 1) && raw_read(base + kSite[kInGame].rva, &ig, 1);
    v->headshot_kills = hs != 0;
    v->in_game_controls = ig != 0;
    return ok;
}

std::string text_of(const Values& v, int i) {
    char b[32];
    switch (i) {
        case kAccuracy: std::snprintf(b, sizeof(b), "%.2f", v.ped_accuracy); break;
        case kPedHealth: std::snprintf(b, sizeof(b), "%d", v.ped_health); break;
        case kPlayerHealth: std::snprintf(b, sizeof(b), "%d", v.player_health); break;
        case kCripple: std::snprintf(b, sizeof(b), "%d", v.cripple_at); break;
        case kHeadshot: return v.headshot_kills ? "true" : "false";
        default: return v.in_game_controls ? "true" : "false";
    }
    return b;
}

// the fields of b that differ from a, written into its memory (a field its own keys changed meanwhile is kept)
void write_mem(uintptr_t base, const Values& a, const Values& b) {
    const uint8_t hs = b.headshot_kills ? 1 : 0, ig = b.in_game_controls ? 1 : 0;
    const void* src[kVars] = {&b.ped_accuracy, &b.ped_health, &b.player_health, &b.cripple_at, &hs, &ig};
    const size_t n[kVars] = {4, 4, 4, 4, 1, 1};
    for (int i = 0; i < kVars; ++i) {
        const std::string was = text_of(a, i), now = text_of(b, i);
        if (was == now) continue;
        const bool ok = raw_write(base + kSite[i].rva, src[i], n[i]);
        log::info("[pgo] %s %s -> %s%s", kKey[i], was.c_str(), now.c_str(), ok ? "" : " (NOT WRITTEN: its memory not writable)");
    }
}

bool same(const Values& a, const Values& b) {
    for (int i = 0; i < kVars; ++i)
        if (text_of(a, i) != text_of(b, i)) return false;
    return true;
}

std::wstring ini_path() {
    wchar_t p[MAX_PATH];
    return log::path_in_game_dir(kIni, p, MAX_PATH);
}

bool read_file(std::string* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, ini_path().c_str(), L"rb") != 0 || !f) return false;
    char b[4096];
    size_t n;
    out->clear();
    while ((n = fread(b, 1, sizeof(b), f)) > 0) out->append(b, n);
    fclose(f);
    return true;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), z = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, z - a + 1);
}

// one ini line: its section header (into *section), or its key and the value's span (before a ';' or '#' comment)
bool key_line(const std::string& line, std::string* section, std::string* key, size_t* v0, size_t* v1) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') return false;
    if (t[0] == '[') {
        const size_t e = t.find(']');
        *section = t.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        return false;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    *key = trim(line.substr(0, eq));
    size_t a = eq + 1, c = line.find_first_of(";#", a);
    if (c == std::string::npos) c = line.size();
    while (a < c && (line[a] == ' ' || line[a] == '\t')) ++a;
    size_t z = c;
    while (z > a && (line[z - 1] == ' ' || line[z - 1] == '\t' || line[z - 1] == '\r' || line[z - 1] == '\n')) --z;  // (r263: a key without a comment kept its line end)
    *v0 = a;
    *v1 = z;
    return true;
}

int var_of(const std::string& section, const std::string& key) {
    for (int i = 0; i < kVars; ++i)
        if (_stricmp(section.c_str(), kSection[i]) == 0 && _stricmp(key.c_str(), kKey[i]) == 0) return i;
    return -1;
}

// its parser's values (true/yes/1, false/no/0); keys not in the file keep v's
void parse(const std::string& text, Values* v) {
    std::istringstream in(text);
    std::string line, section, key;
    size_t a = 0, z = 0;
    while (std::getline(in, line)) {
        if (!key_line(line, &section, &key, &a, &z)) continue;
        const int i = var_of(section, key);
        if (i < 0) continue;
        const std::string val = line.substr(a, z - a);
        const bool truth = _stricmp(val.c_str(), "true") == 0 || _stricmp(val.c_str(), "yes") == 0 || val == "1";
        switch (i) {
            case kAccuracy: v->ped_accuracy = std::strtof(val.c_str(), nullptr); break;
            case kPedHealth: v->ped_health = std::atoi(val.c_str()); break;
            case kPlayerHealth: v->player_health = std::atoi(val.c_str()); break;
            case kCripple: v->cripple_at = std::atoi(val.c_str()); break;
            case kHeadshot: v->headshot_kills = truth; break;
            default: v->in_game_controls = truth; break;
        }
    }
    clamp(v);
}

FILETIME file_time() {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(ini_path().c_str(), GetFileExInfoStandard, &d)) return FILETIME{};
    return d.ftLastWriteTime;
}

// under g_mu: its ini read once, and again when its write time changed (its own Save, a hand edit; looked at each 1 s)
void ensure_read() {
    const double now = log::now_ms();
    if (g_file_read && now - g_file_look_ms < 1000.0) return;
    g_file_look_ms = now;
    const FILETIME t = file_time();
    if (g_file_read && CompareFileTime(&t, &g_file_time) == 0) return;
    std::string text;
    Values v;  // its defaults (as its .data's) for a missing file or key
    if (read_file(&text)) parse(text, &v);
    const bool first = !g_file_read;
    if (first || same(g_pending, g_file)) g_pending = v;  // (pending changes not saved are kept)
    g_file = v;
    g_file_time = t;
    g_file_read = true;
    if (first) log::info("[pgo] its ini: PedAccuracy %.2f PedHealth %d PlayerHealth %d EnableCrippleAtHealth %d HeadshotAlwaysKills %d InGameControls %d",
                         v.ped_accuracy, v.ped_health, v.player_health, v.cripple_at, v.headshot_kills ? 1 : 0, v.in_game_controls ? 1 : 0);
}

// its ini with v's values: each key's value replaced in place (its comments, other keys and line ends kept; a shorter
// value padded so its comment stays in its column), a missing key added under its section (a missing section at the end)
bool write_ini(const Values& v, std::string* err) {
    std::string text;
    read_file(&text);
    const std::string nl = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
    std::string out, section, key;
    bool done[kVars] = {};
    size_t pos = 0;
    size_t header_end[2] = {std::string::npos, std::string::npos};  // GENERAL, KEYBINDS: where a missing key goes
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        e = e == std::string::npos ? text.size() : e + 1;
        std::string line = text.substr(pos, e - pos);
        pos = e;
        size_t a = 0, z = 0;
        if (key_line(line, &section, &key, &a, &z)) {
            const int i = var_of(section, key);
            if (i >= 0 && !done[i]) {
                std::string val = text_of(v, i);
                const bool comment = line.find_first_of(";#", z) != std::string::npos;
                if (comment && val.size() < z - a) val.append(z - a - val.size(), ' ');
                line = line.substr(0, a) + val + line.substr(z);
                done[i] = true;
            }
        }
        out += line;
        if (trim(line).size() && trim(line)[0] == '[') {
            const int s = _stricmp(section.c_str(), "GENERAL") == 0 ? 0 : _stricmp(section.c_str(), "KEYBINDS") == 0 ? 1 : -1;
            if (s >= 0 && header_end[s] == std::string::npos) header_end[s] = out.size();
        }
    }
    for (int s = 1; s >= 0; --s) {  // (KEYBINDS first: an insert into GENERAL above it moves its place)
        std::string add;
        for (int i = 0; i < kVars; ++i)
            if (!done[i] && std::strcmp(kSection[i], s ? "KEYBINDS" : "GENERAL") == 0) add += std::string(kKey[i]) + " = " + text_of(v, i) + nl;
        if (add.empty()) continue;
        if (header_end[s] != std::string::npos) {
            if (header_end[s] > 0 && out[header_end[s] - 1] != '\n') add = nl + add;
            out.insert(header_end[s], add);
            if (s == 1 && header_end[0] != std::string::npos && header_end[0] > header_end[1]) header_end[0] += add.size();
        } else {
            if (!out.empty() && out.back() != '\n') out += nl;
            out += std::string("[") + (s ? "KEYBINDS" : "GENERAL") + "]" + nl + add;
        }
    }
    const std::wstring path = ini_path(), tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) {
        if (err) *err = "could not write PedGunfightOverhaul.ini.tmp in the game folder";
        return false;
    }
    const bool wrote = fwrite(out.data(), 1, out.size(), f) == out.size();
    if (fclose(f) != 0 || !wrote || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        if (err) *err = "could not replace PedGunfightOverhaul.ini";
        return false;
    }
    return true;
}

}  // namespace

bool installed() {
    std::lock_guard lock(g_mu);
    const double now = log::now_ms();
    if (now - g_installed_ms >= 2000.0) {
        g_installed_ms = now;
        wchar_t p[MAX_PATH];
        const DWORD a = GetFileAttributesW(log::path_in_game_dir(kModule, p, MAX_PATH));
        const bool was = g_installed;
        g_installed = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        if (g_installed != was) log::info("[pgo] Ped Gunfight Overhaul %s: the Gunfights page %s", g_installed ? "installed" : "not installed",
                                          g_installed ? "shown" : "hidden");
    }
    return g_installed;
}

Link link() {
    std::lock_guard lock(g_mu);
    Link l = Link::kNotLoaded;
    live_base(&l);
    return l;
}

Values get() {
    std::lock_guard lock(g_mu);
    ensure_read();
    Values v;
    if (const uintptr_t base = live_base(); base && read_mem(base, &v)) return v;
    return g_pending;
}

void set(const Values& in) {
    Values v = in;
    clamp(&v);
    std::lock_guard lock(g_mu);
    ensure_read();
    Values now;
    if (const uintptr_t base = live_base(); base && read_mem(base, &now)) {
        write_mem(base, now, v);
        return;
    }
    for (int i = 0; i < kVars; ++i)
        if (text_of(g_pending, i) != text_of(v, i)) log::info("[pgo] %s %s -> %s (for its ini: not loaded live)", kKey[i], text_of(g_pending, i).c_str(), text_of(v, i).c_str());
    g_pending = v;
}

bool save(std::string* err) {
    std::lock_guard lock(g_mu);
    ensure_read();
    Values v = g_pending;
    if (const uintptr_t base = live_base(); base) read_mem(base, &v);
    if (!write_ini(v, err)) {
        log::warn("[pgo] save: %s", err ? err->c_str() : "failed");
        return false;
    }
    g_file = g_pending = v;
    g_file_time = file_time();
    log::info("[pgo] saved into PedGunfightOverhaul.ini (as its Numpad 5)");
    return true;
}

bool reload(std::string* err) {
    std::lock_guard lock(g_mu);
    std::string text;
    if (!read_file(&text)) {
        if (err) *err = "PedGunfightOverhaul.ini not found in the game folder";
        log::warn("[pgo] reload: no PedGunfightOverhaul.ini");
        return false;
    }
    Values now = g_pending;
    const uintptr_t base = live_base();
    if (base) read_mem(base, &now);
    Values v = now;
    parse(text, &v);
    if (base) write_mem(base, now, v);
    g_file = g_pending = v;
    g_file_time = file_time();
    g_file_read = true;
    log::info("[pgo] reloaded from PedGunfightOverhaul.ini (as its Numpad 0)%s", base ? "" : " (for its ini: not loaded live)");
    return true;
}

bool unsaved() {
    const Values v = get();
    std::lock_guard lock(g_mu);
    return !same(v, g_file);
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string w, a, b, err;
    in >> w;  // "pgo"
    if (in >> a) {
        if (a == "save" && !save(&err)) return "ERROR " + err;
        if (a == "reload" && !reload(&err)) return "ERROR " + err;
        if (a == "set" && in >> a >> b) {
            Values v = get();
            if (a == "accuracy") v.ped_accuracy = std::strtof(b.c_str(), nullptr);
            else if (a == "pedhealth") v.ped_health = std::atoi(b.c_str());
            else if (a == "playerhealth") v.player_health = std::atoi(b.c_str());
            else if (a == "cripple") v.cripple_at = std::atoi(b.c_str());
            else if (a == "headshot") v.headshot_kills = b == "on" || b == "1";
            else if (a == "keys") v.in_game_controls = b == "on" || b == "1";
            else return "ERROR pgo set accuracy|pedhealth|playerhealth|cripple|headshot|keys <value>";
            set(v);
        }
    }
    const bool inst = installed();
    const Link l = link();
    const Values v = get();
    const bool dirty = unsaved();
    Values f;
    {
        std::lock_guard lock(g_mu);
        f = g_file;
    }
    char o[600];
    std::snprintf(o, sizeof(o),
                  "pgo installed %d link %s | PedAccuracy %.2f PedHealth %d PlayerHealth %d EnableCrippleAtHealth %d HeadshotAlwaysKills %d "
                  "InGameControls %d | unsaved %d | ini PedAccuracy %.2f PedHealth %d PlayerHealth %d EnableCrippleAtHealth %d HeadshotAlwaysKills %d "
                  "InGameControls %d",
                  inst ? 1 : 0, l == Link::kLive ? "live" : l == Link::kOtherBuild ? "other-build" : "not-loaded", v.ped_accuracy, v.ped_health,
                  v.player_health, v.cripple_at, v.headshot_kills ? 1 : 0, v.in_game_controls ? 1 : 0, dirty ? 1 : 0, f.ped_accuracy, f.ped_health,
                  f.player_health, f.cripple_at, f.headshot_kills ? 1 : 0, f.in_game_controls ? 1 : 0);
    return o;
}

}  // namespace rdrvr::pgo
