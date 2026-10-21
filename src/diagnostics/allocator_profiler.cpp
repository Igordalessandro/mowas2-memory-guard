#include "diagnostics/allocator_profiler.h"

#include "app/config.h"
#include "diagnostics/retry_policy.h"
#include "diagnostics/large_allocation_history.h"
#include "diagnostics/profiling_policy.h"
#include "orchestration/memory_guard.h"
#include "shared/core/module.h"
#include "shared/logging/logger.h"

#include <windows.h>
#include <intrin.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>

namespace allocator_profiler {
namespace {

using HeapAllocFn = LPVOID (WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFn = LPVOID (WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using HeapFreeFn = BOOL (WINAPI*)(HANDLE, DWORD, LPVOID);
using VirtualAllocFn = LPVOID (WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFreeFn = BOOL (WINAPI*)(LPVOID, SIZE_T, DWORD);

constexpr unsigned long long MiB = 1024ull * 1024ull;
constexpr DWORD TARGET_IMAGE_SIZE = 0x00CFC000u;
constexpr unsigned CALLSITE_CAPACITY = 1024;
constexpr unsigned SAMPLE_CAPACITY = 16384;
constexpr unsigned SAMPLE_RATE = 1024;
constexpr unsigned ENGINE_SAMPLE_RATE = 4096;
constexpr unsigned SAMPLE_PROBES = 16;
constexpr unsigned HEAP_CAPACITY = 16;
constexpr unsigned FLIGHT_CAPACITY = 256;
constexpr unsigned PRESSURE_FLIGHT_RATE = 256;
constexpr SIZE_T LARGE_FLIGHT_BYTES = 1024u * 1024u;
constexpr DWORD SUMMARY_INTERVAL_MS = 30000;
constexpr DWORD DETAIL_INTERVAL_MS = 120000;

static HeapAllocFn g_originalHeapAlloc = nullptr;
static HeapReAllocFn g_originalHeapReAlloc = nullptr;
static HeapFreeFn g_originalHeapFree = nullptr;
static VirtualAllocFn g_originalVirtualAlloc = nullptr;
static VirtualFreeFn g_originalVirtualFree = nullptr;
static std::uintptr_t g_base = 0;
static volatile LONG g_installed = 0;
static thread_local unsigned g_engine_sample_counter = 0;
static thread_local unsigned g_pressure_flight_counter = 0;
static thread_local bool g_engine_context_active = false;
static thread_local DWORD g_engine_context_rva = 0xFFFFFFFFu;
static thread_local SIZE_T g_engine_context_size = 0;
static thread_local HANDLE g_engine_context_heap = nullptr;

struct alignas(8) CallsiteStat {
    volatile LONG rva;
    volatile LONG64 alloc_calls;
    volatile LONG64 realloc_calls;
    volatile LONG64 free_calls;
    volatile LONG failures;
    volatile LONG sample_live_count;
    volatile LONG sample_free_count;
    volatile LONG64 engine_sample_calls;
    volatile LONG64 requested_bytes;
    volatile LONG64 sample_live_bytes;
    volatile LONG64 sample_freed_bytes;
    volatile LONG64 sample_lifetime_ms;
    volatile LONG64 engine_sample_bytes;
};
static_assert(alignof(CallsiteStat) >= 8, "64-bit call-site counters must be aligned");
static_assert(offsetof(CallsiteStat, alloc_calls) % 8 == 0,
              "InterlockedIncrement64 requires aligned call counters");

struct SampleEntry {
    void* pointer;
    HANDLE heap;
    DWORD size;
    DWORD alloc_rva;
    DWORD tick;
    CallsiteStat* origin;
};

struct alignas(8) HeapStat {
    void* volatile handle;
    volatile LONG compatibility;
    volatile LONG sample_live_count;
    volatile LONG64 sample_alloc_count;
    volatile LONG64 sample_free_count;
    volatile LONG64 sample_live_bytes;
    volatile LONG64 sample_freed_bytes;
};
static_assert(alignof(HeapStat) >= 8, "64-bit heap counters must be aligned");
static_assert(offsetof(HeapStat, sample_alloc_count) % 8 == 0,
              "InterlockedIncrement64 requires aligned heap counters");

enum FlightFlags : DWORD {
    FLIGHT_LARGE = 1u,
    FLIGHT_PRESSURE = 2u,
    FLIGHT_FAILURE = 4u,
    FLIGHT_RECOVERED = 8u,
};

struct alignas(8) FlightEvent {
    volatile LONG sequence;
    DWORD tick;
    DWORD thread_id;
    DWORD caller_rva;
    DWORD size;
    DWORD flags;
    void* result;
    HANDLE heap;
};

static CallsiteStat g_callsites[CALLSITE_CAPACITY] = {};
static thread_local diagnostics::CallerCache<CallsiteStat*> g_callsite_cache;
static SampleEntry g_samples[SAMPLE_CAPACITY] = {};
static HeapStat g_heaps[HEAP_CAPACITY] = {};
static FlightEvent g_flight[FLIGHT_CAPACITY] = {};
struct LargeHistoryLock {
    volatile LONG held = 0;
    void lock() {
        while (InterlockedCompareExchange(&held, 1, 0) != 0) SwitchToThread();
    }
    void unlock() { InterlockedExchange(&held, 0); }
};
static LargeAllocationHistory<LargeHistoryLock> g_large_history;
static volatile LONG g_sample_lock = 0;
static volatile LONG g_flight_sequence = 0;
static volatile LONG g_sample_entries = 0;
static volatile LONG g_sample_drops = 0;
static std::atomic<bool> g_callsites_saturated{false};
alignas(8) static volatile LONG64 g_callsite_drops = 0;

alignas(8) static volatile LONG64 g_heap_alloc_calls = 0;
alignas(8) static volatile LONG64 g_heap_realloc_calls = 0;
alignas(8) static volatile LONG64 g_heap_free_calls = 0;
static volatile LONG g_heap_failures = 0;
alignas(8) static volatile LONG64 g_virtual_alloc_calls = 0;
alignas(8) static volatile LONG64 g_virtual_free_calls = 0;
static volatile LONG g_virtual_failures = 0;
static volatile LONG g_virtual_nonmemory_failures = 0;
alignas(8) static volatile LONG64 g_heap_requested_bytes = 0;
alignas(8) static volatile LONG64 g_virtual_requested_bytes = 0;
alignas(8) static volatile LONG64 g_sample_live_bytes = 0;

static __forceinline DWORD caller_rva() {
    const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (caller >= g_base && caller < g_base + TARGET_IMAGE_SIZE) {
        return static_cast<DWORD>(caller - g_base);
    }
    return 0xFFFFFFFFu;
}

static DWORD mix_pointer(const void* pointer) {
    DWORD value = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(pointer) >> 4);
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

static bool should_sample(const void* pointer) {
    return pointer && ((mix_pointer(pointer) & (SAMPLE_RATE - 1u)) == 0);
}

static DWORD sampled_upstream_rva(DWORD direct_rva) {
    void* frames[8] = {};
    const USHORT count = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
    bool saw_direct = false;
    for (USHORT i = 0; i < count; ++i) {
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (address < g_base || address >= g_base + TARGET_IMAGE_SIZE) continue;
        const DWORD rva = static_cast<DWORD>(address - g_base);
        if (!saw_direct) {
            if (rva == direct_rva) saw_direct = true;
            continue;
        }
        if (rva != direct_rva) return rva;
    }
    return direct_rva;
}

static void lock_samples() {
    while (InterlockedCompareExchange(&g_sample_lock, 1, 0) != 0) {
        SwitchToThread();
    }
}

static void unlock_samples() {
    InterlockedExchange(&g_sample_lock, 0);
}

static LONG64 load64(volatile LONG64* value) {
    return InterlockedCompareExchange64(value, 0, 0);
}

static DWORD to_game_rva(std::uintptr_t caller) {
    return caller >= g_base && caller < g_base + TARGET_IMAGE_SIZE
        ? static_cast<DWORD>(caller - g_base) : 0xFFFFFFFFu;
}

static void record_large_request(LargeAllocationKind kind, SIZE_T size,
                                 DWORD direct_rva, void* result, HANDLE heap,
                                 DWORD request_flags, DWORD flags = 0) {
    if (size < LARGE_FLIGHT_BYTES) return;
    LargeAllocationEvent event {};
    event.tick = GetTickCount();
    event.thread_id = GetCurrentThreadId();
    event.caller_rva = direct_rva;
    event.engine_rva = g_engine_context_active ? g_engine_context_rva : 0xFFFFFFFFu;
    event.requested = size;
    event.engine_requested = g_engine_context_active ? g_engine_context_size : 0;
    event.result = reinterpret_cast<std::uintptr_t>(result);
    event.heap = reinterpret_cast<std::uintptr_t>(heap);
    event.request_flags = request_flags;
    event.flags = flags;
    event.kind = kind;
    g_large_history.record(event);
}

static HeapStat* heap_stat_for(HANDLE heap) {
    if (!heap) return nullptr;
    unsigned index = (static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(heap)) >> 3u) &
                     (HEAP_CAPACITY - 1u);
    for (unsigned probe = 0; probe < HEAP_CAPACITY; ++probe) {
        HeapStat& slot = g_heaps[(index + probe) & (HEAP_CAPACITY - 1u)];
        void* observed = slot.handle;
        if (observed == heap) return &slot;
        if (!observed) {
            observed = InterlockedCompareExchangePointer(&slot.handle, heap, nullptr);
            if (!observed || observed == heap) {
                if (!observed) {
                    ULONG compatibility = 0;
                    SIZE_T returned = 0;
                    const BOOL queried = HeapQueryInformation(
                        heap, HeapCompatibilityInformation,
                        &compatibility, sizeof(compatibility), &returned);
                    InterlockedExchange(&slot.compatibility,
                                        queried ? static_cast<LONG>(compatibility) : -1);
                }
                return &slot;
            }
        }
    }
    return nullptr;
}

static void record_flight_event(SIZE_T size, DWORD caller_rva_value, void* result,
                                HANDLE heap, DWORD flags) {
    const LONG sequence = InterlockedIncrement(&g_flight_sequence);
    FlightEvent& event = g_flight[
        (static_cast<unsigned>(sequence) - 1u) & (FLIGHT_CAPACITY - 1u)];

    // Zeroing the published sequence prevents readers from accepting a slot
    // while this thread is replacing it.
    InterlockedExchange(&event.sequence, 0);
    event.tick = GetTickCount();
    event.thread_id = GetCurrentThreadId();
    event.caller_rva = caller_rva_value;
    event.size = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<DWORD>(size);
    event.flags = flags;
    event.result = result;
    event.heap = heap;
    MemoryBarrier();
    InterlockedExchange(&event.sequence, sequence);
}

static CallsiteStat* callsite_for(DWORD rva) {
    // Keep frequent sites current even after the fixed table fills. Cache the
    // hot callers per thread; saturated misses search at most 16 slots instead
    // of returning stale retained totals or probing 1024 slots per call.
    CallsiteStat* cached = nullptr;
    if (g_callsite_cache.lookup(rva, cached)) return cached;
    const bool saturated = g_callsites_saturated.load(std::memory_order_relaxed);
    auto remember = [rva](CallsiteStat* site) {
        g_callsite_cache.remember(rva, site);
        return site;
    };

    // Zero means unused, so keep the out-of-image sentinel but shift a true
    // RVA zero to one (the game has no allocation call at its DOS header).
    LONG key = static_cast<LONG>(rva == 0 ? 1u : rva);
    unsigned index = (rva * 2654435761u) & (CALLSITE_CAPACITY - 1u);
    const unsigned probe_limit = saturated ? 16u : CALLSITE_CAPACITY;
    for (unsigned probe = 0; probe < probe_limit; ++probe) {
        CallsiteStat& slot = g_callsites[(index + probe) & (CALLSITE_CAPACITY - 1u)];
        LONG observed = slot.rva;
        if (observed == key) return remember(&slot);
        if (observed == 0) {
            if (saturated) return remember(nullptr);
            observed = InterlockedCompareExchange(&slot.rva, key, 0);
            if (observed == 0 || observed == key) return remember(&slot);
        }
    }
    InterlockedIncrement64(&g_callsite_drops);
    g_callsites_saturated.store(true, std::memory_order_relaxed);
    return remember(nullptr);
}

static void record_alloc_site(DWORD rva, SIZE_T size, bool realloc_call) {
    CallsiteStat* site = callsite_for(rva);
    if (!site) return;
    if (realloc_call) InterlockedIncrement64(&site->realloc_calls);
    else InterlockedIncrement64(&site->alloc_calls);
    InterlockedExchangeAdd64(&site->requested_bytes, static_cast<LONG64>(size));
}

static void record_free_site(DWORD rva) {
    CallsiteStat* site = callsite_for(rva);
    if (site) InterlockedIncrement64(&site->free_calls);
}

static void record_failure_site(DWORD rva) {
    CallsiteStat* site = callsite_for(rva);
    if (site) InterlockedIncrement(&site->failures);
}

static bool insert_sample(void* pointer, SIZE_T size, DWORD alloc_rva, HANDLE heap) {
    if (!should_sample(pointer)) return false;
    const DWORD origin_rva = g_engine_context_active
        ? alloc_rva : sampled_upstream_rva(alloc_rva);
    const DWORD hash = mix_pointer(pointer);
    CallsiteStat* const origin_site = callsite_for(origin_rva);
    // The low bits select the 1/SAMPLE_RATE cohort. Use different hash bits
    // for the table index or every sampled pointer clusters into a few slots.
    const unsigned start = (hash / SAMPLE_RATE) & (SAMPLE_CAPACITY - 1u);
    bool inserted = false;
    lock_samples();
    for (unsigned probe = 0; probe < SAMPLE_PROBES; ++probe) {
        SampleEntry& slot = g_samples[(start + probe) & (SAMPLE_CAPACITY - 1u)];
        if (!slot.pointer) {
            slot.heap = heap;
            slot.size = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<DWORD>(size);
            slot.alloc_rva = origin_rva;
            slot.tick = GetTickCount();
            slot.origin = origin_site;
            slot.pointer = pointer;
            inserted = true;
            break;
        }
    }
    unlock_samples();

    if (!inserted) {
        InterlockedIncrement(&g_sample_drops);
        return false;
    }

    InterlockedIncrement(&g_sample_entries);
    InterlockedExchangeAdd64(&g_sample_live_bytes, static_cast<LONG64>(size));
    CallsiteStat* site = origin_site;
    if (site) {
        InterlockedIncrement(&site->sample_live_count);
        InterlockedExchangeAdd64(&site->sample_live_bytes,
                                 size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<LONG64>(size));
    }
    HeapStat* heap_stat = heap_stat_for(heap);
    if (heap_stat) {
        InterlockedIncrement64(&heap_stat->sample_alloc_count);
        InterlockedIncrement(&heap_stat->sample_live_count);
        InterlockedExchangeAdd64(&heap_stat->sample_live_bytes,
                                 size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<LONG64>(size));
    }
    return true;
}

static SampleEntry detach_sample(void* pointer) {
    if (!should_sample(pointer)) return {};
    const DWORD hash = mix_pointer(pointer);
    const unsigned start = (hash / SAMPLE_RATE) & (SAMPLE_CAPACITY - 1u);
    SampleEntry found = {};

    lock_samples();
    for (unsigned probe = 0; probe < SAMPLE_PROBES; ++probe) {
        SampleEntry& slot = g_samples[(start + probe) & (SAMPLE_CAPACITY - 1u)];
        if (slot.pointer == pointer) {
            found = slot;
            slot.pointer = nullptr;
            slot.heap = nullptr;
            slot.size = 0;
            slot.alloc_rva = 0;
            slot.tick = 0;
            slot.origin = nullptr;
            break;
        }
    }
    unlock_samples();
    return found;
}

static void retire_sample(const SampleEntry& found, bool freed = true) {
    if (!found.pointer) return;
    InterlockedDecrement(&g_sample_entries);
    InterlockedExchangeAdd64(&g_sample_live_bytes, -static_cast<LONG64>(found.size));
    const DWORD lifetime_ms = GetTickCount() - found.tick;

    // Retain the original site pointer so freeing a sampled block still
    // removes its live bytes after call-site registration has saturated.
    CallsiteStat* origin = found.origin;
    if (origin) {
        InterlockedDecrement(&origin->sample_live_count);
        InterlockedExchangeAdd64(&origin->sample_live_bytes,
                                 -static_cast<LONG64>(found.size));
        if (freed) {
            InterlockedIncrement(&origin->sample_free_count);
            InterlockedExchangeAdd64(&origin->sample_freed_bytes, found.size);
            InterlockedExchangeAdd64(&origin->sample_lifetime_ms, lifetime_ms);
        }
    }
    HeapStat* heap_stat = heap_stat_for(found.heap);
    if (heap_stat) {
        InterlockedDecrement(&heap_stat->sample_live_count);
        InterlockedExchangeAdd64(&heap_stat->sample_live_bytes,
                                 -static_cast<LONG64>(found.size));
        if (freed) {
            InterlockedIncrement64(&heap_stat->sample_free_count);
            InterlockedExchangeAdd64(&heap_stat->sample_freed_bytes,
                                     static_cast<LONG64>(found.size));
        }
    }
}

static void restore_sample(const SampleEntry& entry) {
    if (!entry.pointer) return;
    const unsigned start = (mix_pointer(entry.pointer) / SAMPLE_RATE) &
                           (SAMPLE_CAPACITY - 1u);
    bool restored = false;
    lock_samples();
    for (unsigned probe = 0; probe < SAMPLE_PROBES; ++probe) {
        SampleEntry& slot = g_samples[(start + probe) & (SAMPLE_CAPACITY - 1u)];
        if (!slot.pointer) {
            slot = entry;
            restored = true;
            break;
        }
    }
    unlock_samples();
    if (!restored) {
        // A failed native free/realloc leaves the allocation alive. If other
        // threads filled the sample bucket, drop only our diagnostic record.
        retire_sample(entry, false);
        InterlockedIncrement(&g_sample_drops);
    }
}

static void log_low_level_failure(const char* api, SIZE_T size, DWORD rva,
                                  SIZE_T released_bytes, unsigned retry_count,
                                  bool recovered) {
    InterlockedIncrement(&g_heap_failures);
    record_failure_site(rva);
    logger::log(
        "LOW-LEVEL ALLOC FAILURE%s : %s size=%zu caller=mowas_2.exe+0x%08lX released=%llu MiB retries=%u",
        recovered ? " RECOVERED" : "",
        api,
        size,
        static_cast<unsigned long>(rva),
        static_cast<unsigned long long>(released_bytes) / MiB,
        retry_count);
    logger::report_failure(
        "failure=%s bytes=%zu site=+%08lX released=%lluMiB retries=%u result=%s",
        api, size, static_cast<unsigned long>(rva),
        static_cast<unsigned long long>(released_bytes) / MiB, retry_count,
        recovered ? "ok" : retry_count ? "failed" : "not_attempted");
    log_summary(true);
    logger::flush_now();
}

static LPVOID WINAPI profiled_heap_alloc(HANDLE heap, DWORD flags, SIZE_T size) {
    const DWORD incoming_error = GetLastError();
    const DWORD direct_rva = caller_rva();
    const DWORD rva = g_engine_context_active ? g_engine_context_rva : direct_rva;
    if (g_engine_context_active) g_engine_context_heap = heap;
    InterlockedIncrement64(&g_heap_alloc_calls);
    InterlockedExchangeAdd64(&g_heap_requested_bytes, static_cast<LONG64>(size));
    record_alloc_site(rva, size, false);

    SetLastError(incoming_error);
    LPVOID result = g_originalHeapAlloc(heap, flags, size);
    DWORD native_error = GetLastError();
    const bool initial_failure = !result;
    if (!result) {
        memory_guard::capture_failure_snapshot("HeapAlloc", size, direct_rva, "initial");
        SIZE_T released_bytes = 0;
        unsigned retry_count = 0;
        bool recovery_stage_released = false;
        for (;;) {
            const memory_guard::ReleasedReserve released =
                memory_guard::release_after_failure();
            if (released.bytes == 0) break;
            released_bytes += released.bytes;
            recovery_stage_released =
                recovery_stage_released || released.recovery_stage;
            ++retry_count;
            SetLastError(incoming_error);
            result = g_originalHeapAlloc(heap, flags, size);
            native_error = GetLastError();
            if (result) break;
        }
        if (retry_count) {
            memory_guard::capture_failure_snapshot(
                "HeapAlloc", size, direct_rva, result ? "retry_ok" : "retry_failed");
        }
        record_large_request(LargeAllocationKind::heap, size, direct_rva, result,
                             heap, flags, FLIGHT_FAILURE | (result ? FLIGHT_RECOVERED : 0u));
        record_flight_event(size, rva, result, heap,
                            FLIGHT_FAILURE | (result ? FLIGHT_RECOVERED : 0u));
        log_low_level_failure("HeapAlloc", size, rva, released_bytes,
                              retry_count, result != nullptr);
        log_flight_recorder();
        if (result && recovery_stage_released) {
            memory_guard::rearm_recovery_if_needed();
        }
    }
    if (!initial_failure) {
        record_large_request(LargeAllocationKind::heap, size, direct_rva, result, heap, flags);
    }
    if (result) insert_sample(result, size, rva, heap);
    SetLastError(native_error);
    return result;
}

static LPVOID WINAPI profiled_heap_realloc(HANDLE heap, DWORD flags,
                                           LPVOID pointer, SIZE_T size) {
    const DWORD incoming_error = GetLastError();
    const DWORD direct_rva = caller_rva();
    const DWORD rva = g_engine_context_active ? g_engine_context_rva : direct_rva;
    if (g_engine_context_active) g_engine_context_heap = heap;
    InterlockedIncrement64(&g_heap_realloc_calls);
    InterlockedExchangeAdd64(&g_heap_requested_bytes, static_cast<LONG64>(size));
    record_alloc_site(rva, size, true);

    // Remove the old identity before the heap can reuse its address. Keep the
    // record private until the native result tells us to retire or restore it.
    const SampleEntry previous_sample = detach_sample(pointer);
    SetLastError(incoming_error);
    LPVOID result = g_originalHeapReAlloc(heap, flags, pointer, size);
    DWORD native_error = GetLastError();
    const bool initial_failure = !result;
    if (!result) {
        memory_guard::capture_failure_snapshot("HeapReAlloc", size, direct_rva, "initial");
        SIZE_T released_bytes = 0;
        unsigned retry_count = 0;
        bool recovery_stage_released = false;
        for (;;) {
            const memory_guard::ReleasedReserve released =
                memory_guard::release_after_failure();
            if (released.bytes == 0) break;
            released_bytes += released.bytes;
            recovery_stage_released =
                recovery_stage_released || released.recovery_stage;
            ++retry_count;
            SetLastError(incoming_error);
            result = g_originalHeapReAlloc(heap, flags, pointer, size);
            native_error = GetLastError();
            if (result) break;
        }
        if (retry_count) {
            memory_guard::capture_failure_snapshot(
                "HeapReAlloc", size, direct_rva, result ? "retry_ok" : "retry_failed");
        }
        record_large_request(LargeAllocationKind::realloc, size, direct_rva, result,
                             heap, flags, FLIGHT_FAILURE | (result ? FLIGHT_RECOVERED : 0u));
        record_flight_event(size, rva, result, heap,
                            FLIGHT_FAILURE | (result ? FLIGHT_RECOVERED : 0u));
        log_low_level_failure("HeapReAlloc", size, rva, released_bytes,
                              retry_count, result != nullptr);
        log_flight_recorder();
        if (result && recovery_stage_released) {
            memory_guard::rearm_recovery_if_needed();
        }
    }
    if (!initial_failure) {
        record_large_request(LargeAllocationKind::realloc, size, direct_rva, result, heap, flags);
    }
    if (result) {
        retire_sample(previous_sample);
        insert_sample(result, size, rva, heap);
    } else {
        restore_sample(previous_sample);
    }
    SetLastError(native_error);
    return result;
}

static BOOL WINAPI profiled_heap_free(HANDLE heap, DWORD flags, LPVOID pointer) {
    const DWORD incoming_error = GetLastError();
    const DWORD rva = caller_rva();
    InterlockedIncrement64(&g_heap_free_calls);
    const DWORD sampled_free_rva = should_sample(pointer)
        ? sampled_upstream_rva(rva) : 0;
    const SampleEntry previous_sample = detach_sample(pointer);
    SetLastError(incoming_error);
    const BOOL result = g_originalHeapFree(heap, flags, pointer);
    const DWORD native_error = GetLastError();
    if (result) {
        if (sampled_free_rva) record_free_site(sampled_free_rva);
        retire_sample(previous_sample);
    } else {
        restore_sample(previous_sample);
    }
    SetLastError(native_error);
    return result;
}

static LPVOID WINAPI profiled_virtual_alloc(LPVOID address, SIZE_T size,
                                            DWORD allocation_type, DWORD protect) {
    const DWORD incoming_error = GetLastError();
    const DWORD rva = caller_rva();
    InterlockedIncrement64(&g_virtual_alloc_calls);
    InterlockedExchangeAdd64(&g_virtual_requested_bytes, static_cast<LONG64>(size));

    SetLastError(incoming_error);
    LPVOID result = g_originalVirtualAlloc(address, size, allocation_type, protect);
    DWORD native_error = GetLastError();
    const bool initial_failure = !result;
    // A reserve can help only some failures, but all resource-exhaustion
    // errors belong in the crash evidence, including commit-limit failures.
    if (!result && is_resource_exhaustion_error(native_error)) {
        memory_guard::capture_failure_snapshot("VirtualAlloc", size, rva, "initial");
        const DWORD original_error = native_error;
        SIZE_T released_bytes = 0;
        unsigned retry_count = 0;
        bool recovery_stage_released = false;
        while (may_retry_virtual_allocation(
            reinterpret_cast<std::uintptr_t>(address), size,
            allocation_type, native_error)) {
            const memory_guard::ReleasedReserve released =
                memory_guard::release_after_failure();
            if (released.bytes == 0) break;
            released_bytes += released.bytes;
            recovery_stage_released =
                recovery_stage_released || released.recovery_stage;
            ++retry_count;
            SetLastError(incoming_error);
            result = g_originalVirtualAlloc(address, size, allocation_type, protect);
            native_error = GetLastError();
            if (result) break;
        }
        if (retry_count) {
            memory_guard::capture_failure_snapshot(
                "VirtualAlloc", size, rva, result ? "retry_ok" : "retry_failed");
        }
        record_large_request(LargeAllocationKind::virtual_alloc, size, rva, result,
                             nullptr, allocation_type,
                             FLIGHT_FAILURE | (result ? FLIGHT_RECOVERED : 0u));
        InterlockedIncrement(&g_virtual_failures);
        record_failure_site(rva);
        record_flight_event(size, rva, result, nullptr,
                            FLIGHT_FAILURE | (result ? FLIGHT_RECOVERED : 0u));
        logger::log(
            "LOW-LEVEL VIRTUAL FAILURE%s : VirtualAlloc size=%zu type=0x%08lX caller=mowas_2.exe+0x%08lX error=%lu released=%llu MiB retries=%u",
            result ? " RECOVERED" : "",
            size,
            static_cast<unsigned long>(allocation_type),
            static_cast<unsigned long>(rva),
            static_cast<unsigned long>(original_error),
            static_cast<unsigned long long>(released_bytes) / MiB,
            retry_count);
        logger::report_failure(
            "failure=VirtualAlloc bytes=%zu site=+%08lX err=%lu released=%lluMiB retries=%u result=%s",
            size, static_cast<unsigned long>(rva),
            static_cast<unsigned long>(original_error),
            static_cast<unsigned long long>(released_bytes) / MiB, retry_count,
            result ? "ok" : retry_count ? "failed" : "not_attempted");
        log_summary(true);
        log_flight_recorder();
        logger::flush_now();
        if (result && recovery_stage_released) {
            memory_guard::rearm_recovery_if_needed();
        }
    } else if (!result) {
        record_large_request(LargeAllocationKind::virtual_alloc, size, rva, result,
                             nullptr, allocation_type, FLIGHT_FAILURE);
        InterlockedIncrement(&g_virtual_failures);
        const LONG count = InterlockedIncrement(&g_virtual_nonmemory_failures);
        if (count <= 4) {
            logger::log(
                "VIRTUAL FAILURE : no VA recovery applicable size=%zu type=0x%08lX error=%lu (first four only)",
                size, static_cast<unsigned long>(allocation_type),
                static_cast<unsigned long>(native_error));
        }
    }
    if (!initial_failure) {
        record_large_request(LargeAllocationKind::virtual_alloc, size, rva, result,
                             nullptr, allocation_type);
    }
    SetLastError(native_error);
    return result;
}

static BOOL WINAPI profiled_virtual_free(LPVOID address, SIZE_T size, DWORD free_type) {
    InterlockedIncrement64(&g_virtual_free_calls);
    return g_originalVirtualFree(address, size, free_type);
}

struct IatPatch {
    const char* name;
    void* replacement;
    void** original;
    IMAGE_THUNK_DATA32* slot;
    DWORD old_value;
};

static bool patch_slot(IatPatch& patch) {
    DWORD old_protection = 0;
    if (!VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), PAGE_READWRITE,
                        &old_protection)) {
        return false;
    }
    patch.old_value = patch.slot->u1.Function;
    *patch.original = reinterpret_cast<void*>(static_cast<std::uintptr_t>(patch.old_value));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&patch.slot->u1.Function),
                        static_cast<LONG>(reinterpret_cast<std::uintptr_t>(patch.replacement)));
    DWORD ignored = 0;
    const bool restored = VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD),
                                         old_protection, &ignored) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), &patch.slot->u1.Function, sizeof(DWORD));
    return restored;
}

