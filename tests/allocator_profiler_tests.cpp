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
DWORD diagnostic_tick = 100000;
DWORD WINAPI mock_get_last_error() { return last_error; }
void WINAPI mock_set_last_error(DWORD value) { last_error = value; }
DWORD WINAPI mock_tick() { return diagnostic_tick; }
}
#define GetLastError mock_get_last_error
#define SetLastError mock_set_last_error
#define GetTickCount mock_tick
#include "../src/diagnostics/allocator_profiler.cpp"
#undef GetLastError
#undef SetLastError
#undef GetTickCount

namespace {
unsigned release_calls = 0;
unsigned stages_available = 0;
unsigned native_calls = 0;
SIZE_T native_size = 0;
DWORD native_flags = 0;
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
bool saw_full_profile = false;
bool saw_full_request_site = false;
unsigned trace_lines = 0;
unsigned age_lines = 0;
bool measure_new_log_bytes = false;
unsigned new_log_bytes = 0;
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
void log(const char* format, ...) {
    if (measure_new_log_bytes && (std::strstr(format, "ALLOC SHAPE :") ||
        std::strstr(format, "ALLOC LIFECYCLE :") || std::strstr(format, "ALLOCATOR ASSESSMENT :"))) {
        char rendered[2048]{};
        va_list args;
        va_start(args, format);
        const int length = std::vsnprintf(rendered, sizeof(rendered), format, args);
        va_end(args);
        assert(length >= 0 && static_cast<unsigned>(length) < sizeof(rendered));
        new_log_bytes += static_cast<unsigned>(length) + 32u; // timestamp + CRLF allowance
    }
    assert(!std::strstr(format, "mode=light"));
    assert(!std::strstr(format, "ALLOC TOP REQUEST EST"));
    if (std::strstr(format, "mode=full request_site_rate=1/1")) saw_full_profile = true;
    if (std::strstr(format, "ALLOC TOP REQUEST #")) saw_full_request_site = true;
    if (std::strstr(format, "ALLOC WATCH TRACE :")) ++trace_lines;
    if (std::strstr(format, "ALLOC WATCH AGE :")) ++age_lines;
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
void report_coverage(const char*, ...) { last_error = 0xBAD; }
void report_hotspot(const char* format, ...) {
    assert(!std::strstr(format, "req_est"));
    last_error = 0xBAD;
}
void flush_now() { last_error = 0xBAD; }
}

namespace {
void native_call() {
    assert(last_error == incoming_error);
    ++native_calls;
}
LPVOID WINAPI allocate(HANDLE, DWORD flags, SIZE_T size) {
    native_call();
    native_size = size;
    native_flags = flags;
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
    g_callsites_saturated.store(false);
    g_sample_entries = g_sample_drops = 0;
    g_callsite_cache.clear();
    g_age_sequence = 0;
    diagnostic_tick = 100000;
    g_failure_trace_count.store(0);
    for (auto& trace : g_watch_traces) { trace.state.store(0); trace.event = {}; }
    for (auto& logged : g_watch_code_logged) logged.store(false);
    g_sample_live_bytes = 0;
    g_shape_counter = 0;
    for (auto& shape : g_request_shape) { shape.calls = 0; shape.bytes = 0; }
    for (auto& shape : g_retired_shape) { shape.calls = 0; shape.bytes = 0; }
    g_sample_cross_thread = g_sample_same_thread = g_sample_reallocated = g_sample_retired_ms = 0;
    g_sample_request_flags = 0;
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
    for (auto& shape : g_request_shape) { shape.calls = 0x7fffffffffffffffLL; shape.bytes = 0x7fffffffffffffffLL; }
    for (auto& shape : g_retired_shape) { shape.calls = 0x7fffffffffffffffLL; shape.bytes = 0x7fffffffffffffffLL; }
    measure_new_log_bytes = true;
    log_summary(false);
    measure_new_log_bytes = false;
    assert(new_log_bytes <= 4096);
    std::printf("new allocator evidence maximum-counter summary_bytes=%u\n", new_log_bytes);
    reset();
    // Component microbenchmark only: not FPS or a game-wide overhead claim.
    LARGE_INTEGER frequency{}, start{}, finish{};
    assert(QueryPerformanceFrequency(&frequency));
    for (unsigned round = 0; round < 3; ++round) {
        for (unsigned observed = 0; observed < 2; ++observed) {
            assert(QueryPerformanceCounter(&start));
            for (unsigned i = 0; i < 1000000; ++i) {
                record_alloc_site(10, 32, false);
                if (observed) record_request_shape(32);
            }
            assert(QueryPerformanceCounter(&finish));
            std::printf("shape benchmark round=%u enabled=%u calls=1000000 wall_us=%lld\n", round, observed,
                static_cast<long long>((finish.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart));
        }
    }
    reset();
    // Distribution sampling is additional evidence, never a replacement for
    // full accounting. No allocation payload is touched by either observer.
    for (unsigned i = 0; i < diagnostics::REQUEST_SHAPE_RATE * 2; ++i) record_request_shape(32);
    const unsigned shape_index = diagnostics::shape_bucket(32);
    assert(load64(&g_request_shape[shape_index].calls) == 2);
    assert(load64(&g_request_shape[shape_index].bytes) == 64);
    assert(insert_sample(sampled_pointer, 100, 10, nullptr, HEAP_ZERO_MEMORY));
    SampleEntry life = detach_sample(sampled_pointer);
    assert(life.thread_id == GetCurrentThreadId() && life.request_flags == HEAP_ZERO_MEMORY);
    diagnostic_tick += 10;
    retire_sample(life, true, true);
    assert(load64(&g_sample_reallocated) == 1 && load64(&g_sample_same_thread) == 1);
    assert(load64(&g_sample_retired_ms) == 10);
    assert(insert_sample(sampled_pointer, 100, 10, nullptr));
    life = detach_sample(sampled_pointer);
    life.thread_id = GetCurrentThreadId() + 1;
    restore_sample(life);
    retire_sample(detach_sample(sampled_pointer));
    assert(load64(&g_sample_cross_thread) == 1);
    const auto retired_before = load64(&g_retired_shape[diagnostics::shape_bucket(100)].calls);
    assert(insert_sample(sampled_pointer, 100, 10, nullptr));
    retire_sample(detach_sample(sampled_pointer), false);
    assert(load64(&g_retired_shape[diagnostics::shape_bucket(100)].calls) == retired_before);

    reset();
    assert(insert_sample(sampled_pointer, 100, 10, nullptr));
    const SampleEntry detached = detach_sample(sampled_pointer);
    g_callsites_saturated.store(true);
    assert(insert_sample(replacement_pointer, 200, 20, nullptr));
    retire_sample(detached);
    assert(load64(&g_sample_live_bytes) == 200);
    assert(g_sample_entries == 1);
    retire_sample(detach_sample(replacement_pointer));
    assert(load64(&g_sample_live_bytes) == 0 && g_sample_entries == 0);
    // Site registration saturation must not retain bytes freed afterward.
    g_callsites_saturated.store(false);
    assert(load64(&callsite_for(10)->sample_live_bytes) == 0);

    // Saturation keeps existing hot sites current without registering new
    // sites or resuming unbounded full-table searches on each allocation.
    reset();
    CallsiteStat* const tracked = callsite_for(10);
    g_callsites_saturated.store(true);
    record_alloc_site(10, 50, false);
    assert(load64(&tracked->requested_bytes) == 50);
    assert(callsite_for(20) == nullptr);
    record_alloc_site(10, 60, false);
    assert(load64(&tracked->requested_bytes) == 110);

    // Alternating saturated misses stay in separate thread-local cache slots.
    reset();
    for (unsigned i = 0; i < CALLSITE_CAPACITY; ++i)
        g_callsites[i].rva = static_cast<LONG>(0x30000000u + i);
    g_callsites_saturated.store(true);
    const LONG64 drops_before = load64(&g_callsite_drops);
    for (unsigned i = 0; i < 1000; ++i) {
        assert(!callsite_for(10));
        assert(!callsite_for(20));
    }
    assert(load64(&g_callsite_drops) - drops_before == 2);

    // Pre-registering watch sites keeps them within the saturated probe bound
    // without expanding the table or changing full request accounting.
    reset();
    CallsiteStat* watched[diagnostics::WATCH_SITE_COUNT]{};
    for (unsigned i = 0; i < diagnostics::WATCH_SITE_COUNT; ++i)
        watched[i] = callsite_for(diagnostics::WATCH_SITES[i]);
    for (unsigned i = 0; i < CALLSITE_CAPACITY; ++i) callsite_for(0x100000u + i);
    assert(g_callsites_saturated.load());
    for (unsigned i = 0; i < diagnostics::WATCH_SITE_COUNT; ++i)
        assert(watched[i] && callsite_for(diagnostics::WATCH_SITES[i]) == watched[i]);

    // Age metadata is taken from live sample identities, never payloads or
    // freed-block averages. It survives site-registration saturation.
    reset();
    g_engine_context_rva = diagnostics::WATCH_SITES[0];
    g_callsites_saturated.store(true);
    assert(insert_sample(sampled_pointer, 100, diagnostics::WATCH_SITES[0], nullptr));
    auto ages = snapshot_retained_ages();
    assert(ages.baseline && ages.total.bytes == 100 && ages.sites[0].blocks == 1);
    assert(ages.total.oldest_ms == 0 && ages.deltas[0] == 0);
    diagnostic_tick += 60000;
    ages = snapshot_retained_ages();
    assert(!ages.baseline && ages.span == 60000 && ages.sequence == 2);
    assert(ages.sites[0].oldest_ms == 60000 && ages.sites[0].age_bytes[3] == 100);
    retire_sample(detach_sample(sampled_pointer));
    ages = snapshot_retained_ages();
    assert(ages.total.bytes == 0 && ages.deltas[0] == -100);
    const unsigned ages_before = age_lines;
    log_watch_diagnostics();
    assert(age_lines == ages_before + diagnostics::WATCH_SITE_COUNT);

    // Only one successful >=64 KiB trace per watched caller and eight failure
    // traces per process. Publication preserves LastError and logging drains
    // records once, with no lock held across logger calls.
    reset();
    g_engine_context_rva = diagnostics::WATCH_SITES[0];
    g_engine_context_size = 699192;
    last_error = incoming_error;
    record_diagnostic_trace(LargeAllocationKind::heap, 64, 0x93176c, false, 0);
    assert(g_watch_traces[0].state.load() == 0);
    record_diagnostic_trace(LargeAllocationKind::heap, 699211, 0x93176c, false, 0);
    assert(last_error == incoming_error && g_watch_traces[0].state.load() == 2);
    assert(g_watch_traces[0].event.engine_bytes == 699192);
    assert(g_watch_traces[0].event.native_bytes == 699211);
    record_diagnostic_trace(LargeAllocationKind::heap, 999999, 0x93176c, false, 0);
    assert(g_watch_traces[0].event.native_bytes == 699211);
    for (unsigned i = 0; i < 20; ++i)
        record_diagnostic_trace(LargeAllocationKind::heap, 699211, 0x93176c, true, 0);
    assert(g_failure_trace_count.load() == diagnostics::FAILURE_TRACES);
    const unsigned traces_before = trace_lines;
    log_watch_traces();
    assert(trace_lines == traces_before + 1 + diagnostics::FAILURE_TRACES);
    log_watch_traces();
    assert(trace_lines == traces_before + 1 + diagnostics::FAILURE_TRACES);

    reset();
    g_engine_context_active = false;
    record_diagnostic_trace(LargeAllocationKind::heap, 699211, diagnostics::WATCH_SITES[0], false, 0);
    assert(g_watch_traces[0].state.load() == 2);
    assert(g_watch_traces[0].event.engine_rva == 0xffffffffu);
    assert(g_watch_traces[0].event.engine_bytes == 0);

    // Every request contributes its exact count and size to a tracked site;
    // request-site sampling/weighting must not silently return.
    reset();
    for (unsigned i = 0; i < 131072; ++i) {
        record_alloc_site(10, 64, false);
    }
    CallsiteStat* exact = callsite_for(10);
    assert(load64(&exact->alloc_calls) == 131072);
    assert(load64(&exact->requested_bytes) == 131072ll * 64);
    log_summary(true);
    assert(saw_full_profile && saw_full_request_site);
    reset();
    record_alloc_site(10, static_cast<SIZE_T>(0xffffffffu), true);
    record_alloc_site(10, static_cast<SIZE_T>(0xffffffffu), true);
    assert(load64(&callsite_for(10)->realloc_calls) == 2);
    assert(load64(&callsite_for(10)->requested_bytes) ==
           static_cast<LONG64>(0xffffffffull * 2));

    // Global totals and site accounting must not double-count native retries
    // or change native arguments/error-preservation behavior.
    reset();
    stages_available = 1;
    succeed_on_call = 2;
    const LONG64 calls_before = load64(&g_heap_alloc_calls);
    const LONG64 bytes_before = load64(&g_heap_requested_bytes);
    assert(profiled_heap_alloc(nullptr, HEAP_ZERO_MEMORY, 123) == replacement_pointer);
    assert(load64(&g_heap_alloc_calls) == calls_before + 1);
    assert(load64(&g_heap_requested_bytes) == bytes_before + 123);
    assert(load64(&callsite_for(10)->alloc_calls) == 1);
    assert(load64(&callsite_for(10)->requested_bytes) == 123);
    assert(native_calls == 2 && native_size == 123 && native_flags == HEAP_ZERO_MEMORY);
    assert(release_calls == 1 && snapshot_calls == 2);
    assert(last_error == ERROR_ALREADY_EXISTS);

    // Recovered small native failures retain the real engine context, not a
    // size inferred later from wrapper overhead or a periodic summary.
    reset();
    g_engine_context_rva = diagnostics::WATCH_SITES[1];
    g_engine_context_size = 699192;
    stages_available = 1;
    succeed_on_call = 2;
    assert(profiled_heap_alloc(nullptr, 0, 699211) == replacement_pointer);
    LargeAllocationEvent small_failure[1]{};
    assert(g_large_history.snapshot(small_failure, 1) == 1);
    assert(small_failure[0].requested == 699211 && small_failure[0].engine_requested == 699192);
    assert(small_failure[0].engine_rva == diagnostics::WATCH_SITES[1]);
    assert(small_failure[0].initial_failure && small_failure[0].flags == (FLIGHT_FAILURE | FLIGHT_RECOVERED));

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
