#pragma once

#include <cstddef>
#include <cstdint>

namespace allocator_profiler {

constexpr bool is_resource_exhaustion_error(std::uint32_t error) {
    return error == 8 || error == 14 || error == 1450 ||
           error == 1455 || error == 1816;
}

// Emergency VA reserves can help a new reservation, but cannot repair an
// invalid parameter, MEM_RESET, or a failed commit inside an existing range.
// Keep this decision independent of Windows headers so it can be tested on
// the build host as well as in the Windows regression executable.
constexpr bool may_retry_virtual_allocation(std::uintptr_t address,
                                            std::size_t size,
                                            std::uint32_t type,
                                            std::uint32_t error) {
    constexpr std::uint32_t reserve = 0x2000;
    constexpr std::uint32_t commit = 0x1000;
    constexpr std::uint32_t top_down = 0x100000;
    constexpr std::uint32_t write_watch = 0x200000;
    constexpr std::uint32_t supported = reserve | commit | top_down | write_watch;
    constexpr std::uint32_t not_enough_memory = 8;
    constexpr std::uint32_t out_of_memory = 14;
    // Windows permits MEM_COMMIT with a null address to allocate a new range.
    const bool new_range = (type & reserve) || (address == 0 && (type & commit));
    return size != 0 && new_range && (type & ~supported) == 0 &&
           (error == not_enough_memory || error == out_of_memory);
}

} // namespace allocator_profiler