static void restore_slot(IatPatch& patch) {
    if (!patch.slot || !patch.old_value) return;
    DWORD old_protection = 0;
    if (!VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), PAGE_READWRITE,
                        &old_protection)) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&patch.slot->u1.Function),
                        static_cast<LONG>(patch.old_value));
    DWORD ignored = 0;
    VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &patch.slot->u1.Function, sizeof(DWORD));
    // A thread can already be executing the wrapper when the IAT is restored.
    // Its resolved native function must remain callable after rollback.
}

static bool find_iat_slots(std::uintptr_t base, IatPatch* patches, unsigned count) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(
        base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;

    const DWORD image_size = nt->OptionalHeader.SizeOfImage;
    const auto& import_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!import_dir.VirtualAddress || import_dir.VirtualAddress >= image_size) return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + import_dir.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (descriptor->Name >= image_size) return false;
        const char* dll_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dll_name, "KERNEL32.dll") != 0) continue;
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) return false;

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            base + descriptor->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            if (names->u1.AddressOfData >= image_size) return false;
            auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            const char* name = reinterpret_cast<const char*>(imported->Name);
            for (unsigned i = 0; i < count; ++i) {
                if (!patches[i].slot && std::strcmp(name, patches[i].name) == 0) {
                    patches[i].slot = iat;
                }
            }
        }
        return true;
    }
    return false;
}

