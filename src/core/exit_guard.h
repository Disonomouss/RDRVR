#pragma once
// RedHook v0.8's shutdown runs twice when the game quits: once in the game's exit sequence and again from its
// DLL_PROCESS_DETACH, where 0x18000eb80 reads manager+0x98 (null: never set with DirectXHook disabled, as RDRVR runs it,
// or freed by the first pass) at +0x40. Every graceful quit then ended in "Red Dead Redemption exited unexpectedly"
// and a Crashpad report (2026-10-03 cycles 2, 5); with this guard, cycle 6 quit cleanly.
// With [Compat] TerminateAtExit=1 (default) an inline hook on ntdll!RtlExitUserProcess (where ExitProcess and the CRT's
// exit end up, and which runs the DLL detach notifications) ends the process with TerminateProcess instead. By then the
// game's CRT has run its atexit handlers and static destructors; only the DLL detach routines (RedHook's among them)
// are skipped.

namespace rdrvr::exit_guard {

bool install();

}  // namespace rdrvr::exit_guard
