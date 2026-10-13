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

// Writes bounded aggregate telemetry. Detailed output includes the busiest
// allocation sites, sampled retained allocations, and common free sites.
void log_summary(bool detailed);

} // namespace allocator_profiler