struct RankedSite {
    DWORD rva;
    LONG64 calls;
    LONG64 frees;
    LONG sample_live_count;
    LONG64 requested;
    LONG64 sample_live;
    LONG64 sample_freed;
    LONG64 sample_lifetime;
    LONG sample_free_count;
    LONG64 engine_sample_calls;
    LONG64 engine_sample_bytes;
};

enum RankMode {
    RANK_REQUESTED,
    RANK_LIVE,
    RANK_FREES,
    RANK_ENGINE,
};

static void insert_top(RankedSite* top, unsigned capacity, const RankedSite& candidate,
                       RankMode mode) {
    const LONG64 score = mode == RANK_LIVE ? candidate.sample_live :
                         mode == RANK_FREES ? candidate.frees :
                         mode == RANK_ENGINE ? candidate.engine_sample_bytes :
                         candidate.requested;
    if (score <= 0) return;
    unsigned position = capacity;
    for (unsigned i = 0; i < capacity; ++i) {
        const LONG64 current = mode == RANK_LIVE ? top[i].sample_live :
                               mode == RANK_FREES ? top[i].frees :
                               mode == RANK_ENGINE ? top[i].engine_sample_bytes :
                               top[i].requested;
        if (score > current) {
            position = i;
            break;
        }
    }
    if (position == capacity) return;
    for (unsigned i = capacity - 1; i > position; --i) top[i] = top[i - 1];
    top[position] = candidate;
}

