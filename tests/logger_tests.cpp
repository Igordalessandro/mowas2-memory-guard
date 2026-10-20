#include <windows.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>

static wchar_t fake_module[MAX_PATH] = {};
static BOOL WINAPI fake_module_handle(DWORD, LPCWSTR, HMODULE* module) {
    *module = reinterpret_cast<HMODULE>(1);
    return TRUE;
}
static DWORD WINAPI fake_module_filename(HMODULE, LPWSTR output, DWORD capacity) {
    const size_t length = std::wcslen(fake_module);
    if (length >= capacity) return capacity;
    std::wmemcpy(output, fake_module, length + 1);
    return static_cast<DWORD>(length);
}
static HANDLE WINAPI suppress_worker(LPSECURITY_ATTRIBUTES, SIZE_T,
                                     LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD) {
    return nullptr;
}

// Exercise actual file handling and synchronization in an isolated directory;
// suppress only module discovery and the infinite periodic flushing worker.
#define GetModuleHandleExW fake_module_handle
#define GetModuleFileNameW fake_module_filename
#define CreateThread suppress_worker
#include "../src/shared/logging/logger.cpp"
#undef GetModuleHandleExW
#undef GetModuleFileNameW
#undef CreateThread

namespace {
wchar_t directory[MAX_PATH] = {};

std::wstring path(const wchar_t* name) {
    return std::wstring(directory) + L"\\" + name;
}

void write(const wchar_t* name, const char* content) {
    HANDLE file = CreateFileW(path(name).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    assert(WriteFile(file, content, static_cast<DWORD>(std::strlen(content)), &written, nullptr));
    assert(written == std::strlen(content));
    CloseHandle(file);
}

std::string read(const wchar_t* name) {
    HANDLE file = CreateFileW(path(name).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(file != INVALID_HANDLE_VALUE);
    const DWORD size = GetFileSize(file, nullptr);
    assert(size <= 4096);
    std::string content(size, '\0');
    DWORD count = 0;
    assert(ReadFile(file, content.data(), size, &count, nullptr));
    assert(count == size);
    CloseHandle(file);
    return content;
}

void close_logger() {
    for (HANDLE* file : {&logger::g_logFile, &logger::g_reportFile, &logger::g_emergencyFile}) {
        if (*file != INVALID_HANDLE_VALUE) CloseHandle(*file);
        *file = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&logger::g_logLock);
    InterlockedExchange(&logger::g_initState, logger::INIT_PENDING);
}

void previous_reports_survive() {
    write(L"DLL Crash Report.txt", "prior compact report\r\n");
    write(L"DLL Emergency Crash.txt", "prior emergency report\r\n");
    assert(logger::init());
    assert(read(L"DLL Crash Report.previous.txt") == "prior compact report\r\n");
    assert(read(L"DLL Emergency Crash.previous.txt") == "prior emergency report\r\n");
    logger::report_crash("crash=unhandled code=C0000005 thread=4294967295");
    logger::report_failure("failure=HeapAlloc bytes=4294967295 retries=4 result=failed");
    logger::report_memory("mem free=0MiB largest=1KiB commit=4095MiB req=9223372036854775807");
    std::string noisy(300, '9');
    logger::report_allocator("alloc=%s", noisy.c_str());
    logger::report_hotspot("hot=%s", noisy.c_str());
    logger::report_event("event=bad\n\t\xFF");
    const std::string report = read(L"DLL Crash Report.txt");
    assert(report.size() <= 900 && report.size() >= 2);
    assert(report.substr(report.size() - 2) == "\r\n");
    assert(report.find("crash=unhandled code=C0000005") != std::string::npos);
    assert(report.find("failure=HeapAlloc") != std::string::npos);
    assert(report.find("mem free=0MiB") != std::string::npos);
    for (size_t i = 0; i < report.size(); ++i) {
        const unsigned char character = static_cast<unsigned char>(report[i]);
        if (character == '\r') assert(i + 1 < report.size() && report[i + 1] == '\n');
        else if (character == '\n') assert(i > 0 && report[i - 1] == '\r');
        else assert(character >= 32 && character <= 126);
    }
}

void busy_lock_still_records_emergency() {
    std::atomic<bool> owned{false}, release{false};
    std::thread holder([&] {
        EnterCriticalSection(&logger::g_logLock);
        owned.store(true);
        while (!release.load()) SwitchToThread();
        LeaveCriticalSection(&logger::g_logLock);
    });
    while (!owned.load()) Sleep(1);
    const bool acquired = logger::try_begin_crash_report();
    if (acquired) logger::end_crash_report();
    logger::write_emergency_crash(0xC0000005ul, reinterpret_cast<void*>(0x12345678u),
                                   GetCurrentThreadId(), 0x12345678ul, 0x11223344ul);
    release.store(true);
    holder.join();
    assert(!acquired);
    const std::string emergency = read(L"DLL Emergency Crash.txt");
    assert(emergency.find("code=0xC0000005 address=0x12345678") != std::string::npos);
    assert(emergency.find("capture=minimum") != std::string::npos);
    assert(logger::try_begin_crash_report());
    logger::end_crash_report();
}

void failed_backup_does_not_destroy_original() {
    close_logger();
    assert(DeleteFileW(path(L"DLL Crash Report.previous.txt").c_str()));
    assert(CreateDirectoryW(path(L"DLL Crash Report.previous.txt").c_str(), nullptr));
    write(L"DLL Crash Report.txt", "must survive failed backup\r\n");
    assert(logger::init());
    assert(logger::g_reportFile == INVALID_HANDLE_VALUE);
    assert(read(L"DLL Crash Report.txt") == "must survive failed backup\r\n");
    close_logger();
}

void cleanup() {
    WIN32_FIND_DATAW data = {};
    HANDLE search = FindFirstFileW(path(L"*").c_str(), &data);
    assert(search != INVALID_HANDLE_VALUE);
    do {
        if (std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            assert(RemoveDirectoryW(path(data.cFileName).c_str()));
        } else {
            assert(DeleteFileW(path(data.cFileName).c_str()));
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    assert(RemoveDirectoryW(directory));
}
}  // namespace

int main() {
    wchar_t temp[MAX_PATH] = {};
    assert(GetTempPathW(MAX_PATH, temp));
    assert(GetTempFileNameW(temp, L"mgd", 0, directory));
    assert(DeleteFileW(directory));
    assert(CreateDirectoryW(directory, nullptr));
    assert(_snwprintf_s(fake_module, MAX_PATH, _TRUNCATE, L"%s\\dbghelp.dll", directory) > 0);
    previous_reports_survive();
    busy_lock_still_records_emergency();
    failed_backup_does_not_destroy_original();
    cleanup();
    std::puts("logger tests passed");
}
