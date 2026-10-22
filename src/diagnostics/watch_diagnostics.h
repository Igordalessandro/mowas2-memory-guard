#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace diagnostics {
// Observation filters only, not hook/patch targets or semantic function names.
constexpr std::uint32_t WATCH_SITES[] = {
    0x003749C9u, 0x0032762Du, 0x0004105Du, 0x002A89C1u, 0x00374A97u
};
constexpr unsigned WATCH_SITE_COUNT = sizeof(WATCH_SITES) / sizeof(WATCH_SITES[0]);
constexpr unsigned TRACE_FRAMES = 12;
constexpr unsigned FAILURE_TRACES = 8;
constexpr unsigned TRACE_CAPACITY = WATCH_SITE_COUNT + FAILURE_TRACES;
constexpr std::size_t WATCH_TRACE_MIN_BYTES = 64u * 1024u;
constexpr std::uint32_t CODE_BEFORE = 16;
constexpr std::uint32_t CODE_BYTES = 64;

inline unsigned watch_index(std::uint32_t rva) {
    for (unsigned i = 0; i < WATCH_SITE_COUNT; ++i)
        if (WATCH_SITES[i] == rva) return i;
    return WATCH_SITE_COUNT;
}

struct AgeTotals {
    std::uint64_t bytes = 0;
    std::uint64_t age_bytes[4]{};
    std::uint32_t blocks = 0;
    std::uint32_t oldest_ms = 0;
    void add(std::uint32_t size, std::uint32_t allocated_tick, std::uint32_t now) {
        const std::uint32_t age = now - allocated_tick;
        const unsigned bucket = age < 1000 ? 0u : age < 10000 ? 1u : age < 60000 ? 2u : 3u;
        bytes += size;
        age_bytes[bucket] += size;
        ++blocks;
        if (age > oldest_ms) oldest_ms = age;
    }
};

inline bool image_rva(std::uintptr_t base, std::uint32_t image_size,
                      std::uintptr_t address, std::uint32_t& output) {
    if (address < base || address - base >= image_size) return false;
    output = static_cast<std::uint32_t>(address - base);
    return true;
}

// Only a bounded module-code window; never allocation payloads. The reader
// must report a complete copy. Bounds are checked before any reader call.
template<class Reader>
bool read_code_window(std::uintptr_t base, std::uint32_t image_size,
                      std::uint32_t rva, Reader reader, unsigned char* output) {
    if (rva < CODE_BEFORE || image_size < CODE_BYTES) return false;
    const std::uint32_t start = rva - CODE_BEFORE;
    if (start > image_size - CODE_BYTES ||
        base > std::numeric_limits<std::uintptr_t>::max() - start ||
        base + start > std::numeric_limits<std::uintptr_t>::max() - (CODE_BYTES - 1u))
        return false;
    return reader(base + start, output, CODE_BYTES);
}
} // namespace diagnostics