static DWORD WINAPI summary_thread(LPVOID) {
    DWORD elapsed = 0;
    for (;;) {
        Sleep(SUMMARY_INTERVAL_MS);
        elapsed += SUMMARY_INTERVAL_MS;
        const bool detailed = elapsed >= DETAIL_INTERVAL_MS;
        log_summary(detailed);
        if (detailed) elapsed = 0;
    }
}

} // anonymous namespace

void record_engine_request(std::size_t size, std::uintptr_t caller) {
    if (!g_installed) return;
    if ((++g_engine_sample_counter & (ENGINE_SAMPLE_RATE - 1u)) != 0) return;

    const DWORD rva = to_game_rva(caller);
    CallsiteStat* site = callsite_for(rva);
    if (!site) return;
    InterlockedIncrement64(&site->engine_sample_calls);
    InterlockedExchangeAdd64(&site->engine_sample_bytes, static_cast<LONG64>(size));
}

void begin_engine_request(std::size_t size, std::uintptr_t caller) {
    g_engine_context_active = true;
    g_engine_context_rva = to_game_rva(caller);
    g_engine_context_size = size;
    g_engine_context_heap = nullptr;
    record_engine_request(size, caller);
}

void end_engine_request(void* result, bool initial_failure, bool recovered) {
    if (!g_engine_context_active) return;

    DWORD flags = 0;
    if (g_engine_context_size >= LARGE_FLIGHT_BYTES) flags |= FLIGHT_LARGE;
    if (memory_guard::is_under_pressure() &&
        ((++g_pressure_flight_counter & (PRESSURE_FLIGHT_RATE - 1u)) == 0)) {
        flags |= FLIGHT_PRESSURE;
    }
    if (initial_failure || !result) flags |= FLIGHT_FAILURE;
    if (recovered) flags |= FLIGHT_RECOVERED;
    if (flags != 0) {
        record_flight_event(g_engine_context_size, g_engine_context_rva,
                            result, g_engine_context_heap, flags);
    }
    record_large_request(LargeAllocationKind::engine, g_engine_context_size,
                         g_engine_context_rva, result, g_engine_context_heap, 0, flags);

    g_engine_context_active = false;
    g_engine_context_rva = 0xFFFFFFFFu;
    g_engine_context_size = 0;
    g_engine_context_heap = nullptr;
}

