#pragma once
// Bounded, append-only log next to RDR.exe (RDRVR.log).
//
// Every run appends under a "==== RUN START" banner (never truncates), each line carries milliseconds since
// the process started and the thread id, and the whole run is capped (kMaxBytes) so a runaway caller on a
// draw path cannot fill the disk or stall the game. Lines are written straight to the file (no user-mode
// buffering), so the last line before a crash is on disk.
//
// Use log::once / log::limited on any path that can run per frame or per draw.

#include <cstdint>

namespace rdrvr::log {

enum class Level { Info, Warn, Error };

void open();                         // idempotent; called from DllMain
void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);
void write(Level level, const char* fmt, ...);

// Logs the first `max_count` calls for `key` (a string literal identifying the call site), then one
// "suppressing" line, then nothing. Thread-safe.
void limited(const char* key, uint32_t max_count, const char* fmt, ...);
inline constexpr uint32_t kOnce = 1;

double now_ms();                     // milliseconds since the process started (QPC)
const wchar_t* game_dir();           // directory of RDR.exe, with a trailing backslash
const wchar_t* path_in_game_dir(const wchar_t* name, wchar_t* buf, size_t buf_len);

}  // namespace rdrvr::log
