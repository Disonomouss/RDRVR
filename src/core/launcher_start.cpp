#include "core/launcher_start.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <string>

#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::launcher_start {
namespace {

using anchors::Id;

constexpr ULONGLONG kHoldCapMs = 5ull * 60ull * 1000ull;    // the user stopped the countdown: the game goes on after this anyway
constexpr DWORD kGateCapMs = 6u * 60u * 1000u;              // the main thread's own cap (a bootstrap that never releases it)
constexpr int kStartGraceS = 15;                           // the launcher's own start, before its countdown begins
constexpr ULONGLONG kUpWithinMs = 20000;                   // its "up" event within this, else an older launcher (no --at-start)
constexpr long long kNoteMaxAgeS = 300;                    // the launcher's Play note: Steam's or the Rockstar launcher's own wait

std::atomic<bool> g_show{true};
std::atomic<int> g_timeout{30};

// The gate: the game's main thread waits here at its device init's entry while g_open is false
using Init_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
Init_t o_init = nullptr;
HANDLE g_gate = nullptr;  // manual reset, set by release_game() (never closed: the detour may still read it)
std::atomic<bool> g_open{true};
std::atomic<bool> g_armed{false};  // run() closed the gate: release_game() must open it
std::atomic<bool> g_held_once{false};

int clamp_timeout(int s) { return s <= 0 ? 0 : (s < 5 ? 5 : (s > 300 ? 300 : s)); }

void read_settings() {
    g_show = config::get_bool("Launcher", "ShowAtStart", true);
    g_timeout = clamp_timeout(config::get_int("Launcher", "StartTimeout", 30));
}

// FUN_14012e420 (the game's main thread, its boot): the first call waits while the gate is closed; every call then goes
// straight through. Its only caller passes r8 (kept in rsi); rcx, rdx and r9 pass through untouched
uintptr_t hk_init(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    if (!g_open.load(std::memory_order_acquire) && !g_held_once.exchange(true)) {
        const double t0 = log::now_ms();
        log::info("[launcher] the game held at its device init (its main thread) until the launcher closes");
        const DWORD r = WaitForSingleObject(g_gate, kGateCapMs);
        log::info("[launcher] the game's device init went on after %.0f ms held%s", log::now_ms() - t0,
                  r == WAIT_OBJECT_0 ? "" : " (ITS OWN CAP: the bootstrap never released it; the startup hooks may be late)");
    }
    return o_init(a, b, c, d);
}

bool file_exists(const wchar_t* p) {
    const DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The launcher's Play wrote "started <UTC unix seconds> pid <n>" just before it started the game (Steam and
// PlayRDR.exe pass nothing on to RDR.exe): a fresh note means this start is that one. Any note is deleted once read
bool handed_off() {
    const wchar_t* user = config::user_ini_path();
    if (!user || !user[0]) return false;
    std::wstring path = user;
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    path = path.substr(0, slash + 1) + L"RDRVR.launcher_started";
    if (!file_exists(path.c_str())) return false;
    char text[160] = {};
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
        std::fread(text, 1, sizeof(text) - 1, f);
        std::fclose(f);
    }
    long long t = 0;
    const bool parsed = sscanf_s(text, "started %lld", &t) == 1;
    const long long age = static_cast<long long>(_time64(nullptr)) - t;
    const BOOL gone = DeleteFileW(path.c_str());
    if (parsed && age >= -60 && age <= kNoteMaxAgeS) {
        log::info("[launcher] at start: not shown: started from the launcher's own Play %lld s ago (its note %s)", age,
                  gone ? "deleted" : "NOT DELETED");
        return true;
    }
    log::info("[launcher] at start: an old note from the launcher (%s) %s: ignored", parsed ? (std::to_string(age) + " s").c_str() : "unreadable",
              gone ? "deleted" : "not deleted");
    return false;
}

bool device_init_begun() {
    return *anchors::ptr<const uint64_t*>(Id::RenderThreadId) != ~0ull || *anchors::ptr<void* const*>(Id::D3dDeviceSingleton) != nullptr;
}

const char* exit_text(DWORD code) {
    switch (code) {
        case 0: return "its Play (saved)";
        case 1: return "closed by the user";
        case 2: return "its countdown ran out";
        case 3: return "it saw the game go on";
        case 4: return "it saw the game gone";
        case 6: return "closed by the user: the start cancelled";
        case STILL_ACTIVE: return "still open";
        default: return "an error";
    }
}

std::string narrow(const std::wstring& w) {
    char b[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, static_cast<int>(sizeof(b)) - 1, nullptr, nullptr);
    return b;
}

}  // namespace