void log_flight_recorder() {
    const LONG newest = InterlockedCompareExchange(&g_flight_sequence, 0, 0);
    if (newest <= 0) return;

    constexpr LONG MAX_LOGGED = 16;
    const LONG first = newest > MAX_LOGGED ? newest - MAX_LOGGED + 1 : 1;
    logger::log("ALLOC FLIGHT : newest_sequence=%ld showing=%ld..%ld",
                static_cast<long>(newest), static_cast<long>(first),
                static_cast<long>(newest));
    for (LONG sequence = first; sequence <= newest; ++sequence) {
        FlightEvent& event = g_flight[
            (static_cast<unsigned>(sequence) - 1u) & (FLIGHT_CAPACITY - 1u)];
        if (InterlockedCompareExchange(&event.sequence, 0, 0) != sequence) continue;
        const DWORD tick = event.tick;
        const DWORD thread_id = event.thread_id;
        const DWORD caller = event.caller_rva;
        const DWORD size = event.size;
        const DWORD flags = event.flags;
        void* result = event.result;
        HANDLE heap = event.heap;
        MemoryBarrier();
        if (InterlockedCompareExchange(&event.sequence, 0, 0) != sequence) continue;
        logger::log(
            "ALLOC FLIGHT #%ld : tick=%lu thread=%lu caller=mowas_2.exe+0x%08lX size=%lu result=%p heap=%p flags=0x%02lX",
            static_cast<long>(sequence), static_cast<unsigned long>(tick),
            static_cast<unsigned long>(thread_id), static_cast<unsigned long>(caller),
            static_cast<unsigned long>(size), result, heap,
            static_cast<unsigned long>(flags));
    }
}

