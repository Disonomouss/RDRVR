// dinput8.dll proxy: the game imports DINPUT8!DirectInput8Create through its hidden import table, so a
// dinput8.dll next to RDR.exe is loaded during startup (DECISIONS.md D1). Every export forwards to the
// real System32 dinput8.dll, loaded on first use (never from DllMain: loader lock).

#include <windows.h>
#include <unknwn.h>

#include <atomic>
#include <cwchar>

#include "core/log.h"

namespace {

HMODULE real_module() {
    static std::atomic<HMODULE> g_real{nullptr};
    HMODULE m = g_real.load();
    if (m) return m;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    std::swprintf(path + n, MAX_PATH - n, L"\\dinput8.dll");
    m = LoadLibraryW(path);
    if (!m) {
        rdrvr::log::error("[proxy] could not load %ls (error %lu)", path, GetLastError());
        return nullptr;
    }
    HMODULE expected = nullptr;
    if (!g_real.compare_exchange_strong(expected, m)) {
        FreeLibrary(m);
        return expected;
    }
    rdrvr::log::info("[proxy] real dinput8 loaded from %ls", path);
    return m;
}

template <typename Fn>
Fn real(const char* name) {
    HMODULE m = real_module();
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    static Fn fn = real<Fn>("DirectInput8Create");
    rdrvr::log::limited("proxy.DirectInput8Create", 4, "[proxy] DirectInput8Create(version %#lx)", version);
    return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    static Fn fn = real<Fn>("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    static Fn fn = real<Fn>("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    static Fn fn = real<Fn>("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    static Fn fn = real<Fn>("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

LPCVOID WINAPI Proxy_GetdfDIJoystick() {
    using Fn = LPCVOID(WINAPI*)();
    static Fn fn = real<Fn>("GetdfDIJoystick");
    return fn ? fn() : nullptr;
}

}  // extern "C"
