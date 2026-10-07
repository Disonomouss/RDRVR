#include "core/build_check.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <vector>

#include "core/log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace rdrvr::build_check {
namespace {

bool sha256_file(const wchar_t* path, char out_hex[65], uint64_t* out_size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<unsigned char> buf(1 << 20);
    uint64_t total = 0;
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got) {
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0));
        total += got;
    }
    unsigned char digest[32] = {};
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    for (int i = 0; i < 32; ++i) std::snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
    if (out_size) *out_size = total;
    return ok;
}

DWORD WINAPI worker(void*) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const char*>(&__ImageBase) +
                                                           __ImageBase.e_lfanew);
    char hex[65] = {};
    uint64_t size = 0;
    if (sha256_file(self, hex, &size)) {
        log::info("[build] core %ls  %llu bytes  sha256 %s  link time %#lx", self, size, hex,
                  nt->FileHeader.TimeDateStamp);
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (sha256_file(exe, hex, &size)) log::info("[build] game %ls  %llu bytes  sha256 %s", exe, size, hex);
    return 0;
}

}  // namespace

void log_self_and_game_async() {
    if (HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace rdrvr::build_check