void log_large_allocation_history() {
    LargeAllocationEvent events[16] {};
    const std::size_t count = g_large_history.snapshot(events, 16);
    if (!count) return;
    logger::log("ALLOC LARGE HISTORY : threshold=1048576 capacity=32 showing=%zu completion_order=yes",
                count);
    for (std::size_t i = 0; i < count; ++i) {
        const LargeAllocationEvent& event = events[i];
        const char* kind = event.kind == LargeAllocationKind::engine ? "engine" :
                           event.kind == LargeAllocationKind::heap ? "heap" :
                           event.kind == LargeAllocationKind::realloc ? "realloc" : "virtual";
        logger::log(
            "ALLOC LARGE #%llu : kind=%s tick=%lu thread=%lu direct=+%08lX bytes=%zu engine=+%08lX engine_bytes=%zu result=%p heap=%p request_flags=0x%08lX event_flags=0x%02lX",
            static_cast<unsigned long long>(event.sequence), kind,
            static_cast<unsigned long>(event.tick),
            static_cast<unsigned long>(event.thread_id),
            static_cast<unsigned long>(event.caller_rva), event.requested,
            static_cast<unsigned long>(event.engine_rva), event.engine_requested,
            reinterpret_cast<void*>(event.result), reinterpret_cast<void*>(event.heap),
            static_cast<unsigned long>(event.request_flags),
            static_cast<unsigned long>(event.flags));
    }
}

