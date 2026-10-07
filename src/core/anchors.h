#pragma once
// RDR.exe addresses. The only place an RVA lives (spec: anchors.txt; generated: anchors.inc).
//
// verify() compares each code anchor's bytes in the running image with the bytes tools/gen_anchors.py
// read from research\RDR.exe. Any mismatch, or a different exe build, sets stand_down(): nothing that
// depends on an RVA may be installed then, and the log says why in one loud line.

#include <cstdint>

namespace rdrvr::anchors {

enum class Id : int {
#define ANCHOR(name, kind, rva, n, bytes) name,
#include "core/anchors.inc"
#undef ANCHOR
    Count
};

uintptr_t base();                 // RDR.exe module base
uintptr_t addr(Id id);            // base + rva
template <typename T>
T ptr(Id id) { return reinterpret_cast<T>(addr(id)); }
uint32_t rva(Id id);
const char* name(Id id);

bool verify();                    // run once after the game's code is final (after MTLX unloads)
bool stand_down();                // true if verify() failed (or has not run)
bool exe_matches();               // TimeDateStamp and SizeOfImage match the analysed build
void build_report();              // where this build keeps the anchors (verify() runs it on a mismatch; reads only)

}  // namespace rdrvr::anchors
