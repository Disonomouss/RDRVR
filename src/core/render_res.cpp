#include "core/render_res.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <shlobj.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/anchors.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/hooks.h"
#include "core/log.h"

#pragma comment(lib, "dxgi.lib")

namespace rdrvr::render_res {
namespace {
using anchors::Id;

// The fixed sizes: 16:9 at a headset's eye height (the panel's, or what a runtime asks for at its default), W = H x 16/9
// rounded to even. The eye-shape mode renders each eye at most this tall, so a height at or above the runtime's
// recommended one gives the eye its full density (Automatic takes that height from the last session).
struct Preset {
    uint32_t h;
    const char* names;
};
const Preset kPresets[] = {
    {1600, "Valve Index"},
    {1920, "Quest 2, Quest 3S, Quest Pro"},
    {2040, "PlayStation VR2"},
    {2160, "Pico 4, Reverb G2, Steam Frame"},
    {2208, "Quest 3"},
    {2240, "Valve Index at SteamVR's 100%"},
    {2304, "Virtual Desktop Medium"},
    {2448, "Vive Pro 2, Vive Focus 3"},
    {2560, "Bigscreen Beyond"},
    {2688, "Virtual Desktop High"},
    {2880, "Pimax Crystal, Virtual Desktop Ultra"},
    {3264, "Virtual Desktop Godlike"},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
constexpr int kOff = 0, kAuto = 1, kFirstPreset = 2, kChoiceCount = kFirstPreset + kPresetCount;
constexpr uint32_t kMaxW = 7680, kMaxH = 4320;  // the game's mode label is 12 bytes ("%d x %d"): W <= 9999 keeps it whole
constexpr double kBytesPerPixel = 225.0;        // the game's full-size targets (~129 B/px at 3840x2160) and the mod's (~96)
constexpr double kVramShare = 0.35;             // of the adapter's dedicated memory, at most
constexpr double kSentinelMs = 120000.0;        // two minutes at the size (or a clean exit) clear a start's sentinel

uint32_t width_of(uint32_t h) { return 2u * static_cast<uint32_t>(std::lround(h * 8.0 / 9.0)); }

char g_label[kChoiceCount][96];

// This start (the bootstrap thread writes them before the hooks run; read by the game's threads after)
std::atomic<bool> g_armed{false};    // the sysParams written, the hooks live
std::atomic<bool> g_took{false};     // the device init took the size (the window create saw it)
std::atomic<bool> g_defer{false};    // another build: armed after anchors::verify(), if still in time
uint32_t g_w = 0, g_h = 0;           // the size armed
int g_player_w = 0, g_player_h = 0;  // the xml's size (the window, and what the xml keeps)
bool g_fullscreen = false, g_window_kept = false;
double g_arm_ms = 0, g_window_ms = 0;
char g_why[160] = "";                 // why this start runs at the game's own size (the menu, the status)
char g_wstr[16] = "", g_hstr[16] = "";  // the sysParams' values (strings, read with strtol(s, 0, 0))
std::atomic<uint32_t> g_modes_added{0}, g_saves_kept{0};
std::atomic<uint32_t> g_eds_calls{0}, g_eds_next{0}, g_eds_end{0};  // through the slot; at the list's next-mode call; its end
std::atomic<bool> g_sentinel_cleared{false};

// The runtime's record (per runtime: RDRVR_xr_runtime.txt's runtime, else the system's active one)
std::mutex g_rec_mutex;
char g_runtime_key[16] = "";   // "rt%08x": a hash of the runtime's manifest path
char g_runtime_path[MAX_PATH * 2] = "";
uint32_t g_rec_w = 0, g_rec_h = 0, g_cap_w = 0, g_cap_h = 0;
uint64_t g_vram_mb = 0;

// ---- the state file: %LOCALAPPDATA%\RDRVR\RDRVR.state.ini (the boot sentinel and the runtimes' records; never the
// user ini, which only the menu writes)
wchar_t g_state_path[MAX_PATH] = L"";
const wchar_t* state_path() {
    if (!g_state_path[0]) {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            std::swprintf(g_state_path, MAX_PATH, L"%s\\RDRVR", local);
            CreateDirectoryW(g_state_path, nullptr);
            std::swprintf(g_state_path, MAX_PATH, L"%s\\RDRVR\\RDRVR.state.ini", local);
        }
        CoTaskMemFree(local);
    }
    return g_state_path;
}
std::string state_get(const char* section, const char* key) {
    wchar_t ws[64], wk[64], out[256] = L"";
    MultiByteToWideChar(CP_UTF8, 0, section, -1, ws, 64);
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wk, 64);
    GetPrivateProfileStringW(ws, wk, L"", out, 256, state_path());
    char s[256];
    WideCharToMultiByte(CP_UTF8, 0, out, -1, s, sizeof(s), nullptr, nullptr);
    return s;
}
void state_set(const char* section, const char* key, const char* value) {  // value nullptr: the key removed
    wchar_t ws[64], wk[64], wv[256];
    MultiByteToWideChar(CP_UTF8, 0, section, -1, ws, 64);
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wk, 64);
    if (value) MultiByteToWideChar(CP_UTF8, 0, value, -1, wv, 256);
    if (!WritePrivateProfileStringW(ws, wk, value ? wv : nullptr, state_path()))
        log::warn("[renderres] the state file: writing [%s] %s failed (%lu)", section, key, GetLastError());
}
bool parse_size(const std::string& s, uint32_t* w, uint32_t* h) {  // "WxH", "W x H" or "WXH"; nothing after it
    unsigned a = 0, b = 0;
    char sep = 0, tail = 0;
    if (sscanf_s(s.c_str(), " %u %c %u %c", &a, &sep, 1, &b, &tail, 1) != 3 || (sep != 'x' && sep != 'X') || !a || !b) return false;
    *w = a;
    *h = b;
    return true;
}

