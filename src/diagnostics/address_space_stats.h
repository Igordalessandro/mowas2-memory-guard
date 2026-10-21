#pragma once

#include <cstdint>
#include <limits>

namespace diagnostics {

// Counts free regions able to satisfy one new VirtualAlloc reservation. A
// region may begin at page alignment after another reservation is decommitted
// or split, but a new reservation still starts at allocation granularity.
struct AllocatableHoles {
    std::uint32_t counts[4] = {};

    void add(std::uint64_t base, std::uint64_t bytes,
             std::uint64_t granularity) {
        if (granularity == 0 || bytes == 0) return;
        const std::uint64_t remainder = base % granularity;
        const std::uint64_t padding = remainder ? granularity - remainder : 0;
        if (padding >= bytes || base >
            (std::numeric_limits<std::uint64_t>::max)() - padding) return;
        const std::uint64_t usable = bytes - padding;
        for (unsigned i = 0; i < 4; ++i) {
            const std::uint64_t wanted = (4ull << i) * 1024ull * 1024ull;
            if (usable >= wanted && counts[i] !=
                (std::numeric_limits<std::uint32_t>::max)()) ++counts[i];
        }
    }
};

} // namespace diagnostics
