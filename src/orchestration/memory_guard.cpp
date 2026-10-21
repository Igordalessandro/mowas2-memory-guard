#include "orchestration/memory_guard.h"
#include "orchestration/recovery_state.h"
#include "diagnostics/address_space_stats.h"

#include "app/config.h"
#include "shared/logging/logger.h"

#include <windows.h>
#include <psapi.h>
#include <cstdio>
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
    EVENT_CRITICAL = 2,
    EVENT_FINAL = 4,
    EVENT_RECOVERY = 8,
    EVENT_PRIMARY_REARMED = 16,
    EVENT_RECOVERY_REARMED = 32,
};

struct AddressSpaceSnapshot {
    unsigned long long free_bytes;
    unsigned long long committed_bytes;
    unsigned long long reserved_bytes;
    SIZE_T largest_free;
    void* largest_free_base;
    diagnostics::AllocatableHoles holes;
    bool complete;
};

static PVOID volatile g_primaryReserve = nullptr;
static PVOID volatile g_criticalReserve = nullptr;
static PVOID volatile g_finalReserve = nullptr;
static SIZE_T g_primaryReserveSize = 0;
static SIZE_T g_criticalReserveSize = 0;
static SIZE_T g_finalReserveSize = 0;
static volatile LONG g_recoveryReleasedMiB = 0;
static volatile LONG g_recoveryRearmedMiB = 0;
static volatile LONG g_primaryReleaseReason = RELEASE_NONE;
static volatile LONG g_criticalReleaseReason = RELEASE_NONE;
static volatile LONG g_finalReleaseReason = RELEASE_NONE;
static volatile LONG g_recoveryReleaseReason = RELEASE_NONE;
static volatile LONG g_releaseEvents = EVENT_NONE;
// Recovery reserve ownership and its retry/re-arm metadata form one state.
// Serialize their transitions so a thread releasing a newly published reserve
// cannot have its pending retry overwritten by the thread that re-armed it.
// Only rare failure/re-arm paths acquire this lock; ordinary allocations do not.
struct RecoveryMutex {
    SRWLOCK lock_value = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&lock_value); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&lock_value) != FALSE; }
    void unlock() { ReleaseSRWLockExclusive(&lock_value); }
};
using GuardRecoveryState = RecoveryState<RecoveryMutex>;
static GuardRecoveryState g_recovery;
__declspec(align(8)) static volatile LONG64 g_requestCount = 0;
__declspec(align(8)) static volatile LONG64 g_requestedBytes = 0;
static volatile LONG g_largestRequest = 0;
static volatile LONG g_underPressure = 0;
static volatile LONG g_finalProtectionLogged = 0;

struct PreserveLastError {
    DWORD error = GetLastError();
    ~PreserveLastError() { SetLastError(error); }
};

static AddressSpaceSnapshot scan_address_space() {
    AddressSpaceSnapshot result = {};
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);

    // Use a wide cursor so the last x86 region can end at 0x100000000
    // without wrapping to zero and incorrectly marking the scan incomplete.
    std::uint64_t cursor = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const std::uint64_t limit = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    while (cursor <= limit) {
        MEMORY_BASIC_INFORMATION mbi = {};
        SIZE_T queried = VirtualQuery(reinterpret_cast<LPCVOID>(
            static_cast<std::uintptr_t>(cursor)), &mbi, sizeof(mbi));
        if (queried == 0 || mbi.RegionSize == 0) break;

        const std::uint64_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uint64_t next = base + static_cast<std::uint64_t>(mbi.RegionSize);
        if (base > cursor || next <= cursor) break;
        // Preserve the original policy totals and largest-region measurement;
        // only the diagnostic fit counts account for allocation granularity.
        const unsigned long long region = static_cast<unsigned long long>(mbi.RegionSize);
        if (mbi.State == MEM_FREE) {
            result.free_bytes += region;
            result.holes.add(base, region, si.dwAllocationGranularity);
            if (region > result.largest_free) {
                result.largest_free = static_cast<SIZE_T>(region);
                result.largest_free_base = mbi.BaseAddress;
            }
        } else if (mbi.State == MEM_COMMIT) {
            result.committed_bytes += region;
        } else if (mbi.State == MEM_RESERVE) {
            result.reserved_bytes += region;
        }

        if (next > limit) {
            result.complete = true;
            break;
        }
        cursor = next;
    }
    return result;
}