// The runtime the XR session will use: RDRVR_xr_runtime.txt's manifest (tests: the simulator), else the system's active
// OpenXR runtime (HKLM\SOFTWARE\Khronos\OpenXR\1 ActiveRuntime); its records are kept apart, so the simulator's
// size never feeds a headset's Automatic.
void find_runtime() {
    if (g_runtime_key[0]) return;
    wchar_t path[MAX_PATH];
    log::path_in_game_dir(L"RDRVR_xr_runtime.txt", path, MAX_PATH);
    char line[MAX_PATH * 2] = "";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") == 0 && f) {
        std::fgets(line, sizeof(line), f);
        std::fclose(f);
    } else {
        wchar_t v[MAX_PATH] = L"";
        DWORD n = sizeof(v);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, v, &n) ==
            ERROR_SUCCESS)
            WideCharToMultiByte(CP_UTF8, 0, v, -1, line, sizeof(line), nullptr, nullptr);
    }
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
    const char* p = line;
    if (n >= 3 && static_cast<unsigned char>(p[0]) == 0xEF) p += 3;
    std::snprintf(g_runtime_path, sizeof(g_runtime_path), "%s", *p ? p : "(none)");
    uint32_t hash = 2166136261u;  // FNV-1a of the lowercased path
    for (const char* c = g_runtime_path; *c; ++c) {
        const char ch = (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c + 32) : *c == '/' ? '\\' : *c;
        hash = (hash ^ static_cast<unsigned char>(ch)) * 16777619u;
    }
    std::snprintf(g_runtime_key, sizeof(g_runtime_key), "rt%08x", hash);
    char sec[32];
    std::snprintf(sec, sizeof(sec), "Runtime %s", g_runtime_key);
    parse_size(state_get(sec, "Recommended"), &g_rec_w, &g_rec_h);
    parse_size(state_get(sec, "Largest"), &g_cap_w, &g_cap_h);
}

