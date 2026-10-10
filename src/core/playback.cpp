// [Render] PlaybackBatch (playback.h). ReplayChunk FUN_140ecdd30's loop (Ghidra 2026-10-10): RBP the chunk (set once at
// entry), R14 the read offset's address; its head (0x140ecdd61..0x140ecdd88) waits for count +10h > 0 (yielding through
// the game's SwitchToThread thunk), LOCK DECs it, and reads the packet at [R14] + [RBP+18h]; every case sets its size in
// EAX and the common tail (0x140ece51f) adds it to [R14] and loops to the head until the terminator. Nothing else in
// the function touches +10h or +18h, and the head has one entry (the tail's jnz at 0x140ece525). A link packet (0x40,
// FUN_140eca0a0) replays its chunk through the immediate context: a nested ReplayChunk call, so a claim is kept per
// call (the entry hook saves and restores it).

#include "core/playback.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"

extern "C" void rdrvr_playback_head();
extern "C" void* rdrvr_rb_dispatch = nullptr;  // 0x140ecdd8a: the original dispatch (eax = the opcode, rdi = the packet)

namespace rdrvr::playback {
namespace {

struct Claim {
    char* chunk = nullptr;
    long budget = 0;  // packets claimed from the chunk's count and not yet handed out
    char* data = nullptr;
};
thread_local Claim t_claim;
std::atomic<int> g_k{-1};
bool g_installed = false;
// the playback thread's own counters (one writer; read torn at worst by the status)
volatile uint64_t g_claims = 0, g_packets = 0, g_waits = 0, g_entries = 0, g_nested = 0;
using Yield_t = BOOL(WINAPI*)();
Yield_t g_yield = nullptr;
constexpr int kReadUs = 2, kIdleUs = 20;
int64_t g_read_ticks = 1, g_idle_ticks = 1;  // QPC ticks
using ReplayChunk_t = uint64_t (*)(void* chunk, void* ctx, int* offset, uint64_t terminator);
ReplayChunk_t o_ReplayChunk = nullptr;
void* o_head = nullptr;  // the head hook's trampoline: unused (the stub enters the original at the dispatch)

uint64_t hk_ReplayChunk(void* chunk, void* ctx, int* offset, uint64_t terminator) {
    const Claim outer = t_claim;
    if (outer.chunk) g_nested = g_nested + 1;
    t_claim = Claim{};
    g_entries = g_entries + 1;
    const uint64_t r = o_ReplayChunk(chunk, ctx, offset, terminator);
    t_claim = outer;
    return r;
}

}  // namespace
}  // namespace rdrvr::playback

// The next packet of `chunk` at *offset, once it is counted: from this call's claim, else a new claim. K >= 1 claims
// every packet counted so far, waiting first until at least K are or the count has not grown for kIdleUs (the recorder
// paused: a frame's end, a wait of its own, work without packets); between two reads of the count the thread waits
// about kReadUs on its own (a pause loop on the clock, after a yield), so the shared line is touched rarely. K = 0
// claims one at a time, as the original head did, yielding between reads as it did.
extern "C" char* rdrvr_rb_next(char* chunk, int* offset) {
    using namespace rdrvr::playback;
    Claim& c = t_claim;
    if (c.chunk != chunk || c.budget <= 0) {
        volatile long* count = reinterpret_cast<volatile long*>(chunk + 0x10);
        const int k = g_k.load(std::memory_order_relaxed);
        long n = *count;
        if (k < 1) {
            while (n <= 0) {  // the original's wait
                g_yield();
                n = *count;
            }
        } else if (n < k) {
            long last = n;
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            int64_t changed = t.QuadPart;
            for (;;) {
                g_yield();
                QueryPerformanceCounter(&t);
                const int64_t read_at = t.QuadPart + g_read_ticks;
                do {
                    _mm_pause();
                    QueryPerformanceCounter(&t);
                } while (t.QuadPart < read_at);
                n = *count;
                if (n != last) {
                    last = n;
                    changed = t.QuadPart;
                }
                if (n > 0 && (n >= k || t.QuadPart - changed >= g_idle_ticks)) break;
            }
            g_waits = g_waits + 1;
        }
        const long take = k >= 1 ? n : 1;
        _InterlockedExchangeAdd(count, -take);
        c.chunk = chunk;
        c.budget = take;
        c.data = *reinterpret_cast<char* volatile*>(chunk + 0x18);
        g_claims = g_claims + 1;
        g_packets = g_packets + static_cast<uint64_t>(take);
    }
    --c.budget;
    return c.data + *offset;
}

namespace rdrvr::playback {

void install() {
    const int k = config::get_int("Render", "PlaybackBatch", 256);
    if (k < 0) {
        log::info("[playback] the playback loop's head as the game's ([Render] PlaybackBatch=-1)");
        return;
    }
    if (anchors::stand_down()) return;
    g_yield = reinterpret_cast<Yield_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SwitchToThread"));
    rdrvr_rb_dispatch = reinterpret_cast<void*>(anchors::addr(anchors::Id::ReplayDispatch));
    const uintptr_t head = anchors::addr(anchors::Id::ReplayHead), entry = anchors::addr(anchors::Id::ReplayChunk);
    if (!g_yield || !rdrvr_rb_dispatch || !head || !entry) {
        log::warn("[playback] not installed: an anchor or SwitchToThread missing");
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_read_ticks = f.QuadPart * kReadUs / 1000000 + 1;
    g_idle_ticks = f.QuadPart * kIdleUs / 1000000 + 1;
    g_k = k;
    if (!hooks::install("RDR ReplayChunk (playback batch: the claim per call)", reinterpret_cast<void*>(entry), hk_ReplayChunk,
                        &o_ReplayChunk))
        return;
    if (!hooks::install("RDR ReplayChunk loop head (playback batch)", reinterpret_cast<void*>(head),
                        reinterpret_cast<void*>(&rdrvr_playback_head), &o_head))
        return;
    g_installed = true;
    log::info("[playback] the playback loop's head replaced: %s", k >= 1 ? "packets claimed in batches" : "one packet at a time");
}

bool installed() { return g_installed; }
int batch() { return g_installed ? g_k.load() : -1; }

bool set_batch(int k) {
    if (!g_installed) return false;
    g_k = k < 0 ? 0 : k;
    log::info("[playback] batch %d", g_k.load());
    return true;
}

void status(char* out, size_t len) {
    const uint64_t claims = g_claims, packets = g_packets;
    std::snprintf(out, len, "playback batch %s K %d: claims %llu, packets %llu (%.1f a claim), waits %llu, chunks %llu, nested %llu",
                  g_installed ? "installed" : "not installed", g_k.load(), static_cast<unsigned long long>(claims),
                  static_cast<unsigned long long>(packets), claims ? static_cast<double>(packets) / static_cast<double>(claims) : 0.0,
                  static_cast<unsigned long long>(g_waits), static_cast<unsigned long long>(g_entries),
                  static_cast<unsigned long long>(g_nested));
}

}  // namespace rdrvr::playback
