#pragma once

#include <cstddef>

namespace memory_guard {

// Reserves emergency virtual address space and starts periodic telemetry.
bool install();

// Cheap for ordinary allocations.  Large requests receive an immediate
// address-space check so the reserve can be released before calling the game.
void before_allocation(std::size_t size);

// Returns true only for the thread that actually released the reserve.
bool release_after_failure();

void record_request(std::size_t size);
void record_failure(std::size_t size, const char* hook_name, bool retry_succeeded);

} // namespace memory_guard