uint64_t vram_mb() {
    if (g_vram_mb) return g_vram_mb;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || !f) return 0;
    uint64_t best = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {  // the game's: the first hardware adapter (FUN_140f74690)
        DXGI_ADAPTER_DESC1 d{};
        const bool hw = SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        a->Release();
        if (hw) {
            best = d.DedicatedVideoMemory;
            break;
        }
    }
    f->Release();
    g_vram_mb = best >> 20;
    return g_vram_mb;
}

// The game's height from graphicsOptions.xml (Documents\Rockstar Games\Red Dead Redemption), read only: Automatic never
// goes below the player's own size. 0 when not found.
uint32_t xml_height() {
    PWSTR docs = nullptr;
    uint32_t h = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        wchar_t path[MAX_PATH];
        std::swprintf(path, MAX_PATH, L"%s\\Rockstar Games\\Red Dead Redemption\\graphicsOptions.xml", docs);
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") == 0 && f) {
            char buf[8192] = "";
            const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            buf[n] = 0;
            if (const char* p = std::strstr(buf, "<ResolutionY value=\"")) h = static_cast<uint32_t>(std::strtoul(p + 20, nullptr, 10));
        }
    }
    CoTaskMemFree(docs);
    return h;
}

// [Render] RenderResolution: off | auto | <height> (16:9) | <W>x<H>
int parse_choice(const std::string& v, uint32_t* cw, uint32_t* ch) {
    *cw = *ch = 0;
    std::string l = v;
    for (char& c : l) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    if (l.empty() || l == "off" || l == "0") return kOff;
    if (l == "auto") return kAuto;
    uint32_t w = 0, h = 0;
    if (!parse_size(l, &w, &h)) {
        if (l.find_first_not_of("0123456789") != std::string::npos) return -1;  // not off, auto, a height or WxH: 0x0, refused
        h = static_cast<uint32_t>(std::strtoul(l.c_str(), nullptr, 10));
        w = width_of(h);
    }
    for (int i = 0; i < kPresetCount; ++i)
        if (kPresets[i].h == h && w == width_of(h)) return kFirstPreset + i;
    *cw = w;
    *ch = h;
    return -1;  // a custom size (the ini only)
}

// The size a choice gives now; false: the game's own (off, Automatic without a record, a size out of range)
bool size_of(int c, uint32_t cw, uint32_t ch, uint32_t* w, uint32_t* h, char* why, size_t why_len) {
    if (why && why_len) why[0] = 0;
    uint32_t sw = 0, sh = 0;
    if (c == kOff) {
        if (why) std::snprintf(why, why_len, "off");
        return false;
    } else if (c == kAuto) {
        find_runtime();
        std::lock_guard lock(g_rec_mutex);
        if (!g_rec_h) {
            if (why) std::snprintf(why, why_len, "Automatic: the headset's size is not known yet (start once with the headset)");
            return false;
        }
        const float s = config::get_float("XR", "EyeScale", 1.0f);
        uint32_t want = static_cast<uint32_t>(std::ceil(g_rec_h * (s > 0.25f && s < 4.0f ? s : 1.0f)));
        want = (want + 1) & ~1u;
        static const uint32_t own = xml_height();  // once: the xml before this start's changes
        const uint32_t floor_h = own ? own : static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
        sh = want > floor_h ? want : floor_h;
        sw = width_of(sh);
    } else if (c >= kFirstPreset && c < kChoiceCount) {
        sh = kPresets[c - kFirstPreset].h;
        sw = width_of(sh);
    } else {
        sw = cw;
        sh = ch;
        if (!sw || !sh) {
            if (why) std::snprintf(why, why_len, "[Render] RenderResolution is not off, auto, a height or WxH");
            return false;
        }
        const double a = static_cast<double>(sw) / sh;
        if (a < 1.74 || a > 1.80) {
            if (why) std::snprintf(why, why_len, "%ux%u is not 16:9 (the wrist HUD and the window expect it)", sw, sh);
            return false;
        }
    }
    sw &= ~1u;
    sh &= ~1u;
    if (sw < 640 || sh < 480 || sw > kMaxW || sh > kMaxH) {
        if (why) std::snprintf(why, why_len, "%ux%u is out of range (640x480 to %ux%u)", sw, sh, kMaxW, kMaxH);
        return false;
    }
    *w = sw;
    *h = sh;
    return true;
}

