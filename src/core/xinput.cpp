#include "core/xinput.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::xinput {
namespace {

using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetState_t = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetCaps_t = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
GetState_t o_GetState = nullptr;
SetState_t o_SetState = nullptr;
GetCaps_t o_GetCaps = nullptr;

bool g_virtual = true;  // [Input] VirtualPad
std::atomic<uint16_t> g_buttons{0};
std::atomic<ULONGLONG> g_until{0};
std::atomic<bool> g_sticks_on{false};               // the press holds the sticks too (test aid "stick")
std::atomic<SHORT> g_lx{0}, g_ly{0}, g_rx{0}, g_ry{0};
std::atomic<uint64_t> g_polls{0}, g_injected{0}, g_caps{0}, g_rumbles{0};
// run 7 item 1e: the shoulder buttons' presses the game read (RB is its cover button, LB its own): rising edges
std::atomic<uint64_t> g_rb_presses{0}, g_lb_presses{0}, g_x_presses{0};  // X: the game's jump (item 1f)
std::atomic<uint64_t> g_x_press_ms{0};
WORD g_last_buttons = 0;
std::atomic<uint32_t> g_rumble{0};  // last motor speeds sent to pad 0: left << 16 | right
std::atomic<DWORD> g_packet{0x52445652};
std::atomic<bool> g_was_injecting{false};
std::mutex g_source_mutex;
bool g_source_on = false;
PadState g_source;
bool g_source_changed = false;
std::atomic<bool> g_turn_on{false};
std::atomic<float> g_turn_deg{0.0f}, g_right_x{0.0f};
// Headset round 2: a recentre on the gamepad, both stick clicks held for 1 s (read before any injection).
ULONGLONG g_combo_since = 0;  // polling thread
bool g_combo_fired = false;
std::atomic<bool> g_recentre_combo{false};

// The game asks XInputGetCapabilities which pads exist (pads 0..3, at startup and again later) and then polls
// only the connected ones: with no real pad it called XInputGetState 8 times in a 5-minute run (2026-10-03).
// So the virtual pad must exist from the first query, not only while a press is held.
DWORD WINAPI hk_GetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    DWORD r = o_GetCaps(user, flags, caps);
    uint64_t n = g_caps.fetch_add(1) + 1;
    bool synth = user == 0 && caps && g_virtual && r != ERROR_SUCCESS;
    if (synth) {
        ZeroMemory(caps, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;  // every button the gamepad reports
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = static_cast<SHORT>(0xFFC0);
        caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
        caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFF;
        r = ERROR_SUCCESS;
    }
    if (n <= 8)
        log::info("[xinput] XInputGetCapabilities(%lu, %#lx) -> %lu%s (call %llu)", user, flags, r,
                  synth ? " (virtual pad)" : "", static_cast<unsigned long long>(n));
    return r;
}

DWORD WINAPI hk_GetState(DWORD user, XINPUT_STATE* st) {
    DWORD r = o_GetState(user, st);
    uint64_t n = g_polls.fetch_add(1) + 1;
    bool synth = user == 0 && st && g_virtual && r != ERROR_SUCCESS;
    if (synth) {
        ZeroMemory(st, sizeof(*st));
        st->dwPacketNumber = g_packet.load();
        r = ERROR_SUCCESS;
    }
    if (n <= 4)
        log::info("[xinput] XInputGetState(%lu) -> %lu%s (call %llu)", user, r, synth ? " (virtual pad)" : "",
                  static_cast<unsigned long long>(n));
    if (user == 0 && st && r == ERROR_SUCCESS && !synth) {
        constexpr WORD kBoth = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
        if ((st->Gamepad.wButtons & kBoth) == kBoth) {
            ULONGLONG now = GetTickCount64();
            if (!g_combo_since) g_combo_since = now;
            if (!g_combo_fired && now - g_combo_since >= 1000) {
                g_combo_fired = true;
                g_recentre_combo = true;
                log::info("[xinput] both stick clicks held 1 s: recentre");
            }
        } else {
            g_combo_since = 0;
            g_combo_fired = false;
        }
    }
    if (user == 0 && st && r == ERROR_SUCCESS) {
        // The packet number changes only when the state does, as a real pad's would.
        if (GetTickCount64() < g_until.load()) {
            st->Gamepad.wButtons |= g_buttons.load();
            if (g_sticks_on.load()) {
                st->Gamepad.sThumbLX = g_lx.load();
                st->Gamepad.sThumbLY = g_ly.load();
                st->Gamepad.sThumbRX = g_rx.load();
                st->Gamepad.sThumbRY = g_ry.load();
            }
            if (!g_was_injecting.exchange(true)) ++g_packet;
            st->dwPacketNumber = g_packet.load();
            g_injected.fetch_add(1);
        } else if (g_was_injecting.exchange(false)) {
            st->dwPacketNumber = ++g_packet;
        }
        if (g_turn_on.load(std::memory_order_relaxed)) {
            float a = g_turn_deg.load(std::memory_order_relaxed) * 0.0174532925f;
            float lx = st->Gamepad.sThumbLX / 32767.0f, ly = st->Gamepad.sThumbLY / 32767.0f;
            auto clamp = [](float v) { return static_cast<SHORT>((v < -1 ? -1 : v > 1 ? 1 : v) * 32767.0f); };
            st->Gamepad.sThumbLX = clamp(lx * std::cos(a) - ly * std::sin(a));
            st->Gamepad.sThumbLY = clamp(lx * std::sin(a) + ly * std::cos(a));
            g_right_x.store(st->Gamepad.sThumbRX / 32767.0f, std::memory_order_relaxed);
            st->Gamepad.sThumbRX = 0;
        }
        std::lock_guard lock(g_source_mutex);
        if (g_source_on) {
            // merged with a real pad: buttons or'd, each stick the one pushed further, each trigger the larger
            auto mag = [](SHORT x, SHORT y) { return static_cast<float>(x) * x + static_cast<float>(y) * y; };
            st->Gamepad.wButtons |= g_source.buttons;
            if (mag(g_source.lx, g_source.ly) > mag(st->Gamepad.sThumbLX, st->Gamepad.sThumbLY)) {
                st->Gamepad.sThumbLX = g_source.lx;
                st->Gamepad.sThumbLY = g_source.ly;
            }
            if (mag(g_source.rx, g_source.ry) > mag(st->Gamepad.sThumbRX, st->Gamepad.sThumbRY)) {
                st->Gamepad.sThumbRX = g_source.rx;
                st->Gamepad.sThumbRY = g_source.ry;
            }
            if (g_source.lt > st->Gamepad.bLeftTrigger) st->Gamepad.bLeftTrigger = g_source.lt;
            if (g_source.rt > st->Gamepad.bRightTrigger) st->Gamepad.bRightTrigger = g_source.rt;
            if (g_source_changed) {
                g_source_changed = false;
                ++g_packet;
            }
            st->dwPacketNumber = g_packet.load();
            g_injected.fetch_add(1);
        }
    }
    if (user == 0 && st && r == ERROR_SUCCESS) {
        const WORD b = st->Gamepad.wButtons, up = static_cast<WORD>(b & ~g_last_buttons);
        if (up & XINPUT_GAMEPAD_RIGHT_SHOULDER) g_rb_presses.fetch_add(1, std::memory_order_relaxed);
        if (up & XINPUT_GAMEPAD_LEFT_SHOULDER) g_lb_presses.fetch_add(1, std::memory_order_relaxed);
        if (up & XINPUT_GAMEPAD_X) {
            g_x_presses.fetch_add(1, std::memory_order_relaxed);
            g_x_press_ms.store(GetTickCount64(), std::memory_order_relaxed);
        }
        g_last_buttons = b;
    }
    return r;
}

DWORD WINAPI hk_SetState(DWORD user, XINPUT_VIBRATION* vib) {
    DWORD r = o_SetState(user, vib);
    if (user == 0 && vib) {
        uint32_t v = (static_cast<uint32_t>(vib->wLeftMotorSpeed) << 16) | vib->wRightMotorSpeed;
        if (g_rumble.exchange(v) != v) g_rumbles.fetch_add(1);
        if (g_virtual && r != ERROR_SUCCESS) r = ERROR_SUCCESS;
    }
    return r;
}

}  // namespace