bool show_at_start() { return g_show.load(); }
int start_timeout() { return g_timeout.load(); }

void set_show_at_start(bool on) {
    g_show = on;
    config::set("Launcher", "ShowAtStart", on ? "1" : "0");
}

void set_start_timeout(int seconds) {
    g_timeout = clamp_timeout(seconds);
    config::set("Launcher", "StartTimeout", std::to_string(g_timeout.load()));
}

void run() {
    read_settings();
    {  // a relaunch back to the menu (Undead Nightmare's -rebootToMenu): the player is mid-session, in the headset
        std::wstring cl = GetCommandLineW() ? GetCommandLineW() : L"";
        for (auto& ch : cl) ch = static_cast<wchar_t>(towlower(ch));
        if (cl.find(L"-reboottomenu") != std::wstring::npos) {
            log::info("[launcher] at start: not shown: a relaunch (-rebootToMenu)");
            return;
        }
    }
    if (!g_show.load()) {
        log::info("[launcher] at start: not shown: off ([Launcher] ShowAtStart=0)");
        return;
    }
    // 2026-10-31 (r301, r303): in its own folder, never the game folder's root. There, Windows found the game folder's
    // winmm.dll (RedHook's proxy) first for the launcher too, RedHook.dll loaded into it, and it crashed within 1 s
    // (0xC0000005 in RedHook.dll+0x28bd). Its folder is also its working directory
    wchar_t exe[MAX_PATH], sim[MAX_PATH];
    log::path_in_game_dir(L"RDRVR_Launcher\\RDRVR_Launcher.exe", exe, MAX_PATH);
    if (!file_exists(exe)) {
        log::info("[launcher] at start: not shown: no RDRVR_Launcher\\RDRVR_Launcher.exe in the game folder");
        return;
    }
    log::path_in_game_dir(L"RDRVR_xr_runtime.txt", sim, MAX_PATH);
    const bool with_sim = config::get_bool("Debug", "LauncherWithSimulator", false);
    if (file_exists(sim) && !with_sim) {
        log::info("[launcher] at start: not shown: the simulator is deployed (RDRVR_xr_runtime.txt: a test cycle)");
        return;
    }
    if (handed_off()) return;
    if (config::get_bool("Debug", "RelocateTest", false)) {  // (its scan would find the hooked entry changed)
        log::info("[launcher] at start: not shown: [Debug] RelocateTest");
        return;
    }
    if (!anchors::exe_matches() || !anchors::precheck(Id::BootDeviceInit)) {
        log::info("[launcher] at start: not shown: another RDR.exe build (its device init cannot be held: the settings would not apply)");
        return;
    }
    if (device_init_begun()) {
        log::info("[launcher] at start: not shown: too late, the game's device init had begun");
        return;
    }

    const DWORD pid = GetCurrentProcessId();
    wchar_t name[64];
    std::swprintf(name, 64, L"Local\\RDRVR_LauncherHold_%lu", pid);
    HANDLE hold = CreateEventW(nullptr, TRUE, FALSE, name);
    std::swprintf(name, 64, L"Local\\RDRVR_LauncherGone_%lu", pid);
    HANDLE gone = CreateEventW(nullptr, TRUE, FALSE, name);
    std::swprintf(name, 64, L"Local\\RDRVR_LauncherUp_%lu", pid);
    HANDLE up = CreateEventW(nullptr, TRUE, FALSE, name);  // set by the launcher once it runs in its at-start mode
    g_gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto close_events = [&] {
        if (hold) CloseHandle(hold);
        if (gone) CloseHandle(gone);
        if (up) CloseHandle(up);
    };
    if (!hold || !gone || !up || !g_gate) {
        log::warn("[launcher] at start: not shown: an event could not be made (error %lu)", GetLastError());
        close_events();
        return;  // (g_gate unused: the gate stays open)
    }
    g_open.store(false, std::memory_order_release);
    g_armed = true;
    if (!hooks::install("RDR boot device init (the launcher at start)", reinterpret_cast<void*>(anchors::addr(Id::BootDeviceInit)), hk_init,
                        &o_init)) {
        g_armed = false;
        g_open.store(true, std::memory_order_release);
        SetEvent(g_gate);
        close_events();
        log::warn("[launcher] at start: not shown: the hook on the device init could not be installed");
        return;
    }
    if (device_init_begun() && !g_held_once.load()) {  // the init began while the hook went in: nothing can be held now
        release_game();
        close_events();
        log::info("[launcher] at start: not shown: too late, the game's device init began while the hook went in");
        return;
    }

    const int timeout = g_timeout.load();
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --at-start " + std::to_wstring(pid) + L" " + std::to_wstring(timeout);
    const std::string test = config::get_string("Debug", "LauncherTestArgs", "");
    if (!test.empty() && test.size() < 400) {
        wchar_t wt[512] = {};
        MultiByteToWideChar(CP_UTF8, 0, test.c_str(), -1, wt, 511);
        cmd += L" ";
        cmd += wt;
    }
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    AllowSetForegroundWindow(ASFW_ANY);  // (the game has no window yet: the launcher may come to the front)
    std::wstring line = cmd;              // CreateProcessW may write into its command line
    if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        log::warn("[launcher] at start: RDRVR_Launcher.exe could not be started (error %lu): the game goes on as without it", GetLastError());
        release_game();
        close_events();
        return;
    }
    CloseHandle(pi.hThread);
    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG deadline = timeout > 0 ? t0 + static_cast<ULONGLONG>(timeout + kStartGraceS) * 1000ull : t0 + kHoldCapMs;
    log::info("[launcher] at start: RDRVR_Launcher.exe started (pid %lu, %s; the game's deadline %.0f s): %s", pi.dwProcessId,
              timeout > 0 ? ("its countdown " + std::to_string(timeout) + " s").c_str() : "no countdown",
              static_cast<double>(deadline - t0) / 1000.0, narrow(cmd).c_str());
    bool held = false, answered = false;
    const char* how = "its deadline passed";
    for (;;) {
        const DWORD r = WaitForSingleObject(pi.hProcess, 250);
        if (r == WAIT_OBJECT_0) {
            how = "the launcher closed";
            break;
        }
        if (r != WAIT_TIMEOUT) {
            how = "the wait failed";
            break;
        }
        if (!answered && WaitForSingleObject(up, 0) == WAIT_OBJECT_0) answered = true;
        if (!answered && GetTickCount64() - t0 >= kUpWithinMs) {
            how = "the launcher never answered (an older RDRVR_Launcher.exe without --at-start?)";
            break;
        }
        if (!held && WaitForSingleObject(hold, 0) == WAIT_OBJECT_0) {
            held = true;
            deadline = t0 + kHoldCapMs;
            log::info("[launcher] at start: the user stopped the countdown after %.1f s: the game waits for Play (5 minutes at most)",
                      static_cast<double>(GetTickCount64() - t0) / 1000.0);
        }
        if (GetTickCount64() >= deadline) break;
    }
    SetEvent(gone);  // the launcher, still open: the game went on (saving is off while it runs)
    // a launcher still open may be in the middle of a save: its own exit (it closes on the gone event when nothing is
    // unsaved) or its .tmp gone first, 3 s at most, so the re-read never meets a file being replaced
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
        WaitForSingleObject(pi.hProcess, 2000);
        const std::wstring tmp = std::wstring(config::user_ini_path()) + L".tmp";
        for (int i = 0; i < 10 && file_exists(tmp.c_str()); ++i) Sleep(100);
    }
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    log::info("[launcher] at start: the wait ended after %.1f s: %s (%s, exit code %lu)", static_cast<double>(GetTickCount64() - t0) / 1000.0,
              how, exit_text(code), code);
    // (2026-11-02, GOAL-older-items item 19; the user: "closing the launcher still runs the game. It should cancel it") the
    // launcher closed by the user (its X or Close: exit code 6; an older launcher's 1, or one ended in Task Manager, lets
    // the game go on): the game's start cancelled. TerminateProcess itself (the review: the exit guard is installed only
    // after this, so ExitProcess would run every DLL's detach, RedHook's crash among them); nothing loaded or saved yet
    if (code == 6 && std::strcmp(how, "the launcher closed") == 0) {
        log::info("[launcher] at start: closed by the user: the game's start cancelled (the game exits)");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    // (hold and gone stay open: the launcher may still read gone)
    config::load();
    read_settings();
    log::info("[launcher] at start: the settings read again (the user file as the launcher left it)");
    if (!g_held_once.load() && device_init_begun())  // the main thread was already past the hooked entry: nothing was held
        log::warn("[launcher] at start: the game's device init had begun without waiting: the settings saved in the launcher apply at the next start");
    if (!test.empty()) config::get_string("Launcher", "TestMark", "");  // (r65: the echo shows the launcher's saved value)
}

void release_game() {
    if (!g_armed.exchange(false)) return;
    g_open.store(true, std::memory_order_release);
    SetEvent(g_gate);
    log::info("[launcher] at start: the game released (%s)", g_held_once.load() ? "it was held at its device init" : "it had not reached its device init");
}

}  // namespace rdrvr::launcher_start
