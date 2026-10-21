// Windows regression executable; calls production wrappers with deterministic
// fake allocators. No real game, Steam, allocation pressure, or injection.
#include <windows.h>
#include <cassert>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
DWORD last_error = 0;
DWORD incoming_error = 0xBEEF;
DWORD WINAPI mock_get_last_error() { return last_error; }
void WINAPI mock_set_last_error(DWORD value) { last_error = value; }
}
#define GetLastError mock_get_last_error
#define SetLastError mock_set_last_error
#include "../src/diagnostics/allocator_profiler.cpp"
#undef GetLastError
#undef SetLastError

namespace {
unsigned release_calls = 0;
unsigned stages_available = 0;
unsigned native_calls = 0;
unsigned succeed_on_call = 0;
DWORD error_on_failure = ERROR_NOT_ENOUGH_MEMORY;
bool free_succeeds = true;
bool reuse_during_free = false;
void* sampled_pointer = nullptr;
void* replacement_pointer = nullptr;
char failure_report[192] = {};
unsigned snapshot_calls = 0;
unsigned snapshot_release_calls[2] = {};
unsigned snapshot_native_calls[2] = {};
char snapshot_phases[2][16] = {};
DWORD snapshot_errors[2] = {};
bool insert_during_logging = false;
}

namespace memory_guard {
ReleasedReserve release_after_failure() {
    ++release_calls;
    last_error = 0xBAD;
    if (!stages_available) return {0, false};
    --stages_available;
    return {8u * 1024u * 1024u, false};
}
void rearm_recovery_if_needed() { last_error = 0xBAD; }
bool is_under_pressure() { return false; }
void capture_failure_snapshot(const char*, std::size_t, std::uintptr_t, const char* phase) {
    assert(snapshot_calls < 2);
    snapshot_release_calls[snapshot_calls] = release_calls;
    snapshot_native_calls[snapshot_calls] = native_calls;
    snapshot_errors[snapshot_calls] = last_error;
    std::snprintf(snapshot_phases[snapshot_calls], sizeof(snapshot_phases[0]), "%s", phase);
    ++snapshot_calls;
    last_error = 0xBAD;
}
}
namespace core { void register_module(const ModuleSpec&) {} }
namespace logger {
void log(const char*, ...) {
    if (insert_during_logging) {
        // A producer must remain free to acquire the history lock while a
        // copied snapshot is logged; logging while holding it deadlocks.
        insert_during_logging = false;
        allocator_profiler::record_large_request(
            allocator_profiler::LargeAllocationKind::heap, 1024u * 1024u,
            20, nullptr, nullptr, 0);
    }
    last_error = 0xBAD;
}
void report_failure(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(failure_report, sizeof(failure_report), format, args);
    va_end(args);
    last_error = 0xBAD;
}
void report_allocator(const char*, ...) { last_error = 0xBAD; }
void report_hotspot(const char*, ...) { last_error = 0xBAD; }
void flush_now() { last_error = 0xBAD; }
}

namespace {
void native_call() {
    assert(last_error == incoming_error);
    ++native_calls;
}
LPVOID WINAPI allocate(HANDLE, DWORD, SIZE_T) {
    native_call();
    const bool success = succeed_on_call && native_calls >= succeed_on_call;
    last_error = success ? ERROR_ALREADY_EXISTS : error_on_failure;
    return success ? replacement_pointer : nullptr;
}
LPVOID WINAPI reallocate(HANDLE heap, DWORD flags, LPVOID, SIZE_T size) {
    return allocate(heap, flags, size);
}
BOOL WINAPI free_block(HANDLE, DWORD, LPVOID pointer) {
    native_call();
    if (reuse_during_free && free_succeeds) {
        // Simulate another thread allocating the just-freed address before
        // the original HeapFree wrapper has returned.
        assert(allocator_profiler::insert_sample(pointer, 256, 20, nullptr));
    }
    last_error = free_succeeds ? 123 : ERROR_INVALID_HANDLE;
    return free_succeeds ? TRUE : FALSE;
}
LPVOID WINAPI virtual_allocate(LPVOID, SIZE_T, DWORD, DWORD) {
    return allocate(nullptr, 0, 0);
}

void reset() {
    using namespace allocator_profiler;
    std::memset(g_callsites, 0, sizeof(g_callsites));
    std::memset(g_samples, 0, sizeof(g_samples));
    std::memset(g_heaps, 0, sizeof(g_heaps));
    g_callsites_saturated = g_sample_entries = g_sample_drops = 0;
    g_callsite_cache_valid = false;
    g_sample_live_bytes = 0;
    g_engine_context_active = true;
    g_engine_context_rva = 10;
    g_engine_context_size = 2u * 1024u * 1024u;
    g_originalHeapAlloc = &allocate;
    g_originalHeapReAlloc = &reallocate;
    g_originalHeapFree = &free_block;
    g_originalVirtualAlloc = &virtual_allocate;
    release_calls = stages_available = native_calls = succeed_on_call = 0;
    free_succeeds = true;
    reuse_during_free = false;
    error_on_failure = ERROR_NOT_ENOUGH_MEMORY;
    last_error = incoming_error;
    failure_report[0] = '\0';
    snapshot_calls = 0;
}
}

