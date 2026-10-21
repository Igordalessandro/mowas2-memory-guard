// Calls the production engine wrapper with native delegates replaced by
// in-process test functions. No executable patching or game launch.
#include <windows.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

#include "../src/orchestration/engine_alloc.cpp"

uintptr_t g_gameBase = 0x00400000;

namespace {
std::vector<std::string> events;
unsigned native_calls = 0;
unsigned available_stages = 0;
unsigned success_on_call = 1;
std::size_t seen_size = 0;
std::uintptr_t seen_caller = 0;
constexpr DWORD incoming_error = 1234;
constexpr DWORD native_error = 5678;
void* result_pointer = reinterpret_cast<void*>(0x12340000u);
bool nested = false;

void reset() {
    events.clear();
    native_calls = available_stages = 0;
    success_on_call = 1;
    seen_size = seen_caller = 0;
    nested = false;
    SetLastError(incoming_error);
}

void* __cdecl native_allocate(std::size_t size) {
    assert(GetLastError() == incoming_error);
    seen_size = size;
    ++native_calls;
    events.emplace_back("native");
    SetLastError(native_error);
    return success_on_call && native_calls >= success_on_call ? result_pointer : nullptr;
}
void* __cdecl native_nested(std::size_t size) {
    assert(!nested);
    nested = true;
    return guarded_allocate(&native_allocate, size, "inner", g_gameBase + 0x2222);
}
}

namespace memory_guard {
void before_allocation(std::size_t size, std::uintptr_t caller) {
    seen_size = size;
    seen_caller = caller;
    events.emplace_back("preflight");
    SetLastError(99);
}
void record_request(std::size_t) { SetLastError(99); }
ReleasedReserve release_after_failure() {
    events.emplace_back("release");
    SetLastError(99);
    if (!available_stages) return {0, false};
    --available_stages;
    return {8u * 1024u * 1024u, true};
}
void rearm_recovery_if_needed() { events.emplace_back("rearm"); SetLastError(99); }
void capture_failure_snapshot(const char*, std::size_t, std::uintptr_t caller,
                               const char* phase) {
    assert(caller == 0x1234);
    events.emplace_back(phase);
    SetLastError(99);
}
void record_failure(std::size_t, const char*, std::size_t, unsigned, bool) {
    events.emplace_back("failure_log");
    SetLastError(99);
}
}
namespace allocator_profiler {
void begin_engine_request(std::size_t, std::uintptr_t) {}
void end_engine_request(void*, bool, bool) { events.emplace_back("end"); SetLastError(99); }
void log_flight_recorder() { SetLastError(99); }
void log_large_allocation_history() { events.emplace_back("large_history"); SetLastError(99); }
}
namespace logger { void log(const char*, ...) { SetLastError(99); } }

int main() {
    reset();
    assert(guarded_allocate(&native_allocate, 44, "test", g_gameBase + 0x1234) == result_pointer);
    assert(seen_size == 44 && seen_caller == 0x1234);
    assert((events == std::vector<std::string>{"preflight", "native", "end"}));
    assert(GetLastError() == native_error);

    reset();
    success_on_call = 2;
    available_stages = 1;
    assert(guarded_allocate(&native_allocate, 100, "test", g_gameBase + 0x1234) == result_pointer);
    assert((events == std::vector<std::string>{"preflight", "native", "initial", "release",
        "native", "retry_ok", "end", "rearm", "failure_log", "large_history"}));
    assert(GetLastError() == native_error);

    reset();
    success_on_call = 0;
    available_stages = 1;
    assert(!guarded_allocate(&native_allocate, 100, "test", g_gameBase + 0x1234));
    assert((events == std::vector<std::string>{"preflight", "native", "initial", "release",
        "native", "release", "retry_failed", "end", "failure_log", "large_history"}));
    assert(GetLastError() == native_error);

    reset();
    assert(guarded_allocate(&native_allocate, KNOWN_BAD_LOBBY_ALLOCATION, "test",
                            g_gameBase + 0x1234) == result_pointer);
    assert(seen_size == LOBBY_ALLOCATION_REPLACEMENT);

    reset();
    assert(guarded_allocate(&native_nested, 77, "test", g_gameBase + 0x1234) == result_pointer);
    assert((events == std::vector<std::string>{"preflight", "native", "end"}));
    assert(GetLastError() == native_error);
}
