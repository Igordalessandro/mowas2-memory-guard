#pragma once
#include <cstddef>
#include <cstring>

namespace diagnostics {
// Sampled independently of the existing full request-site accounting.
constexpr unsigned REQUEST_SHAPE_RATE = 1024;
constexpr unsigned SHAPE_BUCKETS = 12;
constexpr std::size_t SHAPE_LIMITS[SHAPE_BUCKETS - 1] = {
    16, 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216
};
inline unsigned shape_bucket(std::size_t bytes) {
    for (unsigned i = 0; i + 1 < SHAPE_BUCKETS; ++i)
        if (bytes < SHAPE_LIMITS[i]) return i;
    return SHAPE_BUCKETS - 1;
}

// No behavioral experiment has met its evidence gate. Never silently enable
// an unimplemented or unknown request, including lists of experiment names.
inline const char* experiment_status(const char* requested) {
    return !requested || !requested[0] || std::strcmp(requested, "off") == 0
        ? "off" : "refused_unvalidated";
}
} // namespace diagnostics
