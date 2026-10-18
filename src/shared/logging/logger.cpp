#include "shared/logging/logger.h"
#include "app/config.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>

namespace logger {

static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static HANDLE g_reportFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;
static constexpr LONG INIT_PENDING = 0;
static constexpr LONG INIT_BUSY    = 1;
static constexpr LONG INIT_DONE    = 2;
static volatile LONG g_initState = INIT_PENDING;
static constexpr int INIT_BUSY_WAIT_MS = 1000;

static char g_reportSession[160] = {};
static char g_reportConfig[128] = {};
static char g_reportMemory[160] = {"mem=not_available"};
static char g_reportAllocator[160] = {"alloc=not_available"};
static char g_reportHotspot[128] = {"hotspot=not_available"};
static char g_reportEvent[128] = {"event=startup"};
static char g_reportCrash[160] = {"crash=not_captured_or_session_still_running"};

static void append_report_line(char* output, size_t capacity,
                               size_t* used, const char* line) {
    if (!output || !used || !line || !line[0] || *used >= capacity) return;
    const size_t remaining = capacity - *used;
    int written = snprintf(output + *used, remaining + 1, "%s\r\n", line);
    if (written < 0) return;
    if (static_cast<size_t>(written) > remaining) {
        *used = capacity;
        output[capacity] = '\0';
    } else {
        *used += static_cast<size_t>(written);
    }
}

static void rewrite_compact_report_locked() {
    if (g_reportFile == INVALID_HANDLE_VALUE) return;

    char output[CRASH_REPORT_MAX_BYTES + 1] = {};
    size_t used = 0;
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used,
                       "DLL Crash Report | paste this entire file into one Steam comment");
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used, g_reportSession);
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used, g_reportConfig);
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used, g_reportCrash);
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used, g_reportEvent);
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used, g_reportMemory);
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used, g_reportAllocator);
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used, g_reportHotspot);
    append_report_line(output, CRASH_REPORT_MAX_BYTES, &used,
                       "Also state: active mods, last action, dialog text or silent desktop crash.");

    SetFilePointer(g_reportFile, 0, nullptr, FILE_BEGIN);
    DWORD written = 0;
    WriteFile(g_reportFile, output, static_cast<DWORD>(used), &written, nullptr);
    SetEndOfFile(g_reportFile);
}

static void update_report_field(char* target, size_t target_size,
                                bool flush_immediately,
                                const char* fmt, va_list args) {
    if (g_initState != INIT_DONE || !target || target_size == 0 || !fmt) return;
    EnterCriticalSection(&g_logLock);
    vsnprintf(target, target_size, fmt, args);
    target[target_size - 1] = '\0';
    rewrite_compact_report_locked();
    if (flush_immediately && g_reportFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_reportFile);
    }
    LeaveCriticalSection(&g_logLock);
}

static const char* architecture_name(WORD architecture) {
    switch (architecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        default: return "unknown";
    }
}

static void initialize_report_header() {
    using WineGetVersionFn = const char* (__cdecl*)();
    const char* environment = "native-windows";
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto wine_get_version = reinterpret_cast<WineGetVersionFn>(
            GetProcAddress(ntdll, "wine_get_version"));
        if (wine_get_version) {
            const char* version = wine_get_version();
            if (version && version[0]) environment = version;
            else environment = "wine/proton";
        }
    }

    SYSTEM_INFO native_info = {};
    GetNativeSystemInfo(&native_info);
    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    const BOOL have_memory = GlobalMemoryStatusEx(&memory);

    snprintf(g_reportSession, sizeof(g_reportSession),
             "session v=%s build=%s_%s pid=%lu env=%s process=x86 native=%s wow64=%s ram=%lluMiB",
             MEMORY_GUARD_VERSION, __DATE__, __TIME__,
             static_cast<unsigned long>(GetCurrentProcessId()), environment,
             architecture_name(native_info.wProcessorArchitecture), wow64 ? "yes" : "no",
             have_memory ? static_cast<unsigned long long>(memory.ullTotalPhys) / (1024ull * 1024ull) : 0ull);
    snprintf(g_reportConfig, sizeof(g_reportConfig),
             "guard=%u/%u/%uMiB thresholds=%u/%u,%u/%u,%u/%uMiB report_max=%uB",
             PRIMARY_RESERVE_MB, CRITICAL_RESERVE_MB, FINAL_RESERVE_MB,
             RELEASE_TOTAL_FREE_MB, RELEASE_LARGEST_FREE_MB,
             CRITICAL_TOTAL_FREE_MB, CRITICAL_LARGEST_FREE_MB,
             FINAL_TOTAL_FREE_MB, FINAL_LARGEST_FREE_MB,
             CRASH_REPORT_MAX_BYTES);
}

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
        if (g_reportFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_reportFile);
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

    wchar_t reportPath[MAX_PATH] = {0};
    _snwprintf_s(reportPath, MAX_PATH, _TRUNCATE,
                 L"%s\\%s", dllDir, CRASH_REPORT_FILE_NAME);
    g_reportFile = CreateFileW(
        reportPath,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    const DWORD report_create_error = g_reportFile == INVALID_HANDLE_VALUE
        ? GetLastError() : ERROR_SUCCESS;
    initialize_report_header();

    InterlockedExchange(&g_initState, INIT_DONE);

    EnterCriticalSection(&g_logLock);
    rewrite_compact_report_locked();
    LeaveCriticalSection(&g_logLock);

    log("============================================================");
    log("mowas2-memory-guard v%s loaded (experimental allocator diagnostics; no SteamMatchmaking hooks)",
        MEMORY_GUARD_VERSION);
    log("Build: %s %s", __DATE__, __TIME__);
    log("Session: pid=%lu log=%ws", static_cast<unsigned long>(GetCurrentProcessId()), logPath);
    log("Compact report: %ws (hard limit %u ASCII bytes)",
        CRASH_REPORT_FILE_NAME, CRASH_REPORT_MAX_BYTES);
    if (g_reportFile == INVALID_HANDLE_VALUE) {
        log("Compact report creation failed err=%lu", report_create_error);
    }
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

void report_memory(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    update_report_field(g_reportMemory, sizeof(g_reportMemory), false, fmt, args);
    va_end(args);
}

void report_allocator(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    update_report_field(g_reportAllocator, sizeof(g_reportAllocator), false, fmt, args);
    va_end(args);
}

void report_hotspot(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    update_report_field(g_reportHotspot, sizeof(g_reportHotspot), false, fmt, args);
    va_end(args);
}

void report_event(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    update_report_field(g_reportEvent, sizeof(g_reportEvent), true, fmt, args);
    va_end(args);
}

void report_crash(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    update_report_field(g_reportCrash, sizeof(g_reportCrash), true, fmt, args);
    va_end(args);
}

void flush_now() {
    if (g_initState != INIT_DONE) return;
    EnterCriticalSection(&g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_logFile);
    }
    if (g_reportFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_reportFile);
    }
    LeaveCriticalSection(&g_logLock);
}

}  // namespace logger