void set_pad_turn(bool on, float deg) {
    if (g_turn_on.exchange(on) != on)
        log::info("[xinput] gamepad %s", on ? "drives walking (left stick turned by the head's yaw) and turning (right stick)" : "as the game reads it");
    g_turn_deg = deg;
    if (!on) g_right_x = 0;
}

float pad_right_x() { return g_right_x.load(std::memory_order_relaxed); }

bool take_recentre_combo() { return g_recentre_combo.exchange(false); }

void set_source(bool on, const PadState& state) {
    std::lock_guard lock(g_source_mutex);
    bool changed = on != g_source_on || std::memcmp(&state, &g_source, sizeof(state)) != 0;
    if (on != g_source_on) log::info("[xinput] gameplay pad source %s", on ? "on" : "off");
    g_source_on = on;
    g_source = state;
    g_source_changed = g_source_changed || changed;
}

bool install() {
    g_virtual = config::get_bool("Input", "VirtualPad", true);
    HMODULE m = GetModuleHandleW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) {
        log::warn("[xinput] xinput1_4.dll not available");
        return false;
    }
    // The game imports ordinals 2, 3 and 4 through its hidden import table (research\hidden_imports.txt).
    bool ok = hooks::install("xinput1_4!XInputGetState", reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")),
                             hk_GetState, &o_GetState);
    ok = hooks::install("xinput1_4!XInputSetState", reinterpret_cast<void*>(GetProcAddress(m, "XInputSetState")),
                        hk_SetState, &o_SetState) && ok;
    ok = hooks::install("xinput1_4!XInputGetCapabilities",
                        reinterpret_cast<void*>(GetProcAddress(m, "XInputGetCapabilities")), hk_GetCaps, &o_GetCaps) &&
         ok;
    log::info("[xinput] virtual pad %s", g_virtual ? "on (pad 0 always connected)" : "off (injection only with a real pad 0)");
    return ok;
}

