#include "diagnostics/allocator_assessment.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>

int main() {
    using namespace diagnostics;
    assert(shape_bucket(0) == 0);
    for (unsigned i = 0; i + 1 < SHAPE_BUCKETS; ++i) {
        assert(shape_bucket(SHAPE_LIMITS[i] - 1) == i);
        assert(shape_bucket(SHAPE_LIMITS[i]) == i + 1);
    }
    assert(shape_bucket(std::numeric_limits<std::size_t>::max()) == SHAPE_BUCKETS - 1);
    assert(std::strcmp(experiment_status(nullptr), "off") == 0);
    assert(std::strcmp(experiment_status(""), "off") == 0);
    assert(std::strcmp(experiment_status("off"), "off") == 0);
    for (const char* request : {"heap-trim", "graphics-trim", "targeted-allocator", "on", "off,heap-trim"})
        assert(std::strcmp(experiment_status(request), "refused_unvalidated") == 0);
    std::puts("allocator assessment tests passed");
}