void log_summary(bool detailed) {
    // Global sample lifetime accounting continues after site-table saturation;
    // summing the partial per-site table used to freeze/misstate this estimate.
    const LONG64 sampled_live = load64(&g_sample_live_bytes);
    const unsigned long long estimated_live = sampled_live > 0
        ? static_cast<unsigned long long>(sampled_live) * SAMPLE_RATE : 0ull;

    logger::log(
        "ALLOC PROFILE : heap alloc=%lld realloc=%lld free=%lld failures=%ld requested=%llu MiB | virtual alloc=%lld free=%lld failures=%ld requested=%llu MiB | samples=%ld live=%llu KiB estimate=%llu MiB drops=%ld sites_dropped=%lld sites_saturated=%ld mode=full request_site_rate=1/1",
        static_cast<long long>(load64(&g_heap_alloc_calls)),
        static_cast<long long>(load64(&g_heap_realloc_calls)),
        static_cast<long long>(load64(&g_heap_free_calls)),
        static_cast<long>(g_heap_failures),
        static_cast<unsigned long long>(load64(&g_heap_requested_bytes)) / MiB,
        static_cast<long long>(load64(&g_virtual_alloc_calls)),
        static_cast<long long>(load64(&g_virtual_free_calls)),
        static_cast<long>(g_virtual_failures),
        static_cast<unsigned long long>(load64(&g_virtual_requested_bytes)) / MiB,
        static_cast<long>(g_sample_entries),
        static_cast<unsigned long long>(sampled_live) / 1024ull,
        estimated_live / MiB,
        static_cast<long>(g_sample_drops),
        static_cast<long long>(load64(&g_callsite_drops)),
        static_cast<long>(g_callsites_saturated.load(std::memory_order_relaxed)));
    logger::report_allocator(
        "alloc calls=%lld realloc=%lld free=%lld fail=%ld requested=%lluMiB sampled_live_est=%lluMiB drops=%ld sites_saturated=%ld mode=full",
        static_cast<long long>(load64(&g_heap_alloc_calls)),
        static_cast<long long>(load64(&g_heap_realloc_calls)),
        static_cast<long long>(load64(&g_heap_free_calls)),
        static_cast<long>(g_heap_failures),
        static_cast<unsigned long long>(load64(&g_heap_requested_bytes)) / MiB,
        estimated_live / MiB,
        static_cast<long>(g_sample_drops),
        static_cast<long>(g_callsites_saturated.load(std::memory_order_relaxed)));

    if (!detailed) return;
    constexpr unsigned TOP_COUNT = 6;
    RankedSite top_requested[TOP_COUNT] = {};
    RankedSite top_live[TOP_COUNT] = {};
    RankedSite top_frees[TOP_COUNT] = {};
    RankedSite top_engine[TOP_COUNT] = {};
    for (unsigned i = 0; i < CALLSITE_CAPACITY; ++i) {
        CallsiteStat& site = g_callsites[i];
        const LONG key = site.rva;
        if (!key) continue;
        RankedSite candidate = {};
        candidate.rva = static_cast<DWORD>(key);
        candidate.calls = load64(&site.alloc_calls) + load64(&site.realloc_calls);
        candidate.frees = load64(&site.free_calls);
        candidate.sample_live_count = site.sample_live_count;
        candidate.requested = load64(&site.requested_bytes);
        candidate.sample_live = load64(&site.sample_live_bytes);
        candidate.sample_freed = load64(&site.sample_freed_bytes);
        candidate.sample_lifetime = load64(&site.sample_lifetime_ms);
        candidate.sample_free_count = site.sample_free_count;
        candidate.engine_sample_calls = load64(&site.engine_sample_calls);
        candidate.engine_sample_bytes = load64(&site.engine_sample_bytes);
        insert_top(top_requested, TOP_COUNT, candidate, RANK_REQUESTED);
        insert_top(top_live, TOP_COUNT, candidate, RANK_LIVE);
        insert_top(top_frees, TOP_COUNT, candidate, RANK_FREES);
        insert_top(top_engine, TOP_COUNT, candidate, RANK_ENGINE);
    }

    if (top_requested[0].requested > 0 || top_live[0].sample_live > 0) {
        logger::report_hotspot(
            "hot req=+%08lX/%lluMiB/%lldcalls retained=+%08lX/%lluMiB/%ldblocks",
            static_cast<unsigned long>(top_requested[0].rva),
            static_cast<unsigned long long>(top_requested[0].requested) / MiB,
            static_cast<long long>(top_requested[0].calls),
            static_cast<unsigned long>(top_live[0].rva),
            static_cast<unsigned long long>(top_live[0].sample_live) * SAMPLE_RATE / MiB,
            static_cast<long>(top_live[0].sample_live_count));
    }

    for (unsigned i = 0; i < TOP_COUNT && top_requested[i].requested > 0; ++i) {
        const RankedSite& site = top_requested[i];
        logger::log(
            "ALLOC TOP REQUEST #%u : mowas_2.exe+0x%08lX calls=%lld requested=%llu MiB sample_live=%llu KiB",
            i + 1,
            static_cast<unsigned long>(site.rva),
            static_cast<long long>(site.calls),
            static_cast<unsigned long long>(site.requested) / MiB,
            static_cast<unsigned long long>(site.sample_live) / 1024ull);
    }
    for (unsigned i = 0; i < TOP_COUNT && top_live[i].sample_live > 0; ++i) {
        const RankedSite& site = top_live[i];
        const unsigned long long average_lifetime = site.sample_free_count > 0
            ? static_cast<unsigned long long>(site.sample_lifetime) /
              static_cast<unsigned long long>(site.sample_free_count)
            : 0ull;
        logger::log(
            "ALLOC TOP RETAINED #%u : mowas_2.exe+0x%08lX sample_live=%llu KiB (%ld blocks, est=%llu MiB) sampled_freed=%llu KiB avg_lifetime=%llu ms",
            i + 1,
            static_cast<unsigned long>(site.rva),
            static_cast<unsigned long long>(site.sample_live) / 1024ull,
            static_cast<long>(site.sample_live_count),
            static_cast<unsigned long long>(site.sample_live) * SAMPLE_RATE / MiB,
            static_cast<unsigned long long>(site.sample_freed) / 1024ull,
            average_lifetime);
    }
    for (unsigned i = 0; i < TOP_COUNT && top_frees[i].frees > 0; ++i) {
        const RankedSite& site = top_frees[i];
        logger::log(
            "ALLOC SAMPLED FREE #%u : mowas_2.exe+0x%08lX samples=%lld estimated_calls=%llu",
            i + 1,
            static_cast<unsigned long>(site.rva),
            static_cast<long long>(site.frees),
            static_cast<unsigned long long>(site.frees) * SAMPLE_RATE);
    }
    for (unsigned i = 0; i < TOP_COUNT && top_engine[i].engine_sample_calls > 0; ++i) {
        const RankedSite& site = top_engine[i];
        logger::log(
            "ENGINE ALLOC SAMPLE #%u : mowas_2.exe+0x%08lX samples=%lld sampled=%llu MiB estimated_calls=%llu estimated_requested=%llu MiB",
            i + 1,
            static_cast<unsigned long>(site.rva),
            static_cast<long long>(site.engine_sample_calls),
            static_cast<unsigned long long>(site.engine_sample_bytes) / MiB,
            static_cast<unsigned long long>(site.engine_sample_calls) * ENGINE_SAMPLE_RATE,
            static_cast<unsigned long long>(site.engine_sample_bytes) * ENGINE_SAMPLE_RATE / MiB);
    }

    for (unsigned i = 0; i < HEAP_CAPACITY; ++i) {
        HeapStat& heap = g_heaps[i];
        void* handle = heap.handle;
        if (!handle) continue;
        logger::log(
            "ALLOC HEAP #%u : handle=%p compatibility=%ld sampled_alloc=%lld sampled_free=%lld live_blocks=%ld live=%llu KiB freed=%llu KiB",
            i + 1, handle, static_cast<long>(heap.compatibility),
            static_cast<long long>(load64(&heap.sample_alloc_count)),
            static_cast<long long>(load64(&heap.sample_free_count)),
            static_cast<long>(heap.sample_live_count),
            static_cast<unsigned long long>(load64(&heap.sample_live_bytes)) / 1024ull,
            static_cast<unsigned long long>(load64(&heap.sample_freed_bytes)) / 1024ull);
    }
    log_flight_recorder();
    log_large_allocation_history();
}