static unsigned long guard_mib() {
    const auto recovery = g_recovery.snapshot();
    return static_cast<unsigned long>(
        ((g_primaryReserve ? g_primaryReserveSize : 0) +
         (g_criticalReserve ? g_criticalReserveSize : 0) +
         (g_finalReserve ? g_finalReserveSize : 0) + recovery.reserve.bytes) / MiB);
}

static void log_preflight_release(const char* stage, std::size_t size,
                                  std::uintptr_t caller_rva,
                                  const AddressSpaceSnapshot& before,
                                  const AddressSpaceSnapshot& after) {
    // Only actual releases emit a line. A re-armed primary stage must also
    // retain its next exact trigger; ordinary requests never log here.
    logger::log(
        "MEM GUARD PREFLIGHT : stage=%s bytes=%zu site=+%08lX thread=%lu tick=%lu "
        "before_free=%lluKiB before_largest=%lluKiB after_free=%lluKiB after_largest=%lluKiB complete=%u/%u",
        stage, size, static_cast<unsigned long>(caller_rva),
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long>(GetTickCount()), before.free_bytes / 1024ull,
        static_cast<unsigned long long>(before.largest_free) / 1024ull,
        after.free_bytes / 1024ull,
        static_cast<unsigned long long>(after.largest_free) / 1024ull,
        before.complete ? 1u : 0u, after.complete ? 1u : 0u);
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

static bool release_critical(ReleaseReason reason) {
    return release_reserve(&g_criticalReserve,
                           &g_criticalReleaseReason, EVENT_CRITICAL, reason);
}

static bool release_final(ReleaseReason reason) {
    return release_reserve(&g_finalReserve,
                           &g_finalReleaseReason, EVENT_FINAL, reason);
}

static SIZE_T release_recovery(ReleaseReason reason) {
    return g_recovery.release(GetCurrentThreadId(),
        [reason](const GuardRecoveryState::Reservation& reserve) {
            if (!VirtualFree(reserve.address, 0, MEM_RELEASE)) return false;
            InterlockedExchange(&g_recoveryReleasedMiB,
                                static_cast<LONG>(reserve.bytes / MiB));
            InterlockedExchange(&g_recoveryReleaseReason, reason);
            InterlockedOr(&g_releaseEvents, EVENT_RECOVERY);
            return true;
        });
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

static bool try_rearm_recovery(const AddressSpaceSnapshot& snapshot) {
    // A competing release must publish its owner and pending state before we
    // inspect them. Skip a busy re-arm attempt instead of adding polling to a
    // successful allocation's return path; the monitor will try again later.
    return g_recovery.try_rearm([&snapshot]() -> GuardRecoveryState::Reservation {
        const SIZE_T slack = static_cast<SIZE_T>(RECOVERY_REARM_SLACK_MB) *
                             static_cast<SIZE_T>(MiB);
        SIZE_T candidate = static_cast<SIZE_T>(RECOVERY_RESERVE_MB) *
                           static_cast<SIZE_T>(MiB);
        const SIZE_T minimum = static_cast<SIZE_T>(RECOVERY_REARM_MIN_MB) *
                               static_cast<SIZE_T>(MiB);
        while (candidate >= minimum &&
               (snapshot.largest_free < candidate + slack ||
                snapshot.free_bytes < static_cast<unsigned long long>(candidate) + slack)) {
            candidate -= static_cast<SIZE_T>(8u) * static_cast<SIZE_T>(MiB);
        }
        if (candidate >= minimum) {
            void* replacement = reserve_high_end(snapshot, candidate);
            if (replacement) {
                InterlockedExchange(&g_recoveryRearmedMiB,
                                    static_cast<LONG>(candidate / MiB));
                InterlockedOr(&g_releaseEvents, EVENT_RECOVERY_REARMED);
                return {replacement, candidate};
            }
        }
        return {nullptr, 0};
    });
}

static void log_snapshot(const AddressSpaceSnapshot& snapshot, bool low) {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    const BOOL have_process = GetProcessMemoryInfo(
        GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

    const unsigned long primary_mib = g_primaryReserve
        ? static_cast<unsigned long>(g_primaryReserveSize / MiB) : 0ul;
    const unsigned long critical_mib = g_criticalReserve
        ? static_cast<unsigned long>(g_criticalReserveSize / MiB) : 0ul;
    const unsigned long final_mib = g_finalReserve
        ? static_cast<unsigned long>(g_finalReserveSize / MiB) : 0ul;
    const auto recovery = g_recovery.snapshot();
    const unsigned long recovery_mib =
        static_cast<unsigned long>(recovery.reserve.bytes / MiB);
    const unsigned long reserve_mib =
        primary_mib + critical_mib + final_mib + recovery_mib;
    const LONG64 count = InterlockedCompareExchange64(&g_requestCount, 0, 0);
    const LONG64 requested = InterlockedCompareExchange64(&g_requestedBytes, 0, 0);
    const LONG largest = g_largestRequest;

    logger::log(
        "MEM%s : VA free=%llu MiB largest=%lu MiB @ %p committed=%llu MiB reserved=%llu MiB "
        "guard=%lu MiB (primary=%lu critical=%lu final=%lu recovery=%lu) | requests=%lld cumulative=%llu MiB max_request=%ld KiB | private=%llu MiB working_set=%llu MiB",
        low ? " WARNING" : "",
        snapshot.free_bytes / MiB,
        static_cast<unsigned long>(snapshot.largest_free / MiB),
        snapshot.largest_free_base,
        snapshot.committed_bytes / MiB,
        snapshot.reserved_bytes / MiB,
        reserve_mib,
        primary_mib,
        critical_mib,
        final_mib,
        recovery_mib,
        static_cast<long long>(count),
        static_cast<unsigned long long>(requested) / MiB,
        largest / 1024,
        have_process ? static_cast<unsigned long long>(pmc.PrivateUsage) / MiB : 0ull,
        have_process ? static_cast<unsigned long long>(pmc.WorkingSetSize) / MiB : 0ull);
    logger::log("MEM HOLES : fit4/8/16/32MiB=%lu/%lu/%lu/%lu complete=%u",
                static_cast<unsigned long>(snapshot.holes.counts[0]),
                static_cast<unsigned long>(snapshot.holes.counts[1]),
                static_cast<unsigned long>(snapshot.holes.counts[2]),
                static_cast<unsigned long>(snapshot.holes.counts[3]),
                snapshot.complete ? 1u : 0u);
    char largest_field[40] = {};
    if (snapshot.largest_free < MiB) {
        snprintf(largest_field, sizeof(largest_field), "largest=%lluKiB",
                 static_cast<unsigned long long>(snapshot.largest_free) / 1024ull);
    } else {
        snprintf(largest_field, sizeof(largest_field), "largest=%lluMiB",
                 static_cast<unsigned long long>(snapshot.largest_free) / MiB);
    }
    logger::report_memory(
        "mem free=%lluMiB %s commit=%lluMiB reserve=%lluMiB guard=%luMiB private=%lluMiB ws=%lluMiB req=%lld total=%lluMiB max=%ldKiB",
        snapshot.free_bytes / MiB,
        largest_field,
        snapshot.committed_bytes / MiB,
        snapshot.reserved_bytes / MiB,
        reserve_mib,
        have_process ? static_cast<unsigned long long>(pmc.PrivateUsage) / MiB : 0ull,
        have_process ? static_cast<unsigned long long>(pmc.WorkingSetSize) / MiB : 0ull,
        static_cast<long long>(count),
        static_cast<unsigned long long>(requested) / MiB,
        largest / 1024);
}

static DWORD WINAPI monitor_thread(LPVOID) {
    DWORD last_log = 0;
    DWORD recovery_since = 0;
    DWORD interval = MONITOR_INTERVAL_MS;
    for (;;) {
        Sleep(interval);
        AddressSpaceSnapshot snapshot = scan_address_space();

        const bool low = snapshot.free_bytes < static_cast<unsigned long long>(RELEASE_TOTAL_FREE_MB) * MiB ||
                         snapshot.largest_free < static_cast<SIZE_T>(RELEASE_LARGEST_FREE_MB) * MiB;
        const bool critical = snapshot.free_bytes < static_cast<unsigned long long>(CRITICAL_TOTAL_FREE_MB) * MiB ||
                              snapshot.largest_free < static_cast<SIZE_T>(CRITICAL_LARGEST_FREE_MB) * MiB;
        const bool emergency =
            snapshot.free_bytes < static_cast<unsigned long long>(FINAL_TOTAL_FREE_MB) * MiB ||
            snapshot.largest_free < static_cast<SIZE_T>(FINAL_LARGEST_FREE_MB) * MiB;
        const bool diagnostic_pressure =
            snapshot.free_bytes < 256ull * MiB || snapshot.largest_free < 64u * MiB;
        InterlockedExchange(&g_underPressure, diagnostic_pressure ? 1 : 0);
        interval = diagnostic_pressure ? CRITICAL_MONITOR_INTERVAL_MS : MONITOR_INTERVAL_MS;

        if (low) release_primary(RELEASE_LOW_ADDRESS_SPACE);
        if (critical) release_critical(RELEASE_LOW_ADDRESS_SPACE);
        // Do not release the final contiguous block merely because general
        // address-space pressure is high. The RobZ failure report showed that
        // released space remained available for over a minute, then was no
        // longer protected when demand arrived. New diagnostics record the
        // exact release trigger; size-aware preflight and failure/retry paths
        // remain authorized to release it.
        if (emergency && g_finalReserve &&
            InterlockedCompareExchange(&g_finalProtectionLogged, 1, 0) == 0) {
            logger::log(
                "MEM GUARD PROTECTED : final %lu MiB reserve held for a large request or allocation retry",
                static_cast<unsigned long>(g_finalReserveSize / MiB));
        }

        const auto recovery = g_recovery.snapshot();
        if (recovery.rearm_allowed &&
            try_rearm_recovery(snapshot)) {
            snapshot = scan_address_space();
        }

        const DWORD now = GetTickCount();
        const bool recovered = !g_primaryReserve && g_criticalReserve && g_finalReserve &&
            snapshot.free_bytes >= static_cast<unsigned long long>(REARM_TOTAL_FREE_MB) * MiB &&
            snapshot.largest_free >= static_cast<SIZE_T>(REARM_LARGEST_FREE_MB) * MiB;
        if (recovered) {
            if (recovery_since == 0) recovery_since = now;
            if (now - recovery_since >= REARM_STABLE_MS) {
                const SIZE_T bytes = static_cast<SIZE_T>(PRIMARY_RESERVE_MB) * static_cast<SIZE_T>(MiB);
                void* replacement = reserve_high_end(snapshot, bytes);
                if (replacement) {
                    void* observed = InterlockedCompareExchangePointer(
                        &g_primaryReserve, replacement, nullptr);
                    if (!observed) {
                        g_primaryReserveSize = bytes;
                        InterlockedOr(&g_releaseEvents, EVENT_PRIMARY_REARMED);
                    } else {
                        VirtualFree(replacement, 0, MEM_RELEASE);
                    }
                }
                recovery_since = 0;
            }
        } else {
            recovery_since = 0;
        }

        LONG events = InterlockedExchange(&g_releaseEvents, EVENT_NONE);
        if (events != EVENT_NONE) {
            if (events & EVENT_PRIMARY) {
                const LONG reason = InterlockedExchange(&g_primaryReleaseReason, RELEASE_NONE);
                const char* label = reason == RELEASE_LOW_ADDRESS_SPACE ? "low address space" :
                                    reason == RELEASE_LARGE_REQUEST ? "large allocation request" :
                                    "allocation failure";
                logger::log("MEM GUARD RELEASED : primary %lu MiB VA reserve (%s)",
                            static_cast<unsigned long>(g_primaryReserveSize / MiB), label);
                logger::report_event("event=reserve_release stage=primary size=%luMiB reason=%s",
                                     static_cast<unsigned long>(g_primaryReserveSize / MiB), label);
            }
            if (events & EVENT_CRITICAL) {
                const LONG reason = InterlockedExchange(&g_criticalReleaseReason, RELEASE_NONE);
                const char* label = reason == RELEASE_LOW_ADDRESS_SPACE ? "critical address space" :
                                    reason == RELEASE_LARGE_REQUEST ? "large allocation request" :
                                    "allocation failure";
                logger::log("MEM GUARD RELEASED : critical %lu MiB VA reserve (%s)",
                            static_cast<unsigned long>(g_criticalReserveSize / MiB), label);
                logger::report_event("event=reserve_release stage=critical size=%luMiB reason=%s",
                                     static_cast<unsigned long>(g_criticalReserveSize / MiB), label);
            }
            if (events & EVENT_FINAL) {
                const LONG reason = InterlockedExchange(&g_finalReleaseReason, RELEASE_NONE);
                const char* label = reason == RELEASE_LOW_ADDRESS_SPACE
                    ? "emergency address space" :
                    reason == RELEASE_LARGE_REQUEST
                    ? "large allocation request" :
                    reason == RELEASE_ALLOCATION_FAILURE
                    ? "allocation failure" : "explicit recovery request";
                logger::log("MEM GUARD RELEASED : final %lu MiB VA reserve (%s)",
                            static_cast<unsigned long>(g_finalReserveSize / MiB), label);
                logger::report_event("event=reserve_release stage=final size=%luMiB reason=%s",
                                     static_cast<unsigned long>(g_finalReserveSize / MiB), label);
            }
            if (events & EVENT_RECOVERY) {
                const LONG reason = InterlockedExchange(
                    &g_recoveryReleaseReason, RELEASE_NONE);
                const LONG released_mib = InterlockedExchange(
                    &g_recoveryReleasedMiB, 0);
                const char* label = reason == RELEASE_ALLOCATION_FAILURE
                    ? "allocation failure" :
                    reason == RELEASE_LARGE_REQUEST
                    ? "large allocation request" : "explicit recovery request";
                logger::log(
                    "MEM GUARD RELEASED : recovery %ld MiB VA reserve (%s)",
                    static_cast<long>(released_mib), label);
                logger::report_event(
                    "event=reserve_release stage=recovery size=%ldMiB reason=%s",
                    static_cast<long>(released_mib), label);
            }
            if (events & EVENT_PRIMARY_REARMED) {
                logger::log("MEM GUARD REARMED : primary %lu MiB VA reserve after sustained recovery",
                            static_cast<unsigned long>(g_primaryReserveSize / MiB));
            }
            if (events & EVENT_RECOVERY_REARMED) {
                const LONG rearmed_mib = InterlockedExchange(&g_recoveryRearmedMiB, 0);
                logger::log(
                    "MEM GUARD REARMED : recovery %lu MiB VA reserve from largest remaining region",
                    static_cast<unsigned long>(rearmed_mib));
                logger::report_event(
                    "event=reserve_rearmed stage=recovery size=%luMiB",
                    static_cast<unsigned long>(rearmed_mib));
            }
            snapshot = scan_address_space();
            log_snapshot(snapshot, true);
            last_log = GetTickCount();
        }

        const DWORD log_now = GetTickCount();
        const DWORD log_interval = diagnostic_pressure ? 5000u : TELEMETRY_INTERVAL_MS;
        if (log_now - last_log >= log_interval) {
            log_snapshot(snapshot, low);
            last_log = log_now;
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
    SIZE_T critical_wanted = static_cast<SIZE_T>(CRITICAL_RESERVE_MB) * static_cast<SIZE_T>(MiB);
    void* critical = reserve_high_end(after_primary, critical_wanted);
    if (!critical) critical = VirtualAlloc(nullptr, critical_wanted, MEM_RESERVE, PAGE_NOACCESS);
    if (!critical) {
        critical_wanted /= 2;
        critical = VirtualAlloc(nullptr, critical_wanted, MEM_RESERVE, PAGE_NOACCESS);
    }

    AddressSpaceSnapshot after_critical = scan_address_space();
    SIZE_T final_wanted = static_cast<SIZE_T>(FINAL_RESERVE_MB) * static_cast<SIZE_T>(MiB);
    void* final = reserve_high_end(after_critical, final_wanted);
    if (!final) final = VirtualAlloc(nullptr, final_wanted, MEM_RESERVE, PAGE_NOACCESS);
    if (!final) {
        final_wanted /= 2;
        final = VirtualAlloc(nullptr, final_wanted, MEM_RESERVE, PAGE_NOACCESS);
    }

    AddressSpaceSnapshot after_final = scan_address_space();
    SIZE_T recovery_wanted = static_cast<SIZE_T>(RECOVERY_RESERVE_MB) *
                             static_cast<SIZE_T>(MiB);
    void* recovery = reserve_high_end(after_final, recovery_wanted);
    if (!recovery) {
        recovery = VirtualAlloc(nullptr, recovery_wanted, MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!recovery) {
        recovery_wanted /= 2;
        recovery = VirtualAlloc(nullptr, recovery_wanted, MEM_RESERVE, PAGE_NOACCESS);
    }

    g_primaryReserveSize = primary ? primary_wanted : 0;
    g_criticalReserveSize = critical ? critical_wanted : 0;
    g_finalReserveSize = final ? final_wanted : 0;
    InterlockedExchangePointer(&g_primaryReserve, primary);
    InterlockedExchangePointer(&g_criticalReserve, critical);
    InterlockedExchangePointer(&g_finalReserve, final);
    g_recovery.initialize(recovery, recovery_wanted);
    logger::log("MEM GUARD : staged reserves primary=%p size=%lu MiB critical=%p size=%lu MiB final=%p size=%lu MiB recovery=%p size=%lu MiB",
                primary, static_cast<unsigned long>(g_primaryReserveSize / MiB),
                critical, static_cast<unsigned long>(g_criticalReserveSize / MiB),
                final, static_cast<unsigned long>(g_finalReserveSize / MiB),
                recovery, static_cast<unsigned long>(recovery ? recovery_wanted / MiB : 0));
    log_snapshot(scan_address_space(), false);

    HANDLE thread = CreateThread(nullptr, 0, &monitor_thread, nullptr, 0, nullptr);
    if (!thread) {
        logger::log("MEM GUARD : monitor thread creation failed err=%lu", GetLastError());
        return primary != nullptr || critical != nullptr || final != nullptr ||
               recovery != nullptr;
    }
    CloseHandle(thread);
    return true;
}

void before_allocation(std::size_t size, std::uintptr_t caller_rva) {
    if (size < 8u * 1024u * 1024u) return;
    PreserveLastError preserve_error;

    const unsigned long long margin =
        static_cast<unsigned long long>(LARGE_ALLOCATION_MARGIN_MB) * MiB;
    const unsigned long long needed_total = static_cast<unsigned long long>(size) + margin;
    const SIZE_T max_size = static_cast<SIZE_T>(-1);
    const SIZE_T needed_largest = size > max_size - static_cast<SIZE_T>(margin)
        ? max_size
        : static_cast<SIZE_T>(size) + static_cast<SIZE_T>(margin);

    auto has_headroom = [needed_total, needed_largest](const AddressSpaceSnapshot& snapshot) {
        return snapshot.free_bytes >= needed_total &&
               snapshot.largest_free >= needed_largest;
    };

    AddressSpaceSnapshot snapshot = scan_address_space();
    if (has_headroom(snapshot)) return;

    // The reserve blocks are allocated next to one another at startup. Release
    // and re-scan one stage at a time so a large mod allocation can recover a
    // sufficiently large contiguous region before the engine aborts. The
    // final 48 MiB stage is used only when the smaller stages are insufficient.
    // The separate recovery block is deliberately not released by preflight;
    // it remains available for a later observed allocation failure.
    if (release_primary(RELEASE_LARGE_REQUEST)) {
        const AddressSpaceSnapshot after = scan_address_space();
        log_preflight_release("primary", size, caller_rva, snapshot, after);
        snapshot = after;
        if (has_headroom(snapshot)) return;
    }
    if (release_critical(RELEASE_LARGE_REQUEST)) {
        const AddressSpaceSnapshot after = scan_address_space();
        log_preflight_release("critical", size, caller_rva, snapshot, after);
        snapshot = after;
        if (has_headroom(snapshot)) return;
    }
    if (release_final(RELEASE_LARGE_REQUEST)) {
        log_preflight_release("final", size, caller_rva,
                              snapshot, scan_address_space());
    }
}

void capture_failure_snapshot(const char* api, std::size_t bytes,
                              std::uintptr_t caller_rva, const char* phase) {
    PreserveLastError preserve_error;
    const AddressSpaceSnapshot snapshot = scan_address_space();
    const unsigned long guard = guard_mib();
    const DWORD tick = GetTickCount();
    logger::log(
        "MEM FAILURE SNAPSHOT : api=%s bytes=%zu site=+%08lX phase=%s thread=%lu tick=%lu "
        "free=%lluKiB largest=%lluKiB committed=%lluMiB reserved=%lluMiB guard=%luMiB "
        "fit4/8/16/32MiB=%lu/%lu/%lu/%lu complete=%u",
        api ? api : "unknown", bytes, static_cast<unsigned long>(caller_rva),
        phase ? phase : "unknown", static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long>(tick), snapshot.free_bytes / 1024ull,
        static_cast<unsigned long long>(snapshot.largest_free) / 1024ull,
        snapshot.committed_bytes / MiB, snapshot.reserved_bytes / MiB, guard,
        static_cast<unsigned long>(snapshot.holes.counts[0]),
        static_cast<unsigned long>(snapshot.holes.counts[1]),
        static_cast<unsigned long>(snapshot.holes.counts[2]),
        static_cast<unsigned long>(snapshot.holes.counts[3]), snapshot.complete ? 1u : 0u);
    logger::report_failure_memory(
        "mem_fail phase=%.12s free=%lluMiB largest=%lluKiB guard=%luMiB holes4/8/16/32=%lu/%lu/%lu/%lu tick=%lu complete=%u",
        phase ? phase : "unknown", snapshot.free_bytes / MiB,
        static_cast<unsigned long long>(snapshot.largest_free) / 1024ull, guard,
        static_cast<unsigned long>(snapshot.holes.counts[0]),
        static_cast<unsigned long>(snapshot.holes.counts[1]),
        static_cast<unsigned long>(snapshot.holes.counts[2]),
        static_cast<unsigned long>(snapshot.holes.counts[3]),
        static_cast<unsigned long>(tick), snapshot.complete ? 1u : 0u);
}

ReleasedReserve release_after_failure() {
    if (release_primary(RELEASE_ALLOCATION_FAILURE)) {
        return {g_primaryReserveSize, false};
    }
    if (release_critical(RELEASE_ALLOCATION_FAILURE)) {
        return {g_criticalReserveSize, false};
    }
    if (release_final(RELEASE_ALLOCATION_FAILURE)) {
        return {g_finalReserveSize, false};
    }
    const SIZE_T recovery_bytes = release_recovery(RELEASE_ALLOCATION_FAILURE);
    if (recovery_bytes != 0) {
        return {recovery_bytes, true};
    }
    return {0, false};
}

void rearm_recovery_if_needed() {
    if (!g_recovery.authorize_rearm(GetCurrentThreadId())) return;
    try_rearm_recovery(scan_address_space());
}

bool is_under_pressure() {
    return InterlockedCompareExchange(&g_underPressure, 0, 0) != 0;
}

void record_request(std::size_t size) {
    InterlockedIncrement64(&g_requestCount);
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
                    std::size_t released_bytes, unsigned retry_count,
                    bool retry_succeeded) {
    logger::log("ALLOC FAILURE%s : %s requested=%zu bytes reserve_released=%s",
                retry_succeeded ? " RECOVERED" : "",
                hook_name ? hook_name : "unknown", size,
                released_bytes ? "yes" : "no");
    logger::report_event("event=alloc_failure hook=%s bytes=%zu released=%zuMiB retries=%u result=%s",
                         hook_name ? hook_name : "unknown", size,
                         released_bytes / static_cast<std::size_t>(MiB),
                         retry_count,
                         retry_succeeded ? "recovered" :
                         retry_count ? "failed" : "not_attempted");
    logger::report_failure("failure=%s bytes=%zu released=%zuMiB retries=%u result=%s",
                           hook_name ? hook_name : "unknown", size,
                           released_bytes / static_cast<std::size_t>(MiB),
                           retry_count,
                           retry_succeeded ? "ok" :
                           retry_count ? "failed" : "not_attempted");
    logger::flush_now();
}

} // namespace memory_guard
