#include "core/config.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
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
        std::fputs("; RDRVR user settings, written by the in-headset menu. Never replaced by a release or a test.\r\n", f);
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

void load() {
    wchar_t ini[MAX_PATH];
    log::path_in_game_dir(L"RDRVR.ini", ini, MAX_PATH);

    PWSTR local = nullptr;
    if (!g_user_override && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        std::swprintf(g_user_path, MAX_PATH, L"%s\\RDRVR\\RDRVR.user.ini", local);
        CoTaskMemFree(local);
    }

    AcquireSRWLockExclusive(&g_lock);
    g_values.clear();
    g_echoed.clear();  // re-echo after a reload so the log shows what the new values are
    parse_file(ini, "ini");
    if (g_user_path[0]) parse_file(g_user_path, "user");
    ReleaseSRWLockExclusive(&g_lock);
}

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

}  // namespace rdrvr::config
