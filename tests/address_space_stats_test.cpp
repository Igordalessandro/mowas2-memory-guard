#include "diagnostics/address_space_stats.h"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    constexpr std::uint64_t MiB = 1024ull * 1024ull;
    diagnostics::AllocatableHoles holes;
    holes.add(0x10000, 32 * MiB, 0x10000);
    for (unsigned i = 0; i < 4; ++i) assert(holes.counts[i] == 1);

    // A nominal 4 MiB hole beginning one page past allocation granularity
    // cannot hold a new 4 MiB reservation. Restoring the alignment padding can.
    holes.add(0x11000, 4 * MiB, 0x10000);
    assert(holes.counts[0] == 1);
    holes.add(0x11000, 4 * MiB + 0xf000, 0x10000);
    assert(holes.counts[0] == 2 && holes.counts[1] == 1);

    holes.add(0x20000, 16 * MiB - 1, 0x10000);
    assert(holes.counts[0] == 3 && holes.counts[1] == 2);
    assert(holes.counts[2] == 1 && holes.counts[3] == 1);

    // Invalid granularity, tiny regions, and alignment overflow must not
    // invent usable holes. The helper also works without power-of-two input.
    holes.add(0, 64 * MiB, 0);
    holes.add(1, 1, 0x10000);
    holes.add(std::numeric_limits<std::uint64_t>::max(), 64 * MiB, 0x10000);
    assert(holes.counts[0] == 3);
    diagnostics::AllocatableHoles non_power;
    non_power.add(1, 4 * MiB + 2, 3);
    assert(non_power.counts[0] == 1);

    // The final hole of a 4 GiB x86 address space remains measurable using
    // wide arithmetic, including its exact 32 MiB fit threshold.
    diagnostics::AllocatableHoles high;
    high.add(0xfe000000ull, 32 * MiB, 0x10000);
    assert(high.counts[3] == 1);
}