bool vram_allows(uint32_t w, uint32_t h, char* why, size_t why_len) {
    const uint64_t mb = vram_mb();
    if (!mb) return true;  // not known: no limit from it
    if (mb < 2560) {  // the game itself forces 1920x1080 windowed below this (0x14012e59e)
        if (why) std::snprintf(why, why_len, "the graphics card has %llu MB (the game needs 2560 MB above 1920x1080)",
                               static_cast<unsigned long long>(mb));
        return false;
    }
    const double need = static_cast<double>(w) * h * kBytesPerPixel, room = kVramShare * static_cast<double>(mb) * 1048576.0;
    if (need > room) {
        if (why) std::snprintf(why, why_len, "too large for the graphics card's %llu MB (about %.1f GB needed, %.1f allowed)",
                               static_cast<unsigned long long>(mb), need / 1073741824.0, room / 1073741824.0);
        return false;
    }
    return true;
}

// ---- the hooks (inert unless g_armed)
template <typename T>
T* at(Id id) {
    return reinterpret_cast<T*>(anchors::addr(id));
}

using Pass4_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
Pass4_t o_window = nullptr, o_save = nullptr;
using Eds_t = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
Eds_t o_eds = nullptr;
uintptr_t g_ret_next = 0;

// FUN_140ec3770 (the device init's window create, the game's main thread): the size the init took, and in Windowed the
// player's own window (the swapchain stays at the size; STRETCH scales it into the window)
uintptr_t hk_window(uintptr_t a1, uintptr_t fullscreen, uintptr_t a3, uintptr_t a4) {
    if (g_armed.load()) {
        g_window_ms = log::now_ms();
        const uint32_t sw = static_cast<uint32_t>(*at<int>(Id::ScreenWidth)), sh = static_cast<uint32_t>(*at<int>(Id::ScreenHeight));
        auto* opt = at<uint8_t>(Id::GraphicsOptions);
        g_player_w = *reinterpret_cast<int*>(opt + 4);
        g_player_h = *reinterpret_cast<int*>(opt + 8);
        g_fullscreen = (fullscreen & 0xff) != 0;
        if (sw == g_w && sh == g_h) {
            g_took = true;
            const bool sane = g_player_w >= 640 && g_player_h >= 480 && g_player_w <= 16384 && g_player_h <= 16384;
            if (!g_fullscreen && sane && (static_cast<uint32_t>(g_player_w) != g_w || static_cast<uint32_t>(g_player_h) != g_h) &&
                config::get_bool("Debug", "RenderResolutionKeepWindow", true)) {
                *at<int>(Id::WindowSizeW) = g_player_w;
                *at<int>(Id::WindowSizeH) = g_player_h;
                g_window_kept = true;
            }
            if (!sane) g_player_w = g_player_h = 0;  // the xml keeps what the game wrote (no swap at the save)
        } else {
            std::snprintf(g_why, sizeof(g_why), "the game started at %ux%u, not %ux%u (its own -width/-height or a low-VRAM card)", sw,
                          sh, g_w, g_h);
        }
    }
    return o_window(a1, fullscreen, a3, a4);
}

// FUN_140158be0 (graphicsOptions.xml written, the game's main thread): the xml keeps the player's size, not this start's
// (opening the Graphics menu copies the live size into the options)
uintptr_t hk_save(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    int* res = reinterpret_cast<int*>(at<uint8_t>(Id::GraphicsOptions) + 4);
    const bool swap = g_took.load() && g_player_w > 0 && static_cast<uint32_t>(res[0]) == g_w && static_cast<uint32_t>(res[1]) == g_h &&
                      (static_cast<uint32_t>(g_player_w) != g_w || static_cast<uint32_t>(g_player_h) != g_h);
    if (swap) {
        res[0] = g_player_w;
        res[1] = g_player_h;
        g_saves_kept.fetch_add(1);
    }
    const uintptr_t r = o_save(a1, a2, a3, a4);
    if (swap && res[0] == g_player_w && res[1] == g_player_h) {
        res[0] = static_cast<int>(g_w);
        res[1] = static_cast<int>(g_h);
    }
    return r;
}

