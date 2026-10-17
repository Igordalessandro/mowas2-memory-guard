#pragma once

#include <cstddef>
#include <cstdint>

namespace allocator_profiler {

// Installs observation/recovery hooks in mowas_2.exe's own allocation IAT.
// The original Windows allocators remain authoritative.
bool install(std::uintptr_t game_base);

// Records a sparse sample of the game's high-frequency allocator requests.
// This complements the lower-level IAT hooks, which primarily see pool growth.
void record_engine_request(std::size_t size, std::uintptr_t caller);

// Carries the higher-level engine caller through the low-level HeapAlloc hook
// so retained samples are attributed to gameplay code rather than the common
// heap wrapper.  Calls must be balanced on the same thread.
void begin_engine_request(std::size_t size, std::uintptr_t caller);
void end_engine_request(void* result, bool initial_failure, bool recovered);

// Writes bounded aggregate telemetry. Detailed output includes the busiest
// allocation sites, sampled retained allocations, and common free sites.
void log_summary(bool detailed);

// Writes the newest entries from the fixed, allocation-free flight recorder.
void log_flight_recorder();

} // namespace allocator_profiler
