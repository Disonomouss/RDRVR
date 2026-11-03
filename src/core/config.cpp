#include "core/config.h"

#include <cstring>

#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"

namespace rdrvr::config {
namespace {

struct Value {
    std::string text;
    const char* source;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::map<std::string, Value> g_values;  // "section.key" (lower case) -> value
std::map<std::string, std::string> g_shipped;  // the shipped RDRVR.ini's own values (a reset to the defaults)
std::set<std::string> g_echoed;
wchar_t g_user_path[MAX_PATH] = {};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void parse_file(const wchar_t* path, const char* source) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
        log::info("[cfg] %s: %ls not present", source, path);
        return;
    }
    std::string section;
    char line[1024];
    int keys = 0;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = lower(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(s.substr(0, eq)));
        std::string val = trim(s.substr(eq + 1));
        if (size_t sc = val.find(" ;"); sc != std::string::npos) val = trim(val.substr(0, sc));
        g_values[section + "." + key] = {val, source};
        if (!std::strcmp(source, "ini")) g_shipped[section + "." + key] = val;
        ++keys;
    }
    std::fclose(f);
    log::info("[cfg] %s: %ls (%d keys)", source, path, keys);
}

bool lookup(const char* section, const char* key, std::string& out, const char*& source) {
    std::string k = lower(section) + "." + lower(key);
    AcquireSRWLockShared(&g_lock);
    auto it = g_values.find(k);
    bool found = it != g_values.end();
    if (found) {
        out = it->second.text;
        source = it->second.source;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void echo(const char* section, const char* key, const std::string& shown, const char* source) {
    std::string k = lower(section) + "." + lower(key) + "=" + shown;
    AcquireSRWLockExclusive(&g_lock);
    bool first = g_echoed.insert(k).second;
    ReleaseSRWLockExclusive(&g_lock);
    if (first) log::info("[cfg] read %s.%s = %s (%s)", section, key, shown.c_str(), source);
}

}  // namespace

const wchar_t* user_ini_path() { return g_user_path; }

bool g_user_override = false;

void set_user_path(const wchar_t* path) {
    wcsncpy_s(g_user_path, path, _TRUNCATE);
    g_user_override = true;
    log::warn("[cfg] user file for this run: %ls (test)", g_user_path);
    load();
}

bool set(const char* section, const char* key, const std::string& value) {
    if (!g_user_path[0]) return false;
    // one writer at a time: the menu, the test channel and the grips' saves each read, rewrite and rename the user file
    // through one .tmp (run 7 item 6's review)
    static std::mutex s_write;
    std::lock_guard write_lock(s_write);
    // read the user file's lines; replace the key's line in its section, or add it (and the section) at the end
    std::vector<std::string> lines;
    if (FILE* f = nullptr; _wfopen_s(&f, g_user_path, L"rb") == 0 && f) {
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            lines.push_back(s);
        }
        std::fclose(f);
    }
    std::string want_section = lower(section), want_key = lower(key), cur;
    int section_end = -1, key_line = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string s = trim(lines[i]);
        if (!s.empty() && s.front() == '[' && s.back() == ']') {
            cur = lower(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        if (cur != want_section) continue;
        section_end = i;
        size_t eq = s.find('=');
        if (!s.empty() && s[0] != ';' && s[0] != '#' && eq != std::string::npos && lower(trim(s.substr(0, eq))) == want_key)
            key_line = i;
    }
    std::string entry = std::string(key) + "=" + value;
    if (key_line >= 0) {
        lines[key_line] = entry;
    } else if (section_end >= 0) {
        lines.insert(lines.begin() + section_end + 1, entry);
    } else {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        lines.push_back(std::string("[") + section + "]");
        lines.push_back(entry);
    }
    std::wstring dir = g_user_path;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring tmp = std::wstring(g_user_path) + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    if (lines.empty() || trim(lines.front()).rfind(";", 0) != 0)
        std::fputs("; RDRVR user settings, written by the in-headset menu. A release that resets the settings keeps this file "
                   "as RDRVR.user.ini.before-<version>.bak.\r\n", f);
    for (const auto& l : lines) {
        std::fputs(l.c_str(), f);
        std::fputs("\r\n", f);
    }
    bool ok = std::fclose(f) == 0 && MoveFileExW(tmp.c_str(), g_user_path, MOVEFILE_REPLACE_EXISTING);
    std::string old;
    const char* src = "default";
    lookup(section, key, old, src);
    if (ok) {
        AcquireSRWLockExclusive(&g_lock);
        g_values[want_section + "." + want_key] = {value, "user"};
        ReleaseSRWLockExclusive(&g_lock);
    }
    log::info("[cfg] set %s.%s = %s (was %s from %s) in %ls: %s", section, key, value.c_str(), old.c_str(), src, g_user_path,
              ok ? "written" : "NOT WRITTEN");
    return ok;
}

namespace {
// (GOAL-single-pass-default.md item 3) [Stereo] SinglePass: 1 (the shipped default) supplies the single-pass stack's
// keys wherever neither file sets them (the user's own lines win, the hand-made folder's InstancedShaders included);
// the shaders come from the converter on the player's own game files (InstancedShadersAuto) unless a folder is named.
// Without the converter or its DXC DLL (and no folder) nothing is supplied: two-pass, said in the log. 0 (the two-pass
// switch) turns the stack's master keys off whatever the files say. The run 26 / r309 stack: the soak 99.85 % cut.
const char* const kSinglePassSrc = "single pass";
const char* const kTwoPassSrc = "two-pass switch";
struct Implied {
    const char* key;
    const char* value;
};
constexpr Implied kSinglePassStack[] = {
    {"prepassarrays", "1"},      {"prepassviews", "2"},          {"prepassunioncull", "1"},     {"prepassinstanced", "1"},
    {"prepasswalkcut", "1"},     {"prepasswvpmirror", "1"},      {"prepassinstprevmirror", "1"}, {"prepassinstprevk", "64"},
    {"prepasscutvelocity", "4"}, {"prepasscutholdstarted", "1"}, {"prepasscutemptydraws", "1"}, {"prepassinstprevnotaa", "1"},
};
constexpr const char* kTwoPassOff[] = {"prepassarrays", "prepassviews", "prepassinstanced", "prepasswalkcut"};
std::atomic<int> g_sp_state{-1};  // single_pass_state(): -1 no key, 0 two-pass, 1 single pass, 2 no converter, 3 the files' 0s
std::string g_sp_off_key;          // (state 3, under g_lock) the user's key that turns it off
bool file_exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
void single_pass_defaults() {
    std::string sp, folder, exe, dxc;
    bool sp_set = false;
    {
        AcquireSRWLockShared(&g_lock);
        auto get = [](const char* k, std::string& out) {
            auto it = g_values.find(k);
            if (it == g_values.end()) return false;
            out = trim(it->second.text);
            return true;
        };
        sp_set = get("stereo.singlepass", sp);
        get("stereo.instancedshaders", folder);
        get("stereo.instancedshadersconverter", exe);
        get("stereo.instancedshadersdxc", dxc);
        ReleaseSRWLockShared(&g_lock);
    }
    g_sp_state = -1;
    if (!sp_set) return;  // (no key: the code's own defaults, as before)
    if (sp == "0") {
        AcquireSRWLockExclusive(&g_lock);
        for (const char* k : kTwoPassOff) g_values[std::string("stereo.") + k] = {"0", kTwoPassSrc};
        // (the review) no converter and no folder's twins either: nothing of single pass runs in two-pass
        g_values["stereo.instancedshadersauto"] = {"0", kTwoPassSrc};
        g_values["stereo.instancedshaders"] = {"", kTwoPassSrc};
        ReleaseSRWLockExclusive(&g_lock);
        g_sp_state = 0;
        log::info("[cfg] [Stereo] SinglePass=0: two-pass (PrepassArrays, PrepassViews, PrepassInstanced and PrepassWalkCut off)");
        return;
    }
    if (sp != "1") return;
    // (the review) a master key the files turn off wins over the supplied ones (an older launcher's untick wrote each 0):
    // then it is two-pass, said so, and nothing is supplied (no converter started for nothing)
    {
        std::string off;
        AcquireSRWLockShared(&g_lock);
        for (const char* k : kTwoPassOff) {
            auto it = g_values.find(std::string("stereo.") + k);
            if (it != g_values.end() && (trim(it->second.text) == "0" || trim(it->second.text).empty())) {
                off = it->first.substr(7);
                break;
            }
        }
        ReleaseSRWLockShared(&g_lock);
        if (!off.empty()) {
            AcquireSRWLockExclusive(&g_lock);
            g_sp_off_key = off;
            ReleaseSRWLockExclusive(&g_lock);
            g_sp_state = 3;
            log::warn("[cfg] [Stereo] SinglePass=1 but your settings file sets %s=0: two-pass (the launcher's Single pass box writes the "
                      "single-pass keys again)", off.c_str());
            return;
        }
    }
    const bool by_folder = !folder.empty();
    if (!by_folder) {  // the converter as vi_auto.cpp finds it
        const std::wstring e = exe.empty() ? std::wstring(log::game_dir()) + L"RDRVR_Converter\\rdrvr_vi_convert.exe" : widen(exe);
        const std::wstring d = dxc.empty() ? e.substr(0, e.find_last_of(L"\\/") + 1) + L"rdrvr_dxcompiler.dll" : widen(dxc);
        const bool he = file_exists(e), hd = file_exists(d);
        if (!he || !hd) {
            g_sp_state = 2;
            log::warn("[cfg] [Stereo] SinglePass=1 but the shader converter is missing (%ls%s%ls): two-pass", he ? L"" : e.c_str(),
                      he || hd ? "" : ", ", hd ? L"" : d.c_str());
            return;
        }
    }
    int n = 0;
    AcquireSRWLockExclusive(&g_lock);
    auto put = [&](const char* k, const char* v) {
        const std::string key = std::string("stereo.") + k;
        if (g_values.count(key)) return;
        g_values[key] = {v, kSinglePassSrc};
        ++n;
    };
    for (const Implied& i : kSinglePassStack) put(i.key, i.value);
    if (!by_folder) {  // (the pending hold needs the converter's twins: pso's region table)
        put("instancedshadersauto", "1");
        put("prepasscutholdpending", "1");
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_sp_state = 1;
    log::info("[cfg] [Stereo] SinglePass=1: single pass (%d keys supplied, the shaders %s)", n, by_folder ? "from the folder" : "converted on this PC");
}
void load_files();
}  // namespace

void load() {
    load_files();
    single_pass_defaults();
}
int single_pass_state() { return g_sp_state.load(std::memory_order_relaxed); }
std::string single_pass_off_key() {
    AcquireSRWLockShared(&g_lock);
    std::string k = g_sp_off_key;
    ReleaseSRWLockShared(&g_lock);
    return k;
}

namespace {
void load_files() {
    wchar_t ini[MAX_PATH];
    log::path_in_game_dir(L"RDRVR.ini", ini, MAX_PATH);

    PWSTR local = nullptr;
    if (!g_user_override && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        std::swprintf(g_user_path, MAX_PATH, L"%s\\RDRVR\\RDRVR.user.ini", local);
        CoTaskMemFree(local);
    }

    AcquireSRWLockExclusive(&g_lock);
    g_values.clear();
    g_shipped.clear();
    g_echoed.clear();  // re-echo after a reload so the log shows what the new values are
    parse_file(ini, "ini");
    if (g_user_path[0]) parse_file(g_user_path, "user");
    ReleaseSRWLockExclusive(&g_lock);
    // 2026-10-13 (the user: "For the next release, can you have my current ini settings overwrite the users. I worry
    // there are people who have played since an early version whose old settings keep on carrying over"): a release
    // whose shipped ini names a [Compat] SettingsReset tag starts each player from its defaults once: a user file not
    // made under that tag is moved aside (kept as RDRVR.user.ini.before-<tag>.bak) and a new one records the tag. The
    // repo's own ini leaves it empty, so a developer's deploy never resets.
    std::string tag, mine;
    {
        AcquireSRWLockShared(&g_lock);
        auto it = g_shipped.find("compat.settingsreset");
        if (it != g_shipped.end()) tag = it->second;
        auto mt = g_values.find("compat.settingsreset");
        if (mt != g_values.end() && !std::strcmp(mt->second.source, "user")) mine = mt->second.text;
        ReleaseSRWLockShared(&g_lock);
    }
    if (tag.empty() || !g_user_path[0] || mine == tag) return;
    if (GetFileAttributesW(g_user_path) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring wtag(tag.begin(), tag.end());
        std::wstring bak = std::wstring(g_user_path) + L".before-" + wtag + L".bak";
        for (int n = 1; GetFileAttributesW(bak.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n)
            bak = std::wstring(g_user_path) + L".before-" + wtag + L"." + std::to_wstring(n) + L".bak";
        if (!MoveFileExW(g_user_path, bak.c_str(), MOVEFILE_WRITE_THROUGH)) {
            log::warn("[cfg] the settings reset of %s: %ls not moved aside (error %lu): the old settings kept", tag.c_str(), g_user_path,
                      GetLastError());
            return;
        }
        log::info("[cfg] the settings reset of %s: the old user settings kept as %ls; this release's defaults from now on", tag.c_str(),
                  bak.c_str());
        AcquireSRWLockExclusive(&g_lock);
        g_values.clear();
        g_shipped.clear();
        g_echoed.clear();
        parse_file(ini, "ini");
        ReleaseSRWLockExclusive(&g_lock);
    }
    set("Compat", "SettingsReset", tag);  // (a new user file: this tag's, so the reset is done once)
}
}  // namespace

bool get_bool(const char* section, const char* key, bool def) {
    std::string v;
    const char* src = "default";
    bool r = def;
    if (lookup(section, key, v, src)) {
        std::string l = lower(v);
        r = (l == "1" || l == "true" || l == "yes" || l == "on");
    }
    echo(section, key, r ? "1" : "0", src);
    return r;
}

int get_int(const char* section, const char* key, int def) {
    std::string v;
    const char* src = "default";
    int r = def;
    if (lookup(section, key, v, src)) r = static_cast<int>(std::strtol(v.c_str(), nullptr, 0));
    echo(section, key, std::to_string(r), src);
    return r;
}

float get_float(const char* section, const char* key, float def) {
    std::string v;
    const char* src = "default";
    float r = def;
    if (lookup(section, key, v, src)) r = std::strtof(v.c_str(), nullptr);
    char shown[32];
    std::snprintf(shown, sizeof(shown), "%g", r);
    echo(section, key, shown, src);
    return r;
}

std::string get_string(const char* section, const char* key, const char* def) {
    std::string v;
    const char* src = "default";
    std::string r = def;
    if (lookup(section, key, v, src)) r = v;
    echo(section, key, r, src);
    return r;
}
std::string get_shipped(const char* section, const char* key, const char* def) {
    const std::string k = lower(section) + "." + lower(key);
    AcquireSRWLockShared(&g_lock);
    auto it = g_shipped.find(k);
    std::string r = it != g_shipped.end() ? it->second : std::string(def);
    ReleaseSRWLockShared(&g_lock);
    return r;
}

}  // namespace rdrvr::config
