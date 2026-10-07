#include <windows.h>

#include <cwchar>

#include "core/bootstrap.h"

namespace {

// Other executables in the game folder (RDRMessage.exe, the crash reporter) can load DLLs from it too. The core only
// runs inside RDR.exe; anywhere else it is a plain forwarding proxy.
bool host_is_game() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"RDR.exe") == 0;
}

bool g_active = false;

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            g_active = host_is_game();
            if (g_active) rdrvr::bootstrap::on_process_attach();
            break;
        case DLL_PROCESS_DETACH:
            // reserved != nullptr: the process is exiting; other threads are gone, so only log.
            if (reserved && g_active) rdrvr::bootstrap::on_process_detach();
            break;
    }
    return TRUE;
}