// RDR.exe's import of EnumDisplaySettingsW: at the mode list's next-mode call only, the size as one more mode after the
// last real one (the current mode's frequency), so the Resolution option lists, selects and keeps it; every other call
// (and every other index) as it was. The game's DEVMODEW is 188 bytes (dmSize 0xbc: up to dmDisplayFrequency, the last
// field it reads, at +0xb8): only that much is written.
BOOL WINAPI hk_eds(LPCWSTR dev, DWORD i, DEVMODEW* dm) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const BOOL r = o_eds(dev, i, dm);
    g_eds_calls.fetch_add(1, std::memory_order_relaxed);
    if (ret == g_ret_next) g_eds_next.fetch_add(1, std::memory_order_relaxed);
    if (r || ret != g_ret_next || !dm || i == 0 || i >= 0x10000) return r;
    g_eds_end.fetch_add(1, std::memory_order_relaxed);
    constexpr WORD kMinSize = offsetof(DEVMODEW, dmDisplayFrequency) + sizeof(DWORD);
    if (!g_took.load() || dm->dmSize < kMinSize || dm->dmSize > sizeof(DEVMODEW) || dm->dmDriverExtra != 0) {
        log::limited("renderres.eds", 2, "[renderres] the mode list's end (index %lu): not added (took %d, dmSize %u, dmDriverExtra %u)", i,
                     g_took.load() ? 1 : 0, dm->dmSize, dm->dmDriverExtra);
        return r;
    }
    DEVMODEW prev{};
    prev.dmSize = sizeof(prev);
    if (!o_eds(dev, i - 1, &prev)) return r;  // i - 1 was past the real list too: the added mode was given already
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    if (!o_eds(dev, ENUM_CURRENT_SETTINGS, &cur)) cur = prev;
    const WORD size = dm->dmSize;
    std::memcpy(dm, &cur, size);
    dm->dmSize = size;
    dm->dmDriverExtra = 0;
    dm->dmPelsWidth = g_w;
    dm->dmPelsHeight = g_h;
    dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    g_modes_added.fetch_add(1);
    return TRUE;
}