void press_sticks(float lx, float ly, float rx, float ry, uint32_t ms) {
    auto s = [](float v) { return static_cast<SHORT>((v < -1 ? -1 : v > 1 ? 1 : v) * 32767.0f); };
    g_lx = s(lx);
    g_ly = s(ly);
    g_rx = s(rx);
    g_ry = s(ry);
    g_buttons = 0;
    g_sticks_on = true;
    g_until = GetTickCount64() + ms;
    log::info("[xinput] inject sticks L (%.2f %.2f) R (%.2f %.2f) for %u ms", lx, ly, rx, ry, ms);
}

void press(uint16_t buttons, uint32_t ms) {
    g_sticks_on = false;
    g_buttons = buttons;
    g_until = GetTickCount64() + ms;
    log::info("[xinput] inject buttons %#x for %u ms", buttons, ms);
}

uint64_t polls() { return g_polls.load(); }
uint64_t injected() { return g_injected.load(); }
uint64_t caps_queries() { return g_caps.load(); }
uint32_t rumble() { return g_rumble.load(); }
uint64_t rumble_changes() { return g_rumbles.load(); }
uint64_t rb_presses() { return g_rb_presses.load(std::memory_order_relaxed); }
uint64_t lb_presses() { return g_lb_presses.load(std::memory_order_relaxed); }
uint64_t x_presses() { return g_x_presses.load(std::memory_order_relaxed); }
uint64_t x_press_tick() { return g_x_press_ms.load(std::memory_order_relaxed); }

}  // namespace rdrvr::xinput
