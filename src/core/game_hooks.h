#pragma once
// Hooks into RDR.exe itself (anchors verified first; nothing here installs when the anchors stand down).
//  - View census (DESIGN R0): grcViewport::SetCurrent 0x130940 counted per frame by call site, and the
//    dev-only counting probe on the per-draw globals push 0x130e60.
//  - The decrypted .text dump: RVA 0x1000..0x101000 is encrypted in the file (ENGINE-NOTES 0.1); the running image
//    is written to RDRVR_text_dump.bin so Ghidra can be patched with the real code.

#include <cstddef>

namespace rdrvr::game_hooks {

void install();                       // after anchors::verify() passed
void census_log_now();                // writes the current per-call-site table to the log
void census_text(char* out, size_t len);   // compact "rva:count/frame" list for the status file
bool dump_text_region(const char* reason);

}  // namespace rdrvr::game_hooks