// A sysParam's value slot, checked by its name ("width" at the object's +0, the value at +0x18): on the analysed build
// this is the anchor; a relocated build's GNONE guess must name the same param
bool sysparam_ok(Id id, const char* name) {
    __try {
        const char* n = *reinterpret_cast<const char* const*>(anchors::addr(id) - 0x18);
        return n && std::strcmp(n, name) == 0 && *reinterpret_cast<const char* const*>(anchors::addr(id)) == nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool install_iat() {
    void** slot = at<void*>(Id::EnumDisplaySettingsSlot);
    void* cur = *slot;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(cur), &owner);
    if (!cur || !user32 || (cur != reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsW")) && owner != user32)) {
        log::error("[renderres] EnumDisplaySettingsW's import slot holds %p (user32's export %p): not changed", cur,
                   reinterpret_cast<void*>(user32 ? GetProcAddress(user32, "EnumDisplaySettingsW") : nullptr));
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        log::error("[renderres] EnumDisplaySettingsW's import slot: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    o_eds = reinterpret_cast<Eds_t>(cur);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hk_eds));
    VirtualProtect(slot, sizeof(void*), old, &old);
    log::info("[renderres] EnumDisplaySettingsW's import slot %p: %p -> the mode filter (the list's call returns to %p)", static_cast<void*>(slot), cur,
              reinterpret_cast<void*>(g_ret_next));
    return true;
}

void clear_sentinel(const char* why) {
    if (g_sentinel_cleared.exchange(true)) return;
    state_set("Boot", "Pending", nullptr);
    log::info("[renderres] %ux%u: the boot sentinel cleared (%s)", g_w, g_h, why);
}

// The device init has begun: it writes RenderThreadId at its entry (-1 in the image until then) and makes the device
// (D3dDeviceSingleton, null until then); after either the sysParams may have been read already
bool device_init_started() {
    return *at<uint64_t>(Id::RenderThreadId) != ~0ull || *at<void*>(Id::D3dDeviceSingleton) != nullptr;
}

void frame_end(uint64_t) {
    static double first = 0;
    if (g_sentinel_cleared.load()) return;
    if (!g_took.load()) {  // the window was made before any frame: armed too late after all, nothing of it ran
        if (!g_why[0]) std::snprintf(g_why, sizeof(g_why), "too late this start (the game read its size first)");
        clear_sentinel("not taken this start");
        return;
    }
    if (!active()) return;
    const double now = log::now_ms();
    if (!first) first = now;
    if (now - first >= kSentinelMs) clear_sentinel("two minutes at the size");
}

// Every check, then the hooks, then the sysParams (the last step: the game reads them at its device init). False with
// g_why set: nothing of it is live (an installed inline hook stays, inert).
bool arm(bool early) {
    g_arm_ms = log::now_ms();
    if (device_init_started()) {
        std::snprintf(g_why, sizeof(g_why), "too late this start (the game had made its device: %s)", early ? "early" : "after the anchors' check");
        return false;
    }
    char blocked[64], want[32];
    std::snprintf(blocked, sizeof(blocked), "%s", state_get("Boot", "Blocked").c_str());
    std::snprintf(want, sizeof(want), "%ux%u", g_w, g_h);
    if (blocked[0] && std::strcmp(blocked, want) != 0) {  // another size chosen since: the old block no longer applies
        state_set("Boot", "Blocked", nullptr);
        blocked[0] = 0;
    }
    if (blocked[0]) {
        std::snprintf(g_why, sizeof(g_why), "a start at %s ended within two minutes (not by quitting): choose it again in the menu to retry",
                      blocked);
        return false;
    }
    char why[160];
    if (!vram_allows(g_w, g_h, why, sizeof(why))) {
        std::snprintf(g_why, sizeof(g_why), "%s", why);
        return false;
    }
    if (!sysparam_ok(Id::SysParamWidth, "width") || !sysparam_ok(Id::SysParamHeight, "height")) {
        std::snprintf(g_why, sizeof(g_why), "the game's -width/-height are set already (its command line), or not found in this build");
        return false;
    }
    if (early && !(anchors::precheck(Id::WindowCreate) && anchors::precheck(Id::OptionsSave) && anchors::precheck(Id::ModeListRetNext))) {
        std::snprintf(g_why, sizeof(g_why), "this RDR.exe's code differs (see the log)");
        return false;
    }
    g_ret_next = anchors::addr(Id::ModeListRetNext);
    if (!hooks::install("RDR window create (render resolution)", reinterpret_cast<void*>(anchors::addr(Id::WindowCreate)), hk_window, &o_window) ||
        !hooks::install("RDR options save (render resolution)", reinterpret_cast<void*>(anchors::addr(Id::OptionsSave)), hk_save, &o_save) ||
        !install_iat()) {
        std::snprintf(g_why, sizeof(g_why), "a hook it needs could not be installed (see the log)");
        return false;
    }
    d3d::add_frame_end_listener(frame_end);
    char pending[32];
    std::snprintf(pending, sizeof(pending), "%ux%u", g_w, g_h);
    state_set("Boot", "Pending", pending);
    std::snprintf(g_wstr, sizeof(g_wstr), "%u", g_w);
    std::snprintf(g_hstr, sizeof(g_hstr), "%u", g_h);
    g_armed = true;
    *at<const char*>(Id::SysParamWidth) = g_wstr;
    *at<const char*>(Id::SysParamHeight) = g_hstr;
    const bool late = device_init_started();
    log::info("[renderres] armed %ux%u at %.1f ms (%s): the game's -width/-height%s", g_w, g_h, g_arm_ms, early ? "before the anchors' check" : "after it",
              late ? "; the device init had started meanwhile (the window create will tell)" : "");
    return true;
}

int g_choice = kOff;
uint32_t g_custom_w = 0, g_custom_h = 0;

void load_choice() {
    g_choice = parse_choice(config::get_string("Render", "RenderResolution", "off"), &g_custom_w, &g_custom_h);
}

}  // namespace

void early_arm() {
    load_choice();
    find_runtime();
    // the boot sentinel: a start that armed a size and did not reach its frame count blocks that size
    const std::string pending = state_get("Boot", "Pending");
    if (!pending.empty()) {
        state_set("Boot", "Blocked", pending.c_str());
        state_set("Boot", "Pending", nullptr);
        log::warn("[renderres] the last start at %s ended within two minutes, not by quitting: that size is blocked until it is chosen again",
                  pending.c_str());
    }
    char why[160];
    if (!size_of(g_choice, g_custom_w, g_custom_h, &g_w, &g_h, why, sizeof(why))) {
        std::snprintf(g_why, sizeof(g_why), "%s", why);
        log::info("[renderres] the game's own size: %s", why);
        return;
    }
    if (!anchors::exe_matches() || config::get_bool("Debug", "RelocateTest", false)) {
        g_defer = true;  // after anchors::verify(): this build's addresses
        log::info("[renderres] %ux%u: another RDR.exe build, armed after the anchors' check if still in time", g_w, g_h);
        return;
    }
    if (!arm(true)) log::warn("[renderres] %ux%u not armed: %s", g_w, g_h, g_why);
}

void install() {
    if (!g_defer.exchange(false) || anchors::stand_down()) return;
    if (!arm(false)) log::warn("[renderres] %ux%u not armed: %s", g_w, g_h, g_why);
}

void record_runtime(const char* runtime, uint32_t rec_w, uint32_t rec_h, uint32_t cap_w, uint32_t cap_h) {
    find_runtime();
    {
        std::lock_guard lock(g_rec_mutex);
        g_rec_w = rec_w;
        g_rec_h = rec_h;
        g_cap_w = cap_w;
        g_cap_h = cap_h;
    }
    char sec[32], v[32];
    std::snprintf(sec, sizeof(sec), "Runtime %s", g_runtime_key);
    state_set(sec, "Manifest", g_runtime_path);
    state_set(sec, "Name", runtime ? runtime : "");
    std::snprintf(v, sizeof(v), "%ux%u", rec_w, rec_h);
    state_set(sec, "Recommended", v);
    std::snprintf(v, sizeof(v), "%ux%u", cap_w, cap_h);
    state_set(sec, "Largest", v);
    log::info("[renderres] runtime %s (%s): recommended %ux%u, largest image %ux%u (kept for Automatic)", runtime ? runtime : "?", g_runtime_key,
              rec_w, rec_h, cap_w, cap_h);
}

bool active(uint32_t* w, uint32_t* h) {
    if (!g_took.load()) return false;
    const uint32_t sw = static_cast<uint32_t>(*at<int>(Id::ScreenWidth)), sh = static_cast<uint32_t>(*at<int>(Id::ScreenHeight));
    if (sw != g_w || sh != g_h) return false;  // the game changed its size since (its menu, a fullscreen revert)
    if (w) *w = g_w;
    if (h) *h = g_h;
    return true;
}

void on_exit() {
    if (g_armed.load()) clear_sentinel("a clean exit");
}

int choice_count() { return kChoiceCount; }

const char* choice_label(int i) {
    if (i < 0 || i >= kChoiceCount) return "";
    if (!g_label[i][0]) {
        if (i == kOff) std::snprintf(g_label[i], sizeof(g_label[i]), "The game's own (its Graphics menu)");
        else if (i == kAuto) std::snprintf(g_label[i], sizeof(g_label[i]), "Automatic: what the headset asks for");
        else {
            const Preset& p = kPresets[i - kFirstPreset];
            std::snprintf(g_label[i], sizeof(g_label[i]), "%u tall: %s (%u x %u)", p.h, p.names, width_of(p.h), p.h);
        }
    }
    return g_label[i];
}

bool choice_size(int i, uint32_t* w, uint32_t* h) { return size_of(i, g_custom_w, g_custom_h, w, h, nullptr, 0); }

int choice() { return g_choice; }

void set_choice(int i) {
    if (i < 0 || i >= kChoiceCount) return;
    g_choice = i;
    std::string v = i == kOff ? "off" : i == kAuto ? "auto" : std::to_string(kPresets[i - kFirstPreset].h);
    config::set("Render", "RenderResolution", v);
    state_set("Boot", "Blocked", nullptr);  // the player's own choice retries a blocked size
    if (!g_armed.load() && std::strncmp(g_why, "a start at", 10) == 0)
        std::snprintf(g_why, sizeof(g_why), "blocked this start; chosen again, it is tried at the next start");
}

bool choice_allowed(int i, char* why, size_t why_len) {
    if (why && why_len) why[0] = 0;
    if (i == kOff) return true;
    uint32_t w = 0, h = 0;
    char tmp[160] = "";
    if (!size_of(i, g_custom_w, g_custom_h, &w, &h, tmp, sizeof(tmp))) {
        if (why) std::snprintf(why, why_len, "%s", tmp);
        return i == kAuto;  // Automatic can be chosen before its size is known
    }
    if (!vram_allows(w, h, tmp, sizeof(tmp))) {
        if (why) std::snprintf(why, why_len, "%s", tmp);
        return false;
    }
    std::lock_guard lock(g_rec_mutex);
    if (g_cap_w && (w > g_cap_w || h > g_cap_h) && why)
        std::snprintf(why, why_len, "larger than this headset's largest image (%u x %u): scaled down for it", g_cap_w, g_cap_h);
    return true;
}

void status_text(char* out, size_t len) {
    uint32_t w = 0, h = 0;
    if (active(&w, &h)) {
        if (g_window_kept) std::snprintf(out, len, "running %u x %u (your window %d x %d)", w, h, g_player_w, g_player_h);
        else std::snprintf(out, len, "running %u x %u%s", w, h, g_fullscreen ? " (fullscreen)" : "");
    } else if (g_took.load()) {
        std::snprintf(out, len, "the game changed its size since the start (%d x %d now)", *at<int>(Id::ScreenWidth), *at<int>(Id::ScreenHeight));
    } else if (g_armed.load()) {
        std::snprintf(out, len, "%s", g_why[0] ? g_why : "armed, the game's device not made yet");
    } else {
        std::snprintf(out, len, "the game's own size%s%s", g_why[0] ? ": " : "", g_why);
    }
}

std::string command(const std::string&) {
    char st[256], b[1024];
    status_text(st, sizeof(st));
    std::lock_guard lock(g_rec_mutex);
    std::snprintf(b, sizeof(b),
                  "renderres: choice %d want %ux%u armed %d took %d active %d window %s %dx%d fullscreen %d modes +%u (calls %u, next %u, end %u) saves kept %u "
                  "sentinel %s vram %llu MB rec %ux%u largest %ux%u runtime %s arm %.1f ms window %.1f ms | %s",
                  g_choice, g_w, g_h, g_armed.load() ? 1 : 0, g_took.load() ? 1 : 0, active() ? 1 : 0, g_window_kept ? "kept" : "game's",
                  g_player_w, g_player_h, g_fullscreen ? 1 : 0, g_modes_added.load(), g_eds_calls.load(), g_eds_next.load(), g_eds_end.load(),
                  g_saves_kept.load(),
                  g_sentinel_cleared.load() ? "cleared" : g_armed.load() ? "pending" : "-", static_cast<unsigned long long>(vram_mb()), g_rec_w,
                  g_rec_h, g_cap_w, g_cap_h, g_runtime_key, g_arm_ms, g_window_ms, st);
    return b;
}

}  // namespace rdrvr::render_res
