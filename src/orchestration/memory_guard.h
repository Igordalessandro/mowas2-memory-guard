#pragma once

#include <cstddef>
#include <cstdint>

namespace memory_guard {

// Reserves emergency virtual address space and starts periodic telemetry.
bool install();

// Cheap for ordinary allocations.  Large requests receive an immediate
// address-space check so the reserve can be released before calling the game.
void before_allocation(std::size_t size,
                       std::uintptr_t caller_rva = 0xffffffffu);

// Performs a fresh address-space scan only on observed allocation failures.
// The compact failure field is separate from periodic monitor snapshots. This
// function preserves the caller's GetLastError value.
void capture_failure_snapshot(const char* api, std::size_t bytes,
                              std::uintptr_t caller_rva, const char* phase);

struct ReleasedReserve {
    std::size_t bytes;
    bool recovery_stage;
};

// Releases at most one reserve stage. Callers may retry once per released
// stage instead of exposing every remaining block at once. Recovery-reserve
// ownership and re-arm transitions are serialized across allocating threads.
ReleasedReserve release_after_failure();

// If the failure-only reserve was consumed by a successful retry, attempt to
// protect another bounded piece of the largest remaining free region. This is
// cheap when no re-arm is pending; the monitor thread also retries it.
void rearm_recovery_if_needed();

// Cheap pressure flag maintained by the monitor thread.  Diagnostics use it
// to increase sampling without rescanning the address space on hot paths.
bool is_under_pressure();

void record_request(std::size_t size);
void record_failure(std::size_t size, const char* hook_name,
                    std::size_t released_bytes, unsigned retry_count,
                    bool retry_succeeded);

} // namespace memory_guard
