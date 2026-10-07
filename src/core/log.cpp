#include "core/log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace rdrvr::log {
namespace {

constexpr uint64_t kMaxBytes = 256ull << 20;  // per run
constexpr int kMaxKeys = 512;

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<uint64_t> g_bytes{0};
std::atomic<bool> g_capped{false};
LARGE_INTEGER g_qpc_freq{}, g_qpc_start{};
wchar_t g_game_dir[MAX_PATH] = {};

struct KeyCount {
    const char* key;
    uint32_t count;
};
KeyCount g_keys[kMaxKeys];
int g_key_count = 0;
SRWLOCK g_key_lock = SRWLOCK_INIT;

const char* level_tag(Level l) {
    switch (l) {
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
        default: return "INFO ";
    }
}

void write_raw(const char* text, size_t len) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    if (g_capped.load(std::memory_order_relaxed)) return;
    uint64_t total = g_bytes.fetch_add(len, std::memory_order_relaxed) + len;
    if (total > kMaxBytes) {
        if (!g_capped.exchange(true)) {
            static const char kCap[] = "==== RDRVR.log cap reached for this run; logging stopped ====\r\n";
            DWORD w = 0;
            AcquireSRWLockExclusive(&g_lock);
            WriteFile(g_file, kCap, sizeof(kCap) - 1, &w, nullptr);
            ReleaseSRWLockExclusive(&g_lock);
        }
        return;
    }
    DWORD written = 0;
    AcquireSRWLockExclusive(&g_lock);
    WriteFile(g_file, text, static_cast<DWORD>(len), &written, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
}

void vwrite(Level level, const char* fmt, va_list ap) {
    char buf[2048];
    int n = std::snprintf(buf, sizeof(buf), "%10.3f %5lu %s ", now_ms(), GetCurrentThreadId(), level_tag(level));
    if (n < 0) return;
    const size_t room = sizeof(buf) - 2 - static_cast<size_t>(n);  // keep 2 bytes for "\r\n"
    int m = std::vsnprintf(buf + n, room, fmt, ap);
    size_t body = m < 0 ? 0 : (static_cast<size_t>(m) < room ? static_cast<size_t>(m) : room - 1);  // truncated
    size_t len = static_cast<size_t>(n) + body;
    buf[len++] = '\r';
    buf[len++] = '\n';
    write_raw(buf, len);
}

}  // namespace

double now_ms() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart - g_qpc_start.QuadPart) * 1000.0 / static_cast<double>(g_qpc_freq.QuadPart);
}

const wchar_t* game_dir() { return g_game_dir; }

const wchar_t* path_in_game_dir(const wchar_t* name, wchar_t* buf, size_t buf_len) {
    std::swprintf(buf, buf_len, L"%s%s", g_game_dir, name);
    return buf;
}

void open() {
    if (g_file != INVALID_HANDLE_VALUE) return;
    QueryPerformanceFrequency(&g_qpc_freq);
    // Process start time, so timestamps line up across modules and with the game's own clock.
    FILETIME create{}, exit_{}, kern{}, user{};
    QueryPerformanceCounter(&g_qpc_start);
    if (GetProcessTimes(GetCurrentProcess(), &create, &exit_, &kern, &user)) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER c{{create.dwLowDateTime, create.dwHighDateTime}}, n{{now.dwLowDateTime, now.dwHighDateTime}};
        if (n.QuadPart > c.QuadPart) {
            g_qpc_start.QuadPart -= static_cast<LONGLONG>((n.QuadPart - c.QuadPart) * g_qpc_freq.QuadPart / 10000000ull);
        }
    }

    GetModuleFileNameW(nullptr, g_game_dir, MAX_PATH);
    if (wchar_t* slash = std::wcsrchr(g_game_dir, L'\\')) slash[1] = 0;

    wchar_t path[MAX_PATH];
    path_in_game_dir(L"RDRVR.log", path, MAX_PATH);
    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char banner[256];
    int n = std::snprintf(banner, sizeof(banner),
                          "\r\n==== RUN START %04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu ====\r\n", st.wYear, st.wMonth,
                          st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId());
    if (n > 0) write_raw(banner, static_cast<size_t>(n));
}

void write(Level level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(level, fmt, ap);
    va_end(ap);
}

void info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(Level::Info, fmt, ap);
    va_end(ap);
}

void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(Level::Warn, fmt, ap);
    va_end(ap);
}

void error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(Level::Error, fmt, ap);
    va_end(ap);
}

void limited(const char* key, uint32_t max_count, const char* fmt, ...) {
    uint32_t count = 0;
    AcquireSRWLockExclusive(&g_key_lock);
    int i = 0;
    for (; i < g_key_count; ++i) {
        if (g_keys[i].key == key || std::strcmp(g_keys[i].key, key) == 0) break;
    }
    if (i == g_key_count && g_key_count < kMaxKeys) g_keys[g_key_count++] = {key, 0};
    if (i < kMaxKeys) count = ++g_keys[i].count;
    ReleaseSRWLockExclusive(&g_key_lock);
    if (count == 0 || count > max_count + 1) return;
    if (count == max_count + 1) {
        info("[log] '%s' reached its limit of %u lines; suppressing further lines", key, max_count);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vwrite(Level::Info, fmt, ap);
    va_end(ap);
}

}  // namespace rdrvr::log
