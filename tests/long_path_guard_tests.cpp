// Standalone Windows regression executable. Includes the production module and
// replaces its allocation/error/cwd dependencies with deterministic test doubles.
// No game process, Steam client, DLL injection, or Wine execution is required.
#include <windows.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
DWORD test_error = 0;
DWORD native_error = 0;
DWORD expected_incoming_error = 0;
unsigned allocations = 0;
unsigned frees = 0;
unsigned cwd_queries = 0;
unsigned log_calls = 0;
unsigned report_calls = 0;
bool allocation_fails = false;
bool native_succeeds = false;
std::wstring current_directory = L"C:\\Games\\Assault Squad 2";
std::wstring expected_wide_path;
std::string expected_ansi_path;

DWORD WINAPI mock_get_last_error() { return test_error; }
void WINAPI mock_set_last_error(DWORD error) { test_error = error; }
LPVOID WINAPI mock_heap_alloc(HANDLE, DWORD, SIZE_T size) {
    ++allocations;
    test_error = 0xA110C;
    return allocation_fails ? nullptr : std::malloc(size);
}
BOOL WINAPI mock_heap_free(HANDLE, DWORD, LPVOID block) {
    ++frees;
    std::free(block);
    test_error = 0xFEEE;
    return TRUE;
}
DWORD WINAPI mock_current_directory(DWORD capacity, LPWSTR output) {
    ++cwd_queries;
    test_error = 0xC0D;
    const DWORD length = static_cast<DWORD>(current_directory.size());
    if (capacity <= length) return length + 1;
    std::wmemcpy(output, current_directory.c_str(), length + 1);
    return length;
}
}

#define GetLastError mock_get_last_error
#define SetLastError mock_set_last_error
#define HeapAlloc mock_heap_alloc
#define HeapFree mock_heap_free
#define GetCurrentDirectoryW mock_current_directory
#include "../src/compat/long_path_guard.cpp"
#undef GetLastError
#undef SetLastError
#undef HeapAlloc
#undef HeapFree
#undef GetCurrentDirectoryW

namespace core { void register_module(const ModuleSpec&) {} }
namespace logger {
void log(const char*, ...) { ++log_calls; test_error = 0x106; }
void report_event(const char*, ...) { ++report_calls; test_error = 0xE7E; }
}

namespace {
void native_call() {
    assert(test_error == expected_incoming_error);
    test_error = native_error;
}
HANDLE native_handle() {
    return native_succeeds ? reinterpret_cast<HANDLE>(0x1234) : INVALID_HANDLE_VALUE;
}
HANDLE WINAPI create_a(LPCSTR path, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                       DWORD, DWORD, HANDLE) {
    assert(expected_ansi_path == path);
    native_call();
    return native_handle();
}
HANDLE WINAPI create_w(LPCWSTR path, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                       DWORD, DWORD, HANDLE) {
    assert(expected_wide_path == path);
    native_call();
    return native_handle();
}
HANDLE WINAPI find_w(LPCWSTR path, LPWIN32_FIND_DATAW) {
    assert(expected_wide_path == path);
    native_call();
    return native_handle();
}
HANDLE WINAPI find_ex_a(LPCSTR path, FINDEX_INFO_LEVELS, LPVOID,
                         FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    assert(expected_ansi_path == path);
    native_call();
    return native_handle();
}
HANDLE WINAPI find_ex_w(LPCWSTR path, FINDEX_INFO_LEVELS, LPVOID,
                         FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    assert(expected_wide_path == path);
    native_call();
    return native_handle();
}
DWORD WINAPI attributes_w(LPCWSTR path) {
    assert(expected_wide_path == path);
    native_call();
    return native_succeeds ? FILE_ATTRIBUTE_NORMAL : INVALID_FILE_ATTRIBUTES;
}
BOOL WINAPI attributes_ex_w(LPCWSTR path, GET_FILEEX_INFO_LEVELS, LPVOID) {
    assert(expected_wide_path == path);
    native_call();
    return native_succeeds ? TRUE : FALSE;
}
void reset() {
    test_error = expected_incoming_error = 0xBEEF;
    native_error = ERROR_FILE_NOT_FOUND;
    native_succeeds = false;
    allocation_fails = false;
    allocations = frees = cwd_queries = log_calls = report_calls = 0;
    long_path_guard::g_rewrites = 0;
    long_path_guard::g_failures = 0;
    long_path_guard::g_reported = 0;
}
}

