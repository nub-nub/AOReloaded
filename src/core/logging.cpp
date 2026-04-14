#include "core/logging.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace aor {

namespace {

HANDLE g_log_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_log_cs;
bool g_cs_init = false;

}  // namespace

void LogInit() {
    if (!g_cs_init) {
        InitializeCriticalSection(&g_log_cs);
        g_cs_init = true;
    }

    // CREATE_ALWAYS truncates the file on each launch — clean slate.
    // FILE_SHARE_READ | FILE_SHARE_DELETE lets other tools tail the file.
    g_log_file = CreateFileW(
        L"AOReloaded.log",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (g_log_file != INVALID_HANDLE_VALUE) {
        // Session header so it's obvious where a run starts if the file
        // somehow isn't truncated (e.g. opened with append externally).
        SYSTEMTIME st;
        GetLocalTime(&st);
        char hdr[128];
        int len = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
            "=== AOReloaded session %04u-%02u-%02u %02u:%02u:%02u ===\r\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
        if (len > 0) {
            DWORD w;
            WriteFile(g_log_file, hdr, static_cast<DWORD>(len), &w, nullptr);
        }
    }
}

void LogShutdown() {
    if (g_log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
    if (g_cs_init) {
        DeleteCriticalSection(&g_log_cs);
        g_cs_init = false;
    }
}

void Log(const char* fmt, ...) {
    if (g_log_file == INVALID_HANDLE_VALUE) return;

    char buf[2048];
    int prefix_len = 0;

    // Timestamp: ticks since process start (cheap, monotonic).
    DWORD ticks = GetTickCount();
    prefix_len = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[%8lu.%03lu] ", ticks / 1000, ticks % 1000);
    if (prefix_len < 0) return;

    va_list args;
    va_start(args, fmt);
    int msg_len = _vsnprintf_s(
        buf + prefix_len, sizeof(buf) - prefix_len, _TRUNCATE, fmt, args);
    va_end(args);
    if (msg_len < 0) msg_len = static_cast<int>(strlen(buf + prefix_len));

    int total = prefix_len + msg_len;
    if (total + 2 < static_cast<int>(sizeof(buf))) {
        buf[total++] = '\r';
        buf[total++] = '\n';
    }

    EnterCriticalSection(&g_log_cs);
    DWORD written = 0;
    WriteFile(g_log_file, buf, static_cast<DWORD>(total), &written, nullptr);
    FlushFileBuffers(g_log_file);
    LeaveCriticalSection(&g_log_cs);
}

}  // namespace aor
