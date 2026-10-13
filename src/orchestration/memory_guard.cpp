#include "orchestration/memory_guard.h"

#include "app/config.h"
#include "shared/logging/logger.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>

namespace memory_guard {
namespace {

constexpr unsigned long long MiB = 1024ull * 1024ull;

enum ReleaseReason : LONG {
    RELEASE_NONE = 0,
    RELEASE_LOW_ADDRESS_SPACE = 1,
    RELEASE_LARGE_REQUEST = 2,
    RELEASE_ALLOCATION_FAILURE = 3,
};

enum ReleaseEvent : LONG {
    EVENT_NONE = 0,
    EVENT_PRIMARY = 1,
    EVENT_FINAL = 2,
};

struct AddressSpaceSnapshot {
    unsigned long long free_bytes;
    unsigned long long committed_bytes;
    unsigned long long reserved_bytes;
    SIZE_T largest_free;
    void* largest_free_base;
};

static PVOID volatile g_primaryReserve = nullptr;
static PVOID volatile g_finalReserve = nullptr;
static SIZE_T g_primaryReserveSize = 0;
static SIZE_T g_finalReserveSize = 0;
static volatile LONG g_primaryReleaseReason = RELEASE_NONE;
static volatile LONG g_finalReleaseReason = RELEASE_NONE;
static volatile LONG g_releaseEvents = EVENT_NONE;
static volatile LONG g_requestCount = 0;
__declspec(align(8)) static volatile LONG64 g_requestedBytes = 0;
static volatile LONG g_largestRequest = 0;

static AddressSpaceSnapshot scan_address_space() {
    AddressSpaceSnapshot result = {};
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);

    std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const std::uintptr_t limit = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    while (cursor < limit) {
        MEMORY_BASIC_INFORMATION mbi = {};
        SIZE_T queried = VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi));
        if (queried == 0 || mbi.RegionSize == 0) break;

        const unsigned long long region = static_cast<unsigned long long>(mbi.RegionSize);
        if (mbi.State == MEM_FREE) {
            result.free_bytes += region;
            if (mbi.RegionSize > result.largest_free) {
                result.largest_free = mbi.RegionSize;
                result.largest_free_base = mbi.BaseAddress;
            }
        } else if (mbi.State == MEM_COMMIT) {
            result.committed_bytes += region;
        } else if (mbi.State == MEM_RESERVE) {
            result.reserved_bytes += region;
        }

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t next = base + static_cast<std::uintptr_t>(mbi.RegionSize);
        if (next <= cursor) break;
        cursor = next;
    }
    return result;
}

static bool release_reserve(PVOID volatile* reserve_slot,
                            volatile LONG* reason_slot, LONG event,
                            ReleaseReason reason) {
    PVOID reserve = InterlockedExchangePointer(reserve_slot, nullptr);
    if (!reserve) return false;

    if (!VirtualFree(reserve, 0, MEM_RELEASE)) {
        InterlockedCompareExchangePointer(reserve_slot, reserve, nullptr);
        return false;
    }
    InterlockedExchange(reason_slot, reason);
    InterlockedOr(&g_releaseEvents, event);
    return true;
}

static bool release_primary(ReleaseReason reason) {
    return release_reserve(&g_primaryReserve,
                           &g_primaryReleaseReason, EVENT_PRIMARY, reason);
}

static bool release_final(ReleaseReason reason) {
    return release_reserve(&g_finalReserve,
                           &g_finalReleaseReason, EVENT_FINAL, reason);
}

static void* reserve_high_end(const AddressSpaceSnapshot& snapshot, SIZE_T bytes) {
    if (!snapshot.largest_free_base || snapshot.largest_free < bytes) return nullptr;

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const std::uintptr_t granularity = static_cast<std::uintptr_t>(si.dwAllocationGranularity);
    const std::uintptr_t region_base = reinterpret_cast<std::uintptr_t>(snapshot.largest_free_base);
    const std::uintptr_t region_end = region_base + snapshot.largest_free;
    std::uintptr_t desired = region_end - bytes;
    desired -= desired % granularity;
    if (desired < region_base) return nullptr;

    return VirtualAlloc(reinterpret_cast<LPVOID>(desired), bytes, MEM_RESERVE, PAGE_NOACCESS);
}