int main() {
    using namespace long_path_guard;
    g_create_file_a = &create_a;
    g_create_file_w = &create_w;
    g_find_first_file_w = &find_w;
    g_find_first_file_ex_a = &find_ex_a;
    g_find_first_file_ex_w = &find_ex_w;
    g_get_file_attributes_w = &attributes_w;
    g_get_file_attributes_ex_w = &attributes_ex_w;

    // Short absolute paths preserve caller/native errors without allocation or
    // cwd lookup. Successful OPEN_ALWAYS must retain ERROR_ALREADY_EXISTS.
    reset();
    expected_wide_path = L"C:\\game\\save.dat";
    native_succeeds = true;
    native_error = ERROR_ALREADY_EXISTS;
    assert(guarded_create_file_w(expected_wide_path.c_str(), 0, 0, nullptr,
                                  OPEN_ALWAYS, 0, nullptr) == native_handle());
    assert(test_error == ERROR_ALREADY_EXISTS);
    assert(allocations == 0 && cwd_queries == 0);

    const std::wstring long_path = L"C:\\game\\" + std::wstring(250, L'a');
    const std::string long_path_a(long_path.begin(), long_path.end());
    expected_wide_path = L"\\\\?\\" + long_path;
    expected_ansi_path = "\\\\?\\" + long_path_a;

    // Every hooked API must preserve errors after both logging and HeapFree.
    reset();
    assert(guarded_create_file_w(long_path.c_str(), 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error && frees > 0);
    test_error = expected_incoming_error;
    assert(guarded_create_file_a(long_path_a.c_str(), 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error);
    test_error = expected_incoming_error;
    assert(guarded_find_first_file_w(long_path.c_str(), nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error);
    test_error = expected_incoming_error;
    assert(guarded_find_first_file_ex_a(long_path_a.c_str(), FindExInfoStandard,
        nullptr, FindExSearchNameMatch, nullptr, 0) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error);
    test_error = expected_incoming_error;
    assert(guarded_find_first_file_ex_w(long_path.c_str(), FindExInfoStandard,
        nullptr, FindExSearchNameMatch, nullptr, 0) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error);
    test_error = expected_incoming_error;
    assert(guarded_get_file_attributes_w(long_path.c_str()) == INVALID_FILE_ATTRIBUTES);
    assert(test_error == native_error);
    test_error = expected_incoming_error;
    assert(!guarded_get_file_attributes_ex_w(long_path.c_str(), GetFileExInfoStandard, nullptr));
    assert(test_error == native_error);

    reset();
    native_succeeds = true;
    native_error = ERROR_ALREADY_EXISTS;
    assert(guarded_create_file_a(long_path_a.c_str(), 0, 0, nullptr,
                                  CREATE_ALWAYS, 0, nullptr) == native_handle());
    assert(test_error == ERROR_ALREADY_EXISTS && frees > 0 && log_calls > 0);
    test_error = expected_incoming_error;
    assert(guarded_create_file_w(long_path.c_str(), 0, 0, nullptr,
                                  OPEN_ALWAYS, 0, nullptr) == native_handle());
    assert(test_error == ERROR_ALREADY_EXISTS);

    // A short relative input in a deep directory needs the same rewrite.
    reset();
    current_directory = L"C:\\" + std::wstring(245, L'd');
    expected_wide_path = L"\\\\?\\" + current_directory + L"\\mod.pak";
    assert(guarded_create_file_w(L"mod.pak", 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error && allocations > 0 && log_calls == 1);
    expected_ansi_path.assign(expected_wide_path.begin(), expected_wide_path.end());
    test_error = expected_incoming_error;
    assert(guarded_create_file_a("mod.pak", 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error);

    // Root-relative UNC paths retain the server/share, not the current
    // directory's deeper components or just its initial two backslashes.
    reset();
    current_directory = L"\\\\server\\share\\deep";
    const std::wstring unc_relative = L"\\mods\\" + std::wstring(240, L'm') + L"\\x";
    expected_wide_path = L"\\\\?\\UNC\\server\\share" + unc_relative;
    assert(guarded_create_file_w(unc_relative.c_str(), 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error && allocations > 0);
    const std::string unc_relative_a(unc_relative.begin(), unc_relative.end());
    expected_ansi_path.assign(expected_wide_path.begin(), expected_wide_path.end());
    test_error = expected_incoming_error;
    assert(guarded_create_file_a(unc_relative_a.c_str(), 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error);

    // Drive-relative paths use per-drive working directories. We deliberately
    // let the native API resolve them instead of guessing from the process cwd.
    reset();
    current_directory = L"C:\\" + std::wstring(245, L'd');
    expected_wide_path = L"C:foo";
    assert(guarded_create_file_w(L"C:foo", 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error && g_rewrites == 0);
    expected_ansi_path = "C:foo";
    test_error = expected_incoming_error;
    assert(guarded_create_file_a("C:foo", 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error && g_rewrites == 0);

    // Failed preparation falls back to the caller's path and original error.
    reset();
    allocation_fails = true;
    expected_wide_path = long_path;
    assert(guarded_create_file_w(long_path.c_str(), 0, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    assert(test_error == native_error);

    // Routine missing wildcard probes are counted, but their output is bounded
    // and they never rewrite/flush the compact report for each failure.
    reset();
    for (unsigned i = 0; i < 10000; ++i) {
        record_result("FindFirstFileW", 300, true, false,
                      (i & 1u) ? ERROR_FILE_NOT_FOUND : ERROR_NO_MORE_FILES);
    }
    assert(g_failures == 10000 && g_rewrites == 10000);
    assert(log_calls < 100 && report_calls == 1);
    return 0;
}
