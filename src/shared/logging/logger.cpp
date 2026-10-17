#include "shared/logging/logger.h"
#include "app/config.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>

namespace logger {

static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;
static constexpr LONG INIT_PENDING = 0;
static constexpr LONG INIT_BUSY    = 1;
static constexpr LONG INIT_DONE    = 2;
static volatile LONG g_initState = INIT_PENDING;
static constexpr int INIT_BUSY_WAIT_MS = 1000;

static void rotate_session_logs(const wchar_t* dllDir) {
#if LOG_FILE_PER_RUN
    if (!dllDir || LOG_FILE_RETAINED_RUNS == 0) return;

    wchar_t pattern[MAX_PATH] = {0};
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE,
                 L"%s\\%s-*.log", dllDir, LOG_FILE_STEM);

    // Re-scan after each deletion.  This runs only once at startup, avoids
    // heap allocation, and never considers files outside our exact prefix.
    for (unsigned pass = 0; pass < 128; ++pass) {
        WIN32_FIND_DATAW data = {};
        HANDLE search = FindFirstFileW(pattern, &data);
        if (search == INVALID_HANDLE_VALUE) return;

        unsigned count = 0;
        bool have_oldest = false;
        FILETIME oldest_time = {};
        wchar_t oldest_path[MAX_PATH] = {0};
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ++count;
            if (!have_oldest || CompareFileTime(&data.ftLastWriteTime, &oldest_time) < 0) {
                oldest_time = data.ftLastWriteTime;
                _snwprintf_s(oldest_path, MAX_PATH, _TRUNCATE,
                             L"%s\\%s", dllDir, data.cFileName);
                have_oldest = true;
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);

        // Leave room for the session that is about to be created.
        if (count < LOG_FILE_RETAINED_RUNS || !have_oldest) return;
        if (!DeleteFileW(oldest_path)) return;
    }
#else
    (void)dllDir;
#endif
}

static bool get_dll_directory(wchar_t* outPath, size_t outSize) {
    if (!outPath || outSize == 0) return false;
    outPath[0] = L'\0';
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&get_dll_directory,
            &hMod) || !hMod) {
        return false;
    }
    DWORD n = GetModuleFileNameW(hMod, outPath, (DWORD)outSize);
    if (n == 0 || n >= outSize) {
        outPath[0] = L'\0';
        return false;
    }
    wchar_t* lastSlash = wcsrchr(outPath, L'\\');
    if (!lastSlash) {
        outPath[0] = L'\0';
        return false;
    }
    *lastSlash = L'\0';
    return true;
}

static DWORD WINAPI flush_thread(LPVOID) {
    for (;;) {
        Sleep(5000);
        EnterCriticalSection(&g_logLock);
        if (g_logFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_logFile);
        }
        LeaveCriticalSection(&g_logLock);
    }
}

bool init() {
    LONG prev = InterlockedCompareExchange(&g_initState, INIT_BUSY, INIT_PENDING);
    if (prev != INIT_PENDING) {
        for (int i = 0; i < INIT_BUSY_WAIT_MS && g_initState == INIT_BUSY; ++i) {
            Sleep(1);
        }
        return g_initState == INIT_DONE;
    }
    InitializeCriticalSection(&g_logLock);

#if !ENABLE_LOGGING
    InterlockedExchange(&g_initState, INIT_DONE);
    return true;
#endif

    wchar_t dllDir[MAX_PATH] = {0};
    if (!get_dll_directory(dllDir, MAX_PATH)) {
        DeleteCriticalSection(&g_logLock);
        InterlockedExchange(&g_initState, INIT_PENDING);
        return false;
    }

    wchar_t logPath[MAX_PATH] = {0};
#if LOG_FILE_PER_RUN
    rotate_session_logs(dllDir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    int dated = _snwprintf_s(logPath, MAX_PATH, _TRUNCATE,
                             L"%s\\%s-%04u-%02u-%02u-%02u%02u%02u-%03u-p%lu.log",
                             dllDir, LOG_FILE_STEM,
                             st.wYear, st.wMonth, st.wDay,
                             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                             static_cast<unsigned long>(GetCurrentProcessId()));
    if (dated < 0) {
        _snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%s\\%s", dllDir, LOG_FILE_NAME);
    }
#else
    _snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%s\\%s", dllDir, LOG_FILE_NAME);
#endif

    g_logFile = CreateFileW(
        logPath,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (g_logFile == INVALID_HANDLE_VALUE) {
        DeleteCriticalSection(&g_logLock);
        InterlockedExchange(&g_initState, INIT_PENDING);
        return false;
    }

    InterlockedExchange(&g_initState, INIT_DONE);

    log("============================================================");
    log("mowas2-memory-guard v%s loaded (experimental allocator diagnostics; no SteamMatchmaking hooks)",
        MEMORY_GUARD_VERSION);
    log("Build: %s %s", __DATE__, __TIME__);
    log("Session: pid=%lu log=%ws", static_cast<unsigned long>(GetCurrentProcessId()), logPath);
    log("============================================================");

    HANDLE flushTh = CreateThread(nullptr, 0, &flush_thread, nullptr, 0, nullptr);
    if (flushTh) CloseHandle(flushTh);

    return true;
}

static void write_line_raw(const char* fmt, va_list args) {
    if (g_initState != INIT_DONE || g_logFile == INVALID_HANDLE_VALUE) return;

    char timestamp[32];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    char message[2048];
    int msgLen = vsnprintf(message, sizeof(message) - 2, fmt, args);
    if (msgLen < 0) msgLen = 0;
    if (msgLen >= (int)sizeof(message) - 2) msgLen = (int)sizeof(message) - 2;

    char line[2200];
    int lineLen = snprintf(line, sizeof(line), "[%s] %s\r\n", timestamp, message);
    if (lineLen < 0) return;
    if (lineLen >= (int)sizeof(line)) lineLen = (int)sizeof(line) - 1;

    EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    WriteFile(g_logFile, line, (DWORD)lineLen, &written, nullptr);
    LeaveCriticalSection(&g_logLock);
}

void log(const char* fmt, ...) {
#if !ENABLE_LOGGING
    (void)fmt;
    return;
#else
    va_list args;
    va_start(args, fmt);
    write_line_raw(fmt, args);
    va_end(args);
#endif
}

void flush_now() {
    if (g_initState != INIT_DONE) return;
    EnterCriticalSection(&g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_logFile);
    }
    LeaveCriticalSection(&g_logLock);
}

}  // namespace logger