static void log_snapshot(const AddressSpaceSnapshot& snapshot, bool low) {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    const BOOL have_process = GetProcessMemoryInfo(
        GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

    const unsigned long primary_mib = g_primaryReserve
        ? static_cast<unsigned long>(g_primaryReserveSize / MiB) : 0ul;
    const unsigned long final_mib = g_finalReserve
        ? static_cast<unsigned long>(g_finalReserveSize / MiB) : 0ul;
    const unsigned long reserve_mib = primary_mib + final_mib;
    const LONG count = g_requestCount;
    const LONG64 requested = g_requestedBytes;
    const LONG largest = g_largestRequest;

    logger::log(
        "MEM%s : VA free=%llu MiB largest=%lu MiB @ %p committed=%llu MiB reserved=%llu MiB "
        "guard=%lu MiB (primary=%lu final=%lu) | requests=%ld cumulative=%llu MiB max_request=%ld KiB | private=%llu MiB working_set=%llu MiB",
        low ? " WARNING" : "",
        snapshot.free_bytes / MiB,
        static_cast<unsigned long>(snapshot.largest_free / MiB),
        snapshot.largest_free_base,
        snapshot.committed_bytes / MiB,
        snapshot.reserved_bytes / MiB,
        reserve_mib,
        primary_mib,
        final_mib,
        count,
        static_cast<unsigned long long>(requested) / MiB,
        largest / 1024,
        have_process ? static_cast<unsigned long long>(pmc.PrivateUsage) / MiB : 0ull,
        have_process ? static_cast<unsigned long long>(pmc.WorkingSetSize) / MiB : 0ull);
}

static DWORD WINAPI monitor_thread(LPVOID) {
    DWORD last_log = 0;
    for (;;) {
        Sleep(MONITOR_INTERVAL_MS);
        AddressSpaceSnapshot snapshot = scan_address_space();

        const bool low = snapshot.free_bytes < static_cast<unsigned long long>(RELEASE_TOTAL_FREE_MB) * MiB ||
                         snapshot.largest_free < static_cast<SIZE_T>(RELEASE_LARGEST_FREE_MB) * MiB;
        if (low) release_primary(RELEASE_LOW_ADDRESS_SPACE);

        LONG events = InterlockedExchange(&g_releaseEvents, EVENT_NONE);
        if (events != EVENT_NONE) {
            if (events & EVENT_PRIMARY) {
                const LONG reason = InterlockedExchange(&g_primaryReleaseReason, RELEASE_NONE);
                const char* label = reason == RELEASE_LOW_ADDRESS_SPACE ? "low address space" :
                                    reason == RELEASE_LARGE_REQUEST ? "large allocation request" :
                                    "allocation failure";
                logger::log("MEM GUARD RELEASED : primary %lu MiB VA reserve (%s)",
                            static_cast<unsigned long>(g_primaryReserveSize / MiB), label);
            }
            if (events & EVENT_FINAL) {
                const LONG reason = InterlockedExchange(&g_finalReleaseReason, RELEASE_NONE);
                const char* label = reason == RELEASE_ALLOCATION_FAILURE
                    ? "allocation failure" : "explicit recovery request";
                logger::log("MEM GUARD RELEASED : final %lu MiB VA reserve (%s)",
                            static_cast<unsigned long>(g_finalReserveSize / MiB), label);
            }
            snapshot = scan_address_space();
            log_snapshot(snapshot, true);
            last_log = GetTickCount();
        }

        const DWORD now = GetTickCount();
        if (low || now - last_log >= TELEMETRY_INTERVAL_MS) {
            log_snapshot(snapshot, low);
            last_log = now;
        }
    }
}

} // anonymous namespace

