// version_proxy.cpp
//
// Forwards all version.dll exports to the real System32\version.dll.
// Loaded lazily on first call — safe because DllMain has already returned.

#include "proxy/version_proxy.h"
#include "core/logging.h"

#include <windows.h>
#include <cwchar>

namespace {

// Typedefs matching the real version.dll signatures.
using PFN_GetFileVersionInfoA      = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
using PFN_GetFileVersionInfoW      = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
using PFN_GetFileVersionInfoExA    = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
using PFN_GetFileVersionInfoExW    = BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
using PFN_GetFileVersionInfoSizeA  = DWORD(WINAPI*)(LPCSTR, LPDWORD);
using PFN_GetFileVersionInfoSizeW  = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
using PFN_GetFileVersionInfoSizeExA = DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD);
using PFN_GetFileVersionInfoSizeExW = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);
using PFN_VerFindFileA     = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
using PFN_VerFindFileW     = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
using PFN_VerInstallFileA  = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
using PFN_VerInstallFileW  = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
using PFN_VerLanguageNameA = DWORD(WINAPI*)(DWORD, LPSTR, DWORD);
using PFN_VerLanguageNameW = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
using PFN_VerQueryValueA   = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
using PFN_VerQueryValueW   = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

#define SLOT(name) PFN_##name g_##name = nullptr;
SLOT(GetFileVersionInfoA)
SLOT(GetFileVersionInfoW)
SLOT(GetFileVersionInfoExA)
SLOT(GetFileVersionInfoExW)
SLOT(GetFileVersionInfoSizeA)
SLOT(GetFileVersionInfoSizeW)
SLOT(GetFileVersionInfoSizeExA)
SLOT(GetFileVersionInfoSizeExW)
SLOT(VerFindFileA)
SLOT(VerFindFileW)
SLOT(VerInstallFileA)
SLOT(VerInstallFileW)
SLOT(VerLanguageNameA)
SLOT(VerLanguageNameW)
SLOT(VerQueryValueA)
SLOT(VerQueryValueW)
#undef SLOT

HMODULE g_real = nullptr;
bool g_resolved = false;

}  // namespace

void EnsureRealVersionDllLoaded() {
    if (g_resolved) return;
    g_resolved = true;

    wchar_t sys[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sys, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    wchar_t path[MAX_PATH] = {};
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\version.dll", sys) < 0) return;

    g_real = LoadLibraryW(path);
    if (!g_real) {
        aor::Log("[proxy] failed to load real version.dll: %lu", GetLastError());
        return;
    }

#define RESOLVE(name) \
    g_##name = reinterpret_cast<PFN_##name>(GetProcAddress(g_real, #name)); \
    if (!g_##name) aor::Log("[proxy] missing: %s", #name);
    RESOLVE(GetFileVersionInfoA)
    RESOLVE(GetFileVersionInfoW)
    RESOLVE(GetFileVersionInfoExA)
    RESOLVE(GetFileVersionInfoExW)
    RESOLVE(GetFileVersionInfoSizeA)
    RESOLVE(GetFileVersionInfoSizeW)
    RESOLVE(GetFileVersionInfoSizeExA)
    RESOLVE(GetFileVersionInfoSizeExW)
    RESOLVE(VerFindFileA)
    RESOLVE(VerFindFileW)
    RESOLVE(VerInstallFileA)
    RESOLVE(VerInstallFileW)
    RESOLVE(VerLanguageNameA)
    RESOLVE(VerLanguageNameW)
    RESOLVE(VerQueryValueA)
    RESOLVE(VerQueryValueW)
#undef RESOLVE

    aor::Log("[proxy] real version.dll loaded at %p", g_real);
}

// ── Forwarders ─────────────────────────────────────────────────────────────

extern "C" {

BOOL WINAPI GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    EnsureRealVersionDllLoaded();
    return g_GetFileVersionInfoA ? g_GetFileVersionInfoA(a, b, c, d) : FALSE;
}
BOOL WINAPI GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    EnsureRealVersionDllLoaded();
    return g_GetFileVersionInfoW ? g_GetFileVersionInfoW(a, b, c, d) : FALSE;
}
BOOL WINAPI GetFileVersionInfoExA(DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e) {
    EnsureRealVersionDllLoaded();
    return g_GetFileVersionInfoExA ? g_GetFileVersionInfoExA(a, b, c, d, e) : FALSE;
}
BOOL WINAPI GetFileVersionInfoExW(DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e) {
    EnsureRealVersionDllLoaded();
    return g_GetFileVersionInfoExW ? g_GetFileVersionInfoExW(a, b, c, d, e) : FALSE;
}
DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b) {
    EnsureRealVersionDllLoaded();
    return g_GetFileVersionInfoSizeA ? g_GetFileVersionInfoSizeA(a, b) : 0;
}
DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b) {
    EnsureRealVersionDllLoaded();
    return g_GetFileVersionInfoSizeW ? g_GetFileVersionInfoSizeW(a, b) : 0;
}
DWORD WINAPI GetFileVersionInfoSizeExA(DWORD a, LPCSTR b, LPDWORD c) {
    EnsureRealVersionDllLoaded();
    return g_GetFileVersionInfoSizeExA ? g_GetFileVersionInfoSizeExA(a, b, c) : 0;
}
DWORD WINAPI GetFileVersionInfoSizeExW(DWORD a, LPCWSTR b, LPDWORD c) {
    EnsureRealVersionDllLoaded();
    return g_GetFileVersionInfoSizeExW ? g_GetFileVersionInfoSizeExW(a, b, c) : 0;
}
DWORD WINAPI VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h) {
    EnsureRealVersionDllLoaded();
    return g_VerFindFileA ? g_VerFindFileA(a, b, c, d, e, f, g, h) : 0;
}
DWORD WINAPI VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h) {
    EnsureRealVersionDllLoaded();
    return g_VerFindFileW ? g_VerFindFileW(a, b, c, d, e, f, g, h) : 0;
}
DWORD WINAPI VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f, LPSTR g, PUINT h) {
    EnsureRealVersionDllLoaded();
    return g_VerInstallFileA ? g_VerInstallFileA(a, b, c, d, e, f, g, h) : 0;
}
DWORD WINAPI VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h) {
    EnsureRealVersionDllLoaded();
    return g_VerInstallFileW ? g_VerInstallFileW(a, b, c, d, e, f, g, h) : 0;
}
DWORD WINAPI VerLanguageNameA(DWORD a, LPSTR b, DWORD c) {
    EnsureRealVersionDllLoaded();
    return g_VerLanguageNameA ? g_VerLanguageNameA(a, b, c) : 0;
}
DWORD WINAPI VerLanguageNameW(DWORD a, LPWSTR b, DWORD c) {
    EnsureRealVersionDllLoaded();
    return g_VerLanguageNameW ? g_VerLanguageNameW(a, b, c) : 0;
}
BOOL WINAPI VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d) {
    EnsureRealVersionDllLoaded();
    return g_VerQueryValueA ? g_VerQueryValueA(a, b, c, d) : FALSE;
}
BOOL WINAPI VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d) {
    EnsureRealVersionDllLoaded();
    return g_VerQueryValueW ? g_VerQueryValueW(a, b, c, d) : FALSE;
}

}  // extern "C"
