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

struct AddressSpaceSnapshot {
    unsigned long long free_bytes;
    unsigned long long committed_bytes;
    unsigned long long reserved_bytes;
    SIZE_T largest_free;
    void* largest_free_base;
};

static PVOID volatile g_emergencyReserve = nullptr;
static SIZE_T g_emergencyReserveSize = 0;
static volatile LONG g_releaseReason = RELEASE_NONE;
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

static bool release_reserve(ReleaseReason reason) {
    PVOID reserve = InterlockedExchangePointer(&g_emergencyReserve, nullptr);
    if (!reserve) return false;

    if (!VirtualFree(reserve, 0, MEM_RELEASE)) {
        InterlockedCompareExchangePointer(&g_emergencyReserve, reserve, nullptr);
        return false;
    }
    InterlockedCompareExchange(&g_releaseReason, reason, RELEASE_NONE);
    return true;
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

    const unsigned long reserve_mib = g_emergencyReserve
        ? static_cast<unsigned long>(g_emergencyReserveSize / MiB) : 0ul;
    const LONG count = g_requestCount;
    const LONG64 requested = g_requestedBytes;
    const LONG largest = g_largestRequest;

    logger::log(
        "MEM%s : VA free=%llu MiB largest=%lu MiB @ %p committed=%llu MiB reserved=%llu MiB "
        "guard=%lu MiB | requests=%ld cumulative=%llu MiB max_request=%ld KiB | private=%llu MiB working_set=%llu MiB",
        low ? " WARNING" : "",
        snapshot.free_bytes / MiB,
        static_cast<unsigned long>(snapshot.largest_free / MiB),
        snapshot.largest_free_base,
        snapshot.committed_bytes / MiB,
        snapshot.reserved_bytes / MiB,
        reserve_mib,
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
        if (low) release_reserve(RELEASE_LOW_ADDRESS_SPACE);

        LONG reason = InterlockedExchange(&g_releaseReason, RELEASE_NONE);
        if (reason != RELEASE_NONE) {
            const char* label = reason == RELEASE_LOW_ADDRESS_SPACE ? "low address space" :
                                reason == RELEASE_LARGE_REQUEST ? "large allocation request" :
                                "allocation failure";
            logger::log("MEM GUARD RELEASED : %lu MiB emergency VA reserve (%s)",
                        static_cast<unsigned long>(g_emergencyReserveSize / MiB), label);
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
    SIZE_T wanted = static_cast<SIZE_T>(EMERGENCY_RESERVE_MB) * static_cast<SIZE_T>(MiB);
    void* reserve = reserve_high_end(before, wanted);
    if (!reserve) reserve = VirtualAlloc(nullptr, wanted, MEM_RESERVE, PAGE_NOACCESS);

    if (!reserve) {
        wanted /= 2;
        reserve = reserve_high_end(before, wanted);
        if (!reserve) reserve = VirtualAlloc(nullptr, wanted, MEM_RESERVE, PAGE_NOACCESS);
    }

    g_emergencyReserveSize = reserve ? wanted : 0;
    InterlockedExchangePointer(&g_emergencyReserve, reserve);
    logger::log("MEM GUARD : emergency reserve=%p size=%lu MiB",
                reserve, static_cast<unsigned long>(g_emergencyReserveSize / MiB));
    log_snapshot(scan_address_space(), false);

    HANDLE thread = CreateThread(nullptr, 0, &monitor_thread, nullptr, 0, nullptr);
    if (!thread) {
        logger::log("MEM GUARD : monitor thread creation failed err=%lu", GetLastError());
        return reserve != nullptr;
    }
    CloseHandle(thread);
    return true;
}

void before_allocation(std::size_t size) {
    if (!g_emergencyReserve || size < 8u * 1024u * 1024u) return;
    AddressSpaceSnapshot snapshot = scan_address_space();
    const unsigned long long margin = 16ull * MiB;
    if (snapshot.free_bytes < static_cast<unsigned long long>(size) + margin ||
        snapshot.largest_free < static_cast<SIZE_T>(size) + static_cast<SIZE_T>(margin)) {
        release_reserve(RELEASE_LARGE_REQUEST);
    }
}

bool release_after_failure() {
    return release_reserve(RELEASE_ALLOCATION_FAILURE);
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

void record_failure(std::size_t size, const char* hook_name, bool retry_succeeded) {
    logger::log("ALLOC FAILURE%s : %s requested=%zu bytes",
                retry_succeeded ? " RECOVERED" : "",
                hook_name ? hook_name : "unknown", size);
    logger::flush_now();
}

} // namespace memory_guard