bool install() {
    AddressSpaceSnapshot before = scan_address_space();
    SIZE_T primary_wanted = static_cast<SIZE_T>(PRIMARY_RESERVE_MB) * static_cast<SIZE_T>(MiB);
    void* primary = reserve_high_end(before, primary_wanted);
    if (!primary) primary = VirtualAlloc(nullptr, primary_wanted, MEM_RESERVE, PAGE_NOACCESS);
    if (!primary) {
        primary_wanted /= 2;
        primary = VirtualAlloc(nullptr, primary_wanted, MEM_RESERVE, PAGE_NOACCESS);
    }

    AddressSpaceSnapshot after_primary = scan_address_space();
    SIZE_T final_wanted = static_cast<SIZE_T>(FINAL_RESERVE_MB) * static_cast<SIZE_T>(MiB);
    void* final = reserve_high_end(after_primary, final_wanted);
    if (!final) final = VirtualAlloc(nullptr, final_wanted, MEM_RESERVE, PAGE_NOACCESS);
    if (!final) {
        final_wanted /= 2;
        final = VirtualAlloc(nullptr, final_wanted, MEM_RESERVE, PAGE_NOACCESS);
    }

    g_primaryReserveSize = primary ? primary_wanted : 0;
    g_finalReserveSize = final ? final_wanted : 0;
    InterlockedExchangePointer(&g_primaryReserve, primary);
    InterlockedExchangePointer(&g_finalReserve, final);
    logger::log("MEM GUARD : staged reserves primary=%p size=%lu MiB final=%p size=%lu MiB",
                primary, static_cast<unsigned long>(g_primaryReserveSize / MiB),
                final, static_cast<unsigned long>(g_finalReserveSize / MiB));
    log_snapshot(scan_address_space(), false);

    HANDLE thread = CreateThread(nullptr, 0, &monitor_thread, nullptr, 0, nullptr);
    if (!thread) {
        logger::log("MEM GUARD : monitor thread creation failed err=%lu", GetLastError());
        return primary != nullptr || final != nullptr;
    }
    CloseHandle(thread);
    return true;
}

void before_allocation(std::size_t size) {
    if (!g_primaryReserve || size < 8u * 1024u * 1024u) return;
    AddressSpaceSnapshot snapshot = scan_address_space();
    const unsigned long long margin = 16ull * MiB;
    if (snapshot.free_bytes < static_cast<unsigned long long>(size) + margin ||
        snapshot.largest_free < static_cast<SIZE_T>(size) + static_cast<SIZE_T>(margin)) {
        release_primary(RELEASE_LARGE_REQUEST);
    }
}

bool release_after_failure() {
    const bool primary = release_primary(RELEASE_ALLOCATION_FAILURE);
    const bool final = release_final(RELEASE_ALLOCATION_FAILURE);
    return primary || final;
}

void record_request(std::size_t size) {
    InterlockedIncrement(&g_requestCount);
    InterlockedExchangeAdd64(&g_requestedBytes, static_cast<LONG64>(size));
    LONG observed = g_largestRequest;
    const LONG candidate = size > static_cast<std::size_t>(LONG_MAX)
        ? LONG_MAX : static_cast<LONG>(size);
    while (candidate > observed) {
        const LONG previous = InterlockedCompareExchange(&g_largestRequest, candidate, observed);
        if (previous == observed) break;
        observed = previous;
    }
}

void record_failure(std::size_t size, const char* hook_name,
                    bool reserve_released, bool retry_succeeded) {
    logger::log("ALLOC FAILURE%s : %s requested=%zu bytes reserve_released=%s",
                retry_succeeded ? " RECOVERED" : "",
                hook_name ? hook_name : "unknown", size,
                reserve_released ? "yes" : "no");
    logger::flush_now();
}

} // namespace memory_guard