bool install(std::uintptr_t game_base) {
    g_base = game_base;
    IatPatch patches[] = {
        {"HeapAlloc", reinterpret_cast<void*>(&profiled_heap_alloc),
         reinterpret_cast<void**>(&g_originalHeapAlloc), nullptr, 0},
        {"HeapReAlloc", reinterpret_cast<void*>(&profiled_heap_realloc),
         reinterpret_cast<void**>(&g_originalHeapReAlloc), nullptr, 0},
        {"HeapFree", reinterpret_cast<void*>(&profiled_heap_free),
         reinterpret_cast<void**>(&g_originalHeapFree), nullptr, 0},
        {"VirtualAlloc", reinterpret_cast<void*>(&profiled_virtual_alloc),
         reinterpret_cast<void**>(&g_originalVirtualAlloc), nullptr, 0},
        {"VirtualFree", reinterpret_cast<void*>(&profiled_virtual_free),
         reinterpret_cast<void**>(&g_originalVirtualFree), nullptr, 0},
    };
    constexpr unsigned patch_count = sizeof(patches) / sizeof(patches[0]);
    if (!find_iat_slots(game_base, patches, patch_count)) {
        logger::log("ALLOC PROFILER install failed: KERNEL32 import table unavailable");
        return false;
    }
    for (unsigned i = 0; i < patch_count; ++i) {
        if (!patches[i].slot) {
            logger::log("ALLOC PROFILER install failed: %s import not found", patches[i].name);
            return false;
        }
    }

    unsigned installed = 0;
    for (; installed < patch_count; ++installed) {
        if (!patch_slot(patches[installed])) {
            logger::log("ALLOC PROFILER install failed: could not patch %s",
                        patches[installed].name);
            restore_slot(patches[installed]);
            while (installed > 0) restore_slot(patches[--installed]);
            return false;
        }
        logger::log("ALLOC PROFILER hook : %s IAT=%p original=%p",
                    patches[installed].name,
                    &patches[installed].slot->u1.Function,
                    *patches[installed].original);
    }

    HANDLE thread = CreateThread(nullptr, 0, &summary_thread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else logger::log("ALLOC PROFILER : summary thread creation failed error=%lu", GetLastError());

    InterlockedExchange(&g_installed, 1);
    logger::log(
        "ALLOC PROFILER installed: mode=full request-site=1/1, 1/%u lifetime sampling, 1/%u engine sampling, original allocators and failure recovery retained",
        SAMPLE_RATE, ENGINE_SAMPLE_RATE);
    return true;
}

namespace {
bool install_module(std::uintptr_t game_base) {
    return allocator_profiler::install(game_base);
}
}

MOWAS_REGISTER_MODULE(
    allocator_profiler_module,
    (::core::ModuleSpec{"allocator_profiler", 100, &install_module}));

} // namespace allocator_profiler