int main() {
    using namespace allocator_profiler;
    // Synthetic addresses are only identities, never dereferenced.
    for (std::uintptr_t address = 16; !replacement_pointer; address += 16) {
        auto* pointer = reinterpret_cast<void*>(address);
        if (!should_sample(pointer)) continue;
        if (!sampled_pointer) sampled_pointer = pointer;
        else replacement_pointer = pointer;
    }

    reset();
    assert(insert_sample(sampled_pointer, 100, 10, nullptr));
    const SampleEntry detached = detach_sample(sampled_pointer);
    InterlockedExchange(&g_callsites_saturated, 1);
    assert(insert_sample(replacement_pointer, 200, 20, nullptr));
    retire_sample(detached);
    assert(load64(&g_sample_live_bytes) == 200);
    assert(g_sample_entries == 1);
    retire_sample(detach_sample(replacement_pointer));
    assert(load64(&g_sample_live_bytes) == 0 && g_sample_entries == 0);
    // Site registration saturation must not retain bytes freed afterward.
    InterlockedExchange(&g_callsites_saturated, 0);
    assert(load64(&callsite_for(10)->sample_live_bytes) == 0);

    // Saturation keeps existing hot sites current without registering new
    // sites or resuming unbounded full-table searches on each allocation.
    reset();
    CallsiteStat* const tracked = callsite_for(10);
    InterlockedExchange(&g_callsites_saturated, 1);
    record_alloc_site(10, 50, false);
    assert(load64(&tracked->requested_bytes) == 50);
    assert(callsite_for(20) == nullptr);
    record_alloc_site(10, 60, false);
    assert(load64(&tracked->requested_bytes) == 110);

    reset();
    assert(insert_sample(sampled_pointer, 100, 10, nullptr));
    reuse_during_free = true;
    last_error = incoming_error;
    assert(profiled_heap_free(nullptr, 0, sampled_pointer));
    assert(last_error == 123);
    assert(g_sample_entries == 1 && load64(&g_sample_live_bytes) == 256);
    assert(detach_sample(sampled_pointer).size == 256);

    reset();
    assert(insert_sample(sampled_pointer, 100, 10, nullptr));
    free_succeeds = false;
    last_error = incoming_error;
    assert(!profiled_heap_free(nullptr, 0, sampled_pointer));
    assert(last_error == ERROR_INVALID_HANDLE);
    assert(g_sample_entries == 1 && load64(&g_sample_live_bytes) == 100);
    assert(detach_sample(sampled_pointer).size == 100);

    reset();
    assert(insert_sample(sampled_pointer, 100, 10, nullptr));
    last_error = incoming_error;
    assert(!profiled_heap_realloc(nullptr, 0, sampled_pointer, 500));
    assert(last_error == ERROR_NOT_ENOUGH_MEMORY);
    assert(g_sample_entries == 1 && load64(&g_sample_live_bytes) == 100);
    assert(detach_sample(sampled_pointer).size == 100);
    assert(snapshot_calls == 1 && snapshot_release_calls[0] == 0);
    assert(snapshot_native_calls[0] == 1);
    assert(std::strcmp(snapshot_phases[0], "initial") == 0);
    assert(snapshot_errors[0] == ERROR_NOT_ENOUGH_MEMORY);

    reset();
    stages_available = 4;
    error_on_failure = ERROR_INVALID_PARAMETER;
    assert(!profiled_virtual_alloc(nullptr, 4096, MEM_RESERVE, PAGE_READWRITE));
    assert(release_calls == 0 && native_calls == 1);
    assert(stages_available == 4 && last_error == ERROR_INVALID_PARAMETER);
    assert(snapshot_calls == 0);

    reset();
    stages_available = 4;
    assert(!profiled_virtual_alloc(sampled_pointer, 4096, MEM_COMMIT, PAGE_READWRITE));
    assert(release_calls == 0 && stages_available == 4);
    assert(std::strstr(failure_report, "failure=VirtualAlloc"));
    assert(std::strstr(failure_report, "retries=0 result=not_attempted"));
    assert(snapshot_calls == 1 && snapshot_release_calls[0] == 0);

    reset();
    stages_available = 4;
    error_on_failure = ERROR_COMMITMENT_LIMIT;
    assert(!profiled_virtual_alloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(release_calls == 0 && stages_available == 4);
    assert(last_error == ERROR_COMMITMENT_LIMIT);
    assert(std::strstr(failure_report, "err=1455"));
    assert(std::strstr(failure_report, "retries=0 result=not_attempted"));
    assert(snapshot_calls == 1 && snapshot_errors[0] == ERROR_COMMITMENT_LIMIT);

    reset();
    stages_available = 4;
    succeed_on_call = 3;
    assert(profiled_virtual_alloc(nullptr, 4096, MEM_RESERVE, PAGE_READWRITE) ==
           replacement_pointer);
    assert(release_calls == 2 && native_calls == 3 && stages_available == 2);
    assert(last_error == ERROR_ALREADY_EXISTS);
    assert(snapshot_calls == 2 && snapshot_release_calls[0] == 0);
    assert(snapshot_native_calls[0] == 1 && snapshot_native_calls[1] == 3);
    assert(snapshot_release_calls[1] == 2);
    assert(std::strcmp(snapshot_phases[1], "retry_ok") == 0);
    assert(snapshot_errors[1] == ERROR_ALREADY_EXISTS);

    reset();
    stages_available = 2;
    assert(!profiled_heap_alloc(nullptr, 0, 2u * 1024u * 1024u));
    assert(snapshot_calls == 2 && snapshot_release_calls[0] == 0);
    assert(snapshot_native_calls[0] == 1 && snapshot_native_calls[1] == 3);
    assert(std::strcmp(snapshot_phases[1], "retry_failed") == 0);
    assert(last_error == ERROR_NOT_ENOUGH_MEMORY);

    reset();
    stages_available = 2;
    succeed_on_call = 2;
    assert(profiled_heap_realloc(nullptr, HEAP_ZERO_MEMORY, nullptr,
                                 2u * 1024u * 1024u) == replacement_pointer);
    assert(snapshot_calls == 2 && snapshot_release_calls[0] == 0);
    assert(snapshot_native_calls[1] == 2 && snapshot_release_calls[1] == 1);
    assert(std::strcmp(snapshot_phases[1], "retry_ok") == 0);
    assert(last_error == ERROR_ALREADY_EXISTS);
    LargeAllocationEvent recent[1] {};
    assert(g_large_history.snapshot(recent, 1) == 1);
    assert(recent[0].kind == LargeAllocationKind::realloc);
    assert(recent[0].engine_rva == 10 && recent[0].engine_requested == 2u * 1024u * 1024u);
    assert(recent[0].request_flags == HEAP_ZERO_MEMORY);
    assert(recent[0].flags == (FLIGHT_FAILURE | FLIGHT_RECOVERED));

    reset();
    succeed_on_call = 1;
    assert(profiled_heap_alloc(nullptr, 0, 128) == replacement_pointer);
    assert(last_error == ERROR_ALREADY_EXISTS);
    assert(snapshot_calls == 0);

    // Direct low-level and enclosing engine requests remain separate records.
    reset();
    succeed_on_call = 1;
    assert(profiled_heap_alloc(nullptr, 0, 1024u * 1024u) == replacement_pointer);
    end_engine_request(replacement_pointer, false, false);
    LargeAllocationEvent nested[2] {};
    assert(g_large_history.snapshot(nested, 2) == 2);
    assert(nested[0].kind == LargeAllocationKind::heap);
    assert(nested[0].engine_rva == 10);
    assert(nested[0].requested == 1024u * 1024u);
    assert(nested[0].engine_requested == 2u * 1024u * 1024u);
    assert(nested[1].kind == LargeAllocationKind::engine);
    assert(nested[1].caller_rva == 10);
    insert_during_logging = true;
    log_large_allocation_history();
    assert(!insert_during_logging);
    return 0;
}
