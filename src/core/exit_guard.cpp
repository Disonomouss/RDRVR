#include "core/exit_guard.h"

#include <windows.h>

#include "core/aim.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/render_res.h"

namespace rdrvr::exit_guard {
namespace {

// ntdll!RtlExitUserProcess: kernel32/kernelbase ExitProcess and the CRT's exit all end here, and it is the function that
// runs the DLL detach notifications (LdrShutdownProcess).
using RtlExitUserProcess_t = void(NTAPI*)(LONG);
RtlExitUserProcess_t o_RtlExitUserProcess = nullptr;

bool g_terminate = true;  // [Compat] TerminateAtExit

void NTAPI hk_RtlExitUserProcess(LONG status) {
    render_res::on_exit();  // a clean exit: a new render resolution's boot sentinel cleared
    aim::on_exit();         // others' shots since the last log line
    if (g_terminate) {
        log::info("[exit] process exit (status %#lx): TerminateProcess, so no DLL detach runs (RedHook's crashes)",
                  static_cast<unsigned long>(status));
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(status));
    }
    o_RtlExitUserProcess(status);  // TerminateAtExit=0: the normal exit (the DLL detach runs)
}

}  // namespace

bool install() {
    g_terminate = config::get_bool("Compat", "TerminateAtExit", true);  // off: the hook only marks the exit clean
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    void* fn = ntdll ? reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlExitUserProcess")) : nullptr;
    if (!fn) return false;
    return hooks::install("ntdll!RtlExitUserProcess", fn, hk_RtlExitUserProcess, &o_RtlExitUserProcess);
}

}  // namespace rdrvr::exit_guard
